#include <Storages/SetOrJoinWritersGate.h>

#include <Common/Exception.h>
#include <Common/saturatedDuration.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int DEADLOCK_AVOIDED;
    extern const int LOGICAL_ERROR;
}

namespace
{

/// Waits until `predicate` holds, up to `timeout`, or without a limit for a zero timeout, as RWLockImpl does: a nonzero timeout is clamped
/// (saturatedMilliseconds), so that a negative one has expired already and a huge one cannot overflow the deadline of wait_for.
template <typename Predicate>
bool waitUntil(std::condition_variable & changed, std::unique_lock<std::mutex> & lock, std::chrono::milliseconds timeout, Predicate predicate)
{
    if (timeout == std::chrono::milliseconds(0))
    {
        changed.wait(lock, predicate);
        return true;
    }
    return changed.wait_for(lock, saturatedMilliseconds(timeout.count()), predicate);
}

}

SetOrJoinWritersGate::Token::~Token()
{
    gate.leave();
}

SetOrJoinWritersGate::TokenPtr SetOrJoinWritersGate::enter(std::chrono::milliseconds timeout, const String & table_name)
{
    std::unique_lock lock(mutex);
    if (!waitUntil(changed, lock, timeout, [this] { return !closed; }))
        throw Exception(ErrorCodes::DEADLOCK_AVOIDED,
                        "Cannot insert into table {}: the registration of its stored binary type origin did not finish in {} ms. Client "
                        "should retry", table_name, timeout.count());

    /// Allocated before the writer is counted, so that a failure counts nothing.
    auto token = std::make_unique<Token>(*this);
    ++writers;
    return token;
}

void SetOrJoinWritersGate::close(std::chrono::milliseconds timeout, const String & table_name)
{
    std::unique_lock lock(mutex);
    if (closed)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "The gate of the writers of table {} is closed already", table_name);

    closed = true;
    if (!waitUntil(changed, lock, timeout, [this] { return writers == 0; }))
    {
        closed = false;
        const size_t remaining = writers;
        lock.unlock();
        changed.notify_all();
        throw Exception(ErrorCodes::DEADLOCK_AVOIDED,
                        "Cannot register the stored binary type origin of table {}: {} inserts into it did not finish in {} ms. Client "
                        "should retry", table_name, remaining, timeout.count());
    }
}

void SetOrJoinWritersGate::open() noexcept
{
    {
        std::lock_guard lock(mutex);
        closed = false;
    }
    changed.notify_all();
}

void SetOrJoinWritersGate::leave() noexcept
{
    {
        std::lock_guard lock(mutex);
        --writers;
    }
    changed.notify_all();
}

}
