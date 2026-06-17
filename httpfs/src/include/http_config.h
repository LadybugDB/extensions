#pragma once

#include "main/client_context.h"

namespace lbug {
namespace httpfs_extension {

struct HTTPConfig {
    explicit HTTPConfig(main::ClientContext* context);

    bool cacheFile;

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

struct HTTPConfigEnvProvider {
    static void setOptionValue(main::ClientContext* context);
};

} // namespace httpfs_extension
} // namespace lbug
