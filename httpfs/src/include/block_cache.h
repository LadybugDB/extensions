#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace lbug {
namespace httpfs_extension {

/**
 * Persistent (cross-run) cache of fetched byte ranges for remote immutable
 * datasets, e.g. xet:// content-addressed objects. Entries are keyed by
 * SHA256(remote URL) plus the exact (offset, length) span that was fetched and
 * stored as individual files under
 *      <ext-local-dir>/.block_cache/<url-hash>/<offset>-<length>
 * Because spans are immutable once written, a hit is served without freshness
 * checks. The directory is pruned least-recently-modified-first whenever its
 * total size exceeds the configured budget.
 *
 * Purely a local-file cache: must only be fed with spans whose contents are
 * guaranteed immutable for the keying URL (content-addressed/xet objects).
 */
class PersistentBlockCache {
public:
    void init(const std::string& rootDir, uint64_t maxTotalBytes);

    bool enabled() const { return initialized; }

    /** Attempts to fill `buffer` with the cached span. Returns false on miss. */
    bool get(const std::string& url, uint64_t offset, uint64_t length, char* buffer);

    /** Stores a freshly fetched span (write-through). Failures are ignored. */
    void put(const std::string& url, uint64_t offset, uint64_t length, const char* data);

private:
    void prune();

    bool initialized = false;
    std::string rootDir;
    uint64_t maxTotalBytes = 0;
    std::atomic<uint64_t> bytesSincePrune{0};
    mutable std::mutex mtx; // serializes initialization/pruning
};

} // namespace httpfs_extension
} // namespace lbug
