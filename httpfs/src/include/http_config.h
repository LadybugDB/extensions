#pragma once

#include "main/client_context.h"

#if defined(__WASM__)
#define HTTPFS_REMOTE_READ_OPTIMIZATIONS 0
#else
#define HTTPFS_REMOTE_READ_OPTIMIZATIONS 1
#endif

namespace lbug {
namespace httpfs_extension {

struct HTTPConfig {
    explicit HTTPConfig(main::ClientContext* context);

    bool cacheFile;
    bool cacheBlocks;        // persistent cross-run span cache (opt-in, auto for xet)
    uint64_t cacheBlocksMaxMB;
    uint64_t prefetchDepth;  // read-ahead window size in blocks

    // Read-ahead / cache tuning.
    uint64_t readBufferSize;        // block size for ordinary reads
    uint64_t metadataReadBufferSize; // smaller block size for metadata-like reads
    uint64_t readCacheBlocks;        // number of blocks retained in the LRU cache
};

struct HTTPCacheFileConfig {
    static constexpr const char* HTTP_CACHE_FILE_ENV_VAR = "HTTP_CACHE_FILE";
    static constexpr const char* HTTP_CACHE_FILE_OPTION = "http_cache_file";
    static constexpr bool DEFAULT_CACHE_FILE = false;
};

struct HTTPReadBufferSizeConfig {
    static constexpr const char* HTTP_READ_BUFFER_SIZE_ENV_VAR = "HTTP_READ_BUFFER_SIZE";
    static constexpr const char* HTTP_READ_BUFFER_SIZE_OPTION = "http_read_buffer_size";
    static constexpr uint64_t DEFAULT_READ_BUFFER_SIZE = 1000000;
};

struct HTTPMetadataReadBufferSizeConfig {
    static constexpr const char* HTTP_METADATA_READ_BUFFER_SIZE_ENV_VAR =
        "HTTP_METADATA_READ_BUFFER_SIZE";
    static constexpr const char* HTTP_METADATA_READ_BUFFER_SIZE_OPTION =
        "http_metadata_read_buffer_size";
    static constexpr uint64_t DEFAULT_METADATA_READ_BUFFER_SIZE = 65536;
};

struct HTTPReadCacheBlocksConfig {
    static constexpr const char* HTTP_READ_CACHE_BLOCKS_ENV_VAR = "HTTP_READ_CACHE_BLOCKS";
    static constexpr const char* HTTP_READ_CACHE_BLOCKS_OPTION = "http_read_cache_blocks";
    static constexpr uint64_t DEFAULT_READ_CACHE_BLOCKS = 8;
};

// Enables the persistent (cross-run) block cache for remote reads. It is always
// applied to xet:// content-addressed datasets regardless of this setting,
// because their spans are immutable; other http(s)/s3 endpoints only use it
// when explicitly opted in via this option/env var.
struct HTTPCacheBlocksConfig {
    static constexpr const char* HTTP_CACHE_BLOCKS_ENV_VAR = "HTTP_CACHE_BLOCKS";
    static constexpr const char* HTTP_CACHE_BLOCKS_OPTION = "http_cache_blocks";
    static constexpr bool DEFAULT_CACHE_BLOCKS = false;
};

// How many blocks ahead of the current read position to speculatively fetch in
// parallel. 0 disables prefetching.
struct HTTPPrefetchDepthConfig {
    static constexpr const char* HTTP_PREFETCH_DEPTH_ENV_VAR = "HTTP_PREFETCH_DEPTH";
    static constexpr const char* HTTP_PREFETCH_DEPTH_OPTION = "http_prefetch_depth";
    static constexpr int64_t DEFAULT_PREFETCH_DEPTH = 4;
};

// Storage budget of the persistent block cache on disk, pruned least-recently-
// modified first. Env-only (no database option).
struct HTTPCacheBlocksMaxBytesConfig {
    static constexpr const char* HTTP_CACHE_BLOCKS_MAX_MB_ENV_VAR = "HTTP_CACHE_BLOCKS_MAX_MB";
    static constexpr uint64_t DEFAULT_CACHE_BLOCKS_MAX_MB = 2048;
};

struct HTTPConfigEnvProvider {
    static void setOptionValue(main::ClientContext* context);
};

} // namespace httpfs_extension
} // namespace lbug
