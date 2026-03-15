#pragma once
#include <functional>
#include <future>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>
#include <atomic>

namespace glory {

class ThreadPool {
public:
    explicit ThreadPool(uint32_t threadCount = 0) {
        if (threadCount == 0) {
            threadCount = std::max(1u, std::thread::hardware_concurrency() - 1);
        }
        for (uint32_t i = 0; i < threadCount; ++i) {
            m_workers.emplace_back([this](std::stop_token st) { workerLoop(st); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        // jthread destructor requests stop and joins automatically
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<typename F>
    std::future<void> submit(F&& task) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        {
            std::lock_guard lock(m_mutex);
            m_pendingCount.fetch_add(1, std::memory_order_relaxed);
            m_tasks.emplace([p = std::move(promise), t = std::forward<F>(task)]() mutable {
                try {
                    t();
                    p->set_value();
                } catch (...) {
                    p->set_exception(std::current_exception());
                }
            });
        }
        m_cv.notify_one();
        return future;
    }

    void waitAll() {
        std::unique_lock lock(m_mutex);
        m_allDone.wait(lock, [this] {
            return m_pendingCount.load(std::memory_order_relaxed) == 0;
        });
    }

    uint32_t workerCount() const { return static_cast<uint32_t>(m_workers.size()); }

private:
    void workerLoop(std::stop_token st) {
        while (!st.stop_requested()) {
            std::function<void()> task;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this, &st] {
                    return !m_tasks.empty() || m_stopping || st.stop_requested();
                });
                if ((m_stopping || st.stop_requested()) && m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
            if (m_pendingCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                m_allDone.notify_all();
            }
        }
    }

    std::vector<std::jthread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_allDone;
    std::atomic<uint32_t> m_pendingCount{0};
    bool m_stopping = false;
};

} // namespace glory
