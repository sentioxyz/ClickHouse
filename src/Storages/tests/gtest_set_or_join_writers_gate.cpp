#include <gtest/gtest.h>

#include <Storages/SetOrJoinWritersGate.h>
#include <Common/Exception.h>

#include <chrono>
#include <functional>
#include <limits>
#include <optional>
#include <thread>

/// The gate of the writers of a Set or Join table (SetOrJoinWritersGate): the registration of the stored binary type origin closes it and
/// proceeds only once every writer has left, new writers wait until it opens again, and a writer may leave on another thread than the one
/// it entered on. The timeouts are those of the locks of a table: zero waits without a limit, an expired one throws DEADLOCK_AVOIDED, a
/// negative one has expired already, and a huge one is clamped as RWLockImpl clamps it (saturatedMilliseconds), so that it cannot overflow.
/// Every case is decided by the state of the gate, not by timing: a short wait expires only when no other thread can change the state
/// during it, and a thread that waits is joined.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int DEADLOCK_AVOIDED;
extern const int LOGICAL_ERROR;
}

namespace
{

constexpr std::chrono::milliseconds SHORT_WAIT{1};
constexpr std::chrono::milliseconds LONG_WAIT{60000};
constexpr std::chrono::milliseconds WITHOUT_LIMIT{0};
constexpr std::chrono::milliseconds EXPIRED{-1};
constexpr std::chrono::milliseconds HUGE_WAIT{std::numeric_limits<std::chrono::milliseconds::rep>::max()};

int codeOf(const std::function<void()> & action)
{
    try
    {
        action();
    }
    catch (const Exception & e)
    {
        return e.code();
    }
    return 0;
}

}

TEST(SetOrJoinWritersGate, CloseRefusesWhileAWriterIsInAndReopens)
{
    SetOrJoinWritersGate gate;
    auto token = gate.enter(SHORT_WAIT, "t");

    EXPECT_EQ(codeOf([&] { gate.close(SHORT_WAIT, "t"); }), ErrorCodes::DEADLOCK_AVOIDED);
    /// The refused close leaves the gate open.
    auto second = gate.enter(SHORT_WAIT, "t");

    token.reset();
    second.reset();
    gate.close(SHORT_WAIT, "t");
    gate.open();
}

TEST(SetOrJoinWritersGate, WritersWaitWhileClosed)
{
    SetOrJoinWritersGate gate;
    gate.close(SHORT_WAIT, "t");
    EXPECT_EQ(codeOf([&] { gate.enter(SHORT_WAIT, "t"); }), ErrorCodes::DEADLOCK_AVOIDED);
    EXPECT_EQ(codeOf([&] { gate.close(SHORT_WAIT, "t"); }), ErrorCodes::LOGICAL_ERROR);
    gate.open();
    auto token = gate.enter(SHORT_WAIT, "t");
}

TEST(SetOrJoinWritersGate, WriterLeavesOnAnotherThread)
{
    SetOrJoinWritersGate gate;
    auto token = gate.enter(SHORT_WAIT, "t");
    std::thread other([moved = std::move(token)]() mutable { moved.reset(); });
    other.join();
    gate.close(SHORT_WAIT, "t");
    gate.open();
}

/// A close that waits returns once the writer leaves, whichever comes first: its wait or the leave.
TEST(SetOrJoinWritersGate, CloseReturnsWhenTheWriterLeaves)
{
    SetOrJoinWritersGate gate;
    auto token = gate.enter(SHORT_WAIT, "t");
    std::optional<int> close_code;
    std::thread closer([&] { close_code = codeOf([&] { gate.close(LONG_WAIT, "t"); }); });
    token.reset();
    closer.join();
    ASSERT_TRUE(close_code.has_value());
    EXPECT_EQ(*close_code, 0);

    /// Closed now: a new writer waits, and enters once it opens, whichever comes first.
    std::optional<int> enter_code;
    std::thread writer([&] { enter_code = codeOf([&] { gate.enter(LONG_WAIT, "t"); }); });
    gate.open();
    writer.join();
    ASSERT_TRUE(enter_code.has_value());
    EXPECT_EQ(*enter_code, 0);
}

/// A zero timeout waits without a limit, as lock_acquire_timeout = 0 does for the locks of a table: the close returns once the writer
/// leaves, and the writer enters once the gate opens.
TEST(SetOrJoinWritersGate, ZeroTimeoutWaitsWithoutALimit)
{
    SetOrJoinWritersGate gate;
    auto token = gate.enter(WITHOUT_LIMIT, "t");
    std::optional<int> close_code;
    std::thread closer([&] { close_code = codeOf([&] { gate.close(WITHOUT_LIMIT, "t"); }); });
    token.reset();
    closer.join();
    ASSERT_TRUE(close_code.has_value());
    EXPECT_EQ(*close_code, 0);

    std::optional<int> enter_code;
    std::thread writer([&] { enter_code = codeOf([&] { gate.enter(WITHOUT_LIMIT, "t"); }); });
    gate.open();
    writer.join();
    ASSERT_TRUE(enter_code.has_value());
    EXPECT_EQ(*enter_code, 0);
}

/// A negative timeout has expired already: the close and the entry that would have to wait are refused at once, and those that need not
/// wait succeed.
TEST(SetOrJoinWritersGate, NegativeTimeoutHasExpired)
{
    SetOrJoinWritersGate gate;
    auto token = gate.enter(EXPIRED, "t");
    EXPECT_EQ(codeOf([&] { gate.close(EXPIRED, "t"); }), ErrorCodes::DEADLOCK_AVOIDED);
    token.reset();
    gate.close(EXPIRED, "t");
    EXPECT_EQ(codeOf([&] { gate.enter(EXPIRED, "t"); }), ErrorCodes::DEADLOCK_AVOIDED);
    gate.open();
}

/// A huge timeout, whose nanoseconds do not fit Int64, waits until another thread releases the gate: the close returns once the writer
/// leaves, and the writer enters once the gate opens, whichever comes first.
TEST(SetOrJoinWritersGate, HugeTimeoutWaitsUntilReleased)
{
    SetOrJoinWritersGate gate;
    auto token = gate.enter(HUGE_WAIT, "t");
    std::optional<int> close_code;
    std::thread closer([&] { close_code = codeOf([&] { gate.close(HUGE_WAIT, "t"); }); });
    token.reset();
    closer.join();
    ASSERT_TRUE(close_code.has_value());
    EXPECT_EQ(*close_code, 0);

    std::optional<int> enter_code;
    std::thread writer([&] { enter_code = codeOf([&] { gate.enter(HUGE_WAIT, "t"); }); });
    gate.open();
    writer.join();
    ASSERT_TRUE(enter_code.has_value());
    EXPECT_EQ(*enter_code, 0);
}
