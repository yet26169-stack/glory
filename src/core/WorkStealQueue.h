#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>

namespace glory {

/// Lock-based work-stealing queue for multi-threaded command recording.
/// For lower overhead, replace with enkiTS (see Appendix C of the AAA plan).
class WorkStealQueue {
public:
    using Task = std::function<void()>;

    void push(Task t);
    /// Try to pop from own queue (LIFO for cache locality).
    bool tryPop(Task& out);
    /// Try to steal from another thread's queue (FIFO).
    bool trySteal(Task& out);

private:
    std::deque<Task>   m_queue;
    mutable std::mutex m_mutex;
};

} // namespace glory
