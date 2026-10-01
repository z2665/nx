#include "pipes.hpp"
#include "diag.hpp"
#include <atomic>

namespace nx {

size_t ThreadPool::defaultWorkers() {
    size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    return std::max<size_t>(2, std::min<size_t>(8, cores / 2));
}

ThreadPool::ThreadPool(size_t n)
    : workers_(n) {}   // 惰性：首个 submit 才起线程（降低无异步任务的启动成本）

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    // S4：join 后队列必空、无在跑任务（worker 只在 stop&&空 时退出）——
    // 若未来改动退出逻辑破坏此不变式，哨兵在此 abort
    diag::assert_true(q_.empty() && running_ == 0,
                      "S4 ~ThreadPool：join 后队列非空或任务仍在执行");
}

void ThreadPool::submit(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(Task{std::move(f)});
    }
    while (started_ < workers_) {   // 惰性启动
        threads_.emplace_back([this](std::stop_token st) { worker(st); });
        ++started_;
    }
    cv_.notify_all();
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
