#pragma once

#include <base/types.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>


namespace DB
{

/// The writers of a Set or Join table, counted from the creation of a sink to its destruction: a sink inserts into the state of the table
/// while it consumes, publishes its file when it finishes, and may start on one thread and end on another. The registration of the stored
/// binary type origin of the table closes the gate: no new writer enters, and it waits until every writer has left, so that none inserts
/// into the state or publishes a file while the files are read again, and none started before publishes after the new state. A writer holds
/// a token, not a lock, so it may leave on any thread. Both wait as for the locks of a table (lock_acquire_timeout): a zero timeout waits
/// without a limit, and a timeout that expires throws DEADLOCK_AVOIDED.
class SetOrJoinWritersGate
{
public:
    class Token
    {
    public:
        explicit Token(SetOrJoinWritersGate & gate_) : gate(gate_) {}
        ~Token();

        Token(const Token &) = delete;
        Token & operator=(const Token &) = delete;

    private:
        SetOrJoinWritersGate & gate;
    };
    using TokenPtr = std::unique_ptr<Token>;

    /// Waits up to `timeout` while the gate is closed, then counts a writer until the token is destroyed. Throws DEADLOCK_AVOIDED.
    TokenPtr enter(std::chrono::milliseconds timeout, const String & table_name);

    /// Keeps new writers out and waits up to `timeout` until every writer has left. Throws DEADLOCK_AVOIDED, with the gate open again.
    void close(std::chrono::milliseconds timeout, const String & table_name);

    /// Lets writers in again.
    void open() noexcept;

private:
    void leave() noexcept;

    std::mutex mutex;
    std::condition_variable changed;
    size_t writers = 0;
    bool closed = false;
};

}
