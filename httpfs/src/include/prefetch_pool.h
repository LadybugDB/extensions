#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lbug {
namespace httpfs_extension {

/**
 * Minimal fixed-size worker pool used to issue speculative read-ahead fetches
 * for remote files concurrently with the calling scan thread. On destruction
 * the queue is drained and all workers joined, so jobs can safely capture raw
 * pointers to members of the owner whose destructor runs after the pool's.
 */
class PrefetchPool {
public:
    explicit PrefetchPool(size_t numThreads);
    ~PrefetchPool();

    PrefetchPool(const PrefetchPool&) = delete;
    PrefetchPool& operator=(const PrefetchPool&) = delete;

    void submit(std::function<void()> job);
    /**
     * Returns false if the pool's queue was at capacity and the job was
     * discarded (callers relying on completion bookkeeping must clean up).
     */
    bool trySubmit(std::function<void()> job);

private:
    void workerLoop();

    std::vector<std::thread> workers;
    std::deque<std::function<void()>> jobs;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopping = false;
};

} // namespace httpfs_extension
} // namespace lbug
