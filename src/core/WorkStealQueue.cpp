#include "core/WorkStealQueue.h"

namespace glory {

void WorkStealQueue::push(Task t) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_queue.push_back(std::move(t));
}

bool WorkStealQueue::tryPop(Task& out) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_queue.empty()) return false;
    out = std::move(m_queue.back());
    m_queue.pop_back();
    return true;
}

bool WorkStealQueue::trySteal(Task& out) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

} // namespace glory
