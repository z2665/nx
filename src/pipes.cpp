#include "pipes.hpp"
#include <atomic>

namespace nx {

size_t ThreadPool::defaultWorkers() {
    size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    return std::max<size_t>(2, std::min<size_t>(8, cores / 2));
}

ThreadPool::ThreadPool(size_t n) {
    for (size_t i = 0; i < n; ++i)
        threads_.emplace_back([this](std::stop_token st) { worker(st); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
}

void ThreadPool::submit(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(Task{std::move(f), nullptr});
    }
    cv_.notify_one();
}

void ThreadPool::worker(std::stop_token st) {
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (q_.empty()) {
                if (stop_) return;
                continue;
            }
            t = std::move(q_.front());
            q_.pop_front();
            ++running_;
        }
        try {
            if (t.fn) t.fn();
        } catch (...) {
            std::lock_guard<std::mutex> lk(m_);
            if (!firstErr_) firstErr_ = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            --running_;
        }
        cv_.notify_all();
    }
}

void ThreadPool::waitAll() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&] { return q_.empty() && running_ == 0; });
    if (firstErr_) std::rethrow_exception(*firstErr_);
}

} // namespace nx
