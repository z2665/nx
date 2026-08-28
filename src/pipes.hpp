// pipes.hpp：有界队列（背压）+ 线程池 + 解码泵（设计 D4/D5）
#pragma once
#include "util.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace nx {

// 固定容量 MPSC 有界队列：满则 push 阻塞（背压）；abandon 后两侧立即解阻塞
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t cap) : cap_(cap ? cap : 1) {}

    bool push(T v) {
        std::unique_lock<std::mutex> lk(m_);
        while (q_.size() >= cap_ && !dead_) fullCv_.wait(lk);
        if (dead_) return false;
        q_.push_back(std::move(v));
        emptyCv_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_);
        while (q_.empty() && !closed_ && !dead_) emptyCv_.wait(lk);
        if (dead_) return std::nullopt;
        if (q_.empty()) return std::nullopt;   // closed 且排空
        T v = std::move(q_.front());
        q_.pop_front();
        fullCv_.notify_one();
        return v;
    }

    void close() {   // 生产者结束
        std::lock_guard<std::mutex> lk(m_);
        closed_ = true;
        emptyCv_.notify_all();
    }

    void abandon() { // 消费者消失：唤醒生产者并使其 push 失败
        std::lock_guard<std::mutex> lk(m_);
        dead_ = true;
        q_.clear();
        fullCv_.notify_all();
        emptyCv_.notify_all();
    }

private:
    size_t cap_;
    std::mutex m_;
    std::condition_variable fullCv_, emptyCv_;
    std::deque<T> q_;
    bool closed_ = false;
    bool dead_ = false;
};

// 简单固定线程池（Sink 写出 / 后续扩展）
class ThreadPool {
public:
    explicit ThreadPool(size_t n);
    ~ThreadPool();
    void submit(std::function<void()> f);
    void waitAll();   // 等待已提交任务全部完成
    static size_t defaultWorkers();   // min(8, cores/2)，至少 2
private:
    struct Task {
        std::function<void()> fn;
        std::shared_ptr<std::exception_ptr> err;   // 首个异常传播
    };
    void worker(std::stop_token st);
    std::vector<std::jthread> threads_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Task> q_;
    size_t running_ = 0;
    std::optional<std::exception_ptr> firstErr_;
    bool stop_ = false;
};

} // namespace nx
