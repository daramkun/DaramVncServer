#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "util/ThreadCpu.h"
// Small persistent thread pool for data-parallel loops (e.g. JPEG-compressing independent rectangles). Creating
// threads for every frame costs CPU; this keeps them parked between calls. The calling thread takes part in the
// work. If another caller is already using the pool, parallelFor() just runs serially instead of waiting.
class WorkerPool {
public:
    static WorkerPool& instance() {
        static WorkerPool pool;
        return pool;
    }

    void parallelFor(size_t n, std::function<void(size_t)> fn) {
        if (n <= 1 || threads_.empty() || !callMu_.try_lock()) {
            for (size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        std::lock_guard<std::mutex> guard(callMu_, std::adopt_lock);
        auto run = std::make_shared<Run>();
        run->fn = std::move(fn);
        run->total = n;
        {
            std::lock_guard<std::mutex> lk(m_);
            run_ = run;
            ++generation_;
        }
        cv_.notify_all();
        work(*run);  // the caller helps
        std::unique_lock<std::mutex> lk(m_);
        doneCv_.wait(lk, [&] { return run->done.load() == run->total; });
    }

    size_t workerCount() const { return threads_.size(); }

private:
    struct Run {
        std::function<void(size_t)> fn;
        size_t total = 0;
        std::atomic<size_t> next{0}, done{0};
    };

    WorkerPool() {
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned n = hw > 2 ? std::min(3u, hw - 1) : 0;
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { loop(); });
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }

    void work(Run& r) {
        for (;;) {
            const size_t i = r.next.fetch_add(1);
            if (i >= r.total) break;
            r.fn(i);
            if (r.done.fetch_add(1) + 1 == r.total) {
                std::lock_guard<std::mutex> lk(m_);
                doneCv_.notify_all();
            }
        }
    }

    void loop() {
        threadcpu::Scope cpuScope("jpeg-worker");
        uint64_t seen = 0;
        for (;;) {
            std::shared_ptr<Run> run;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                run = run_;
            }
            work(*run);  // a late waker finds next >= total and returns without touching fn
        }
    }

    std::mutex m_, callMu_;
    std::condition_variable cv_, doneCv_;
    std::shared_ptr<Run> run_;
    uint64_t generation_ = 0;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};
