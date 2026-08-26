#include "block_cache.h"

#include "crypto.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

namespace lbug {
namespace httpfs_extension {
namespace fs = std::filesystem;

namespace {

std::string hashURL(const std::string& url) {
    hash_bytes bytes{};
    sha256(url.data(), url.size(), bytes);
    hash_str hex{};
    hex256(bytes, hex);
    return {reinterpret_cast<const char*>(hex), 64};
}

// Rename that tolerates racing writers losing the final say.
void atomicStore(const fs::path& target, const char* data, size_t length) {
    static std::atomic<uint64_t> tmpCounter{0};
    auto tmp = target;
    tmp += ".tmp." + std::to_string(tmpCounter.fetch_add(1));
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        out.exceptions(std::ostream::failbit | std::ostream::badbit);
        out.write(data, static_cast<std::streamsize>(length));
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // Windows cannot rename onto an existing file: last writer simply drops.
        fs::remove(tmp, ec);
        std::error_code ignore;
        if (!fs::exists(target)) {
            fs::rename(tmp, target, ignore);
        }
    }
}

} // namespace

void PersistentBlockCache::init(const std::string& rootDirIn, uint64_t maxTotalBytesIn) {
    std::lock_guard<std::mutex> lck{mtx};
    if (initialized) {
        return;
    }
    try {
        fs::create_directories(rootDirIn);
        rootDir = rootDirIn;
        maxTotalBytes = maxTotalBytesIn;
        initialized = true;
        prune();
    } catch (...) {
        // A broken cache directory must never break correctness: fall back to
        // an in-memory-only run.
        initialized = false;
    }
}

bool PersistentBlockCache::get(const std::string& url, uint64_t offset, uint64_t length,
    char* buffer) {
    if (!initialized) {
        return false;
    }
    auto filePath =
        fs::path(rootDir) / hashURL(url) / std::format("{}-{}", offset, length);
    std::error_code ec;
    const auto size = fs::file_size(filePath, ec);
    if (ec || size != length) {
        return false;
    }
    try {
        std::ifstream in{filePath, std::ios::binary};
        if (!in) {
            return false;
        }
        in.read(buffer, static_cast<std::streamsize>(length));
        if (in.gcount() != static_cast<std::streamsize>(length)) {
            return false;
        }
        // Touch mtime so the LRU-pruning policy sees recency. Best effort.
        fs::last_write_time(filePath,
            std::filesystem::file_time_type::clock::now(), ec);
    } catch (...) {
        return false;
    }
    return true;
}

void PersistentBlockCache::put(const std::string& url, uint64_t offset, uint64_t length,
    const char* data) {
    if (!initialized) {
        return;
    }
    try {
        auto dir = fs::path(rootDir) / hashURL(url);
        fs::create_directories(dir);
        auto filePath = dir / std::format("{}-{}", offset, length);
        std::error_code ec;
        if (fs::exists(filePath)) {
            return; // immutable span already present
        }
        atomicStore(filePath, data, length);
        if (bytesSincePrune.fetch_add(length) > (128u << 20)) {
            prune();
        }
    } catch (...) {
        // Cache writes are best-effort.
    }
}

void PersistentBlockCache::prune() {
    bytesSincePrune.store(0);
    if (maxTotalBytes == 0) {
        return;
    }
    struct Entry {
        fs::path path;
        uintmax_t size;
        fs::file_time_type mtime;
    };
    std::vector<Entry> entries;
    uint64_t totalSize = 0;
    try {
        std::error_code ec;
        for (const auto& dirEntry : fs::directory_iterator(fs::path(rootDir), ec)) {
            if (!dirEntry.is_directory()) {
                continue;
            }
            for (const auto& fileEntry : fs::directory_iterator(dirEntry.path())) {
                if (!fileEntry.is_regular_file()) {
                    continue;
                }
                auto entryPath = fileEntry.path();
                auto size = fs::file_size(entryPath, ec);
                if (ec) {
                    continue;
                }
                auto mtime = fs::last_write_time(entryPath, ec);
                entries.push_back({entryPath, size, mtime});
                totalSize += size;
            }
        }
    } catch (...) {
        return;
    }
    if (totalSize <= maxTotalBytes) {
        return;
    }
    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
    for (auto& entry : entries) {
        if (totalSize <= maxTotalBytes) {
            break;
        }
        std::error_code ec;
        auto removed = fs::file_size(entry.path, ec);
        fs::remove(entry.path, ec);
        if (!ec && removed < totalSize) {
            totalSize -= removed;
        }
    }
}

} // namespace httpfs_extension
} // namespace lbug
