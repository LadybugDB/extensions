#include "prefetch_pool.h"

#include <algorithm>

namespace lbug {
namespace httpfs_extension {

PrefetchPool::PrefetchPool(size_t numThreads) {
    workers.reserve(numThreads);
    for (auto i = 0u; i < numThreads; ++i) {
        workers.emplace_back([this]() { workerLoop(); });
    }
}

PrefetchPool::~PrefetchPool() {
    {
        std::lock_guard<std::mutex> lck{mtx};
        stopping = true;
    }
    cv.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void PrefetchPool::submit(std::function<void()> job) {
    trySubmit(std::move(job));
}

bool PrefetchPool::trySubmit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lck{mtx};
        // Never queue behind a backlog: prefetching must not grow unboundedly
        // when the consumer is slower than the producers. Keep at most one
        // outstanding queued job per worker; excess is dropped (the next block
        // boundary re-submits anyway).
        const auto maxQueued = std::max<size_t>(workers.size(), 1);
        if (jobs.size() >= maxQueued) {
            return false;
        }
        jobs.push_back(std::move(job));
    }
    cv.notify_one();
    return true;
}

void PrefetchPool::workerLoop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lck{mtx};
            cv.wait(lck, [this]() { return stopping || !jobs.empty(); });
            if (stopping && jobs.empty()) {
                return;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        try {
            job();
        } catch (...) {
            // Speculative work: never propagate failures to the pool thread.
        }
    }
}

} // namespace httpfs_extension
} // namespace lbug
