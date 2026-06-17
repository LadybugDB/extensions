#include "http_config.h"

#include "common/exception/exception.h"
#include "common/exception/runtime.h"
#include "common/types/value/value.h"
#include "function/cast/functions/cast_from_string_functions.h"

namespace lbug {
namespace httpfs_extension {

HTTPConfig::HTTPConfig(main::ClientContext* context) {
    DASSERT(context != nullptr);
    cacheFile =
        context->getCurrentSetting(HTTPCacheFileConfig::HTTP_CACHE_FILE_OPTION).getValue<bool>();
    int64_t readBufferSizeVal = context
        ->getCurrentSetting(HTTPReadBufferSizeConfig::HTTP_READ_BUFFER_SIZE_OPTION)
        .getValue<int64_t>();
    if (readBufferSizeVal < 0) {
        throw common::RuntimeException{std::format("Invalid option value for {}",
            HTTPReadBufferSizeConfig::HTTP_READ_BUFFER_SIZE_OPTION)};
    }
    readBufferSize = static_cast<uint64_t>(readBufferSizeVal);
    int64_t metadataReadBufferSizeVal = context
        ->getCurrentSetting(
            HTTPMetadataReadBufferSizeConfig::HTTP_METADATA_READ_BUFFER_SIZE_OPTION)
        .getValue<int64_t>();
    if (metadataReadBufferSizeVal < 0) {
        throw common::RuntimeException{std::format("Invalid option value for {}",
            HTTPMetadataReadBufferSizeConfig::HTTP_METADATA_READ_BUFFER_SIZE_OPTION)};
    }
    metadataReadBufferSize = static_cast<uint64_t>(metadataReadBufferSizeVal);
    int64_t readCacheBlocksVal = context
        ->getCurrentSetting(HTTPReadCacheBlocksConfig::HTTP_READ_CACHE_BLOCKS_OPTION)
        .getValue<int64_t>();
    if (readCacheBlocksVal < 0) {
        throw common::RuntimeException{std::format("Invalid option value for {}",
            HTTPReadCacheBlocksConfig::HTTP_READ_CACHE_BLOCKS_OPTION)};
    }
    readCacheBlocks = static_cast<uint64_t>(readCacheBlocksVal);
    // Guard against nonsensical values.
    if (readBufferSize == 0) {
        readBufferSize = HTTPReadBufferSizeConfig::DEFAULT_READ_BUFFER_SIZE;
    }
    if (metadataReadBufferSize == 0) {
        metadataReadBufferSize =
            HTTPMetadataReadBufferSizeConfig::DEFAULT_METADATA_READ_BUFFER_SIZE;
    }
    if (readCacheBlocks == 0) {
        readCacheBlocks = HTTPReadCacheBlocksConfig::DEFAULT_READ_CACHE_BLOCKS;
    }
}

void HTTPConfigEnvProvider::setOptionValue(main::ClientContext* context) {
    const auto cacheFileOptionStrVal =
        main::ClientContext::getEnvVariable(HTTPCacheFileConfig::HTTP_CACHE_FILE_ENV_VAR);
    if (cacheFileOptionStrVal != "") {
        bool enableCacheFile = false;
        function::CastString::operation(
            common::string_t{cacheFileOptionStrVal.c_str(), cacheFileOptionStrVal.length()},
            enableCacheFile);
        if (enableCacheFile && context->isInMemory()) {
            throw common::Exception("Cannot enable HTTP file cache when database is in memory");
        }
        context->setExtensionOption(HTTPCacheFileConfig::HTTP_CACHE_FILE_OPTION,
            common::Value::createValue(enableCacheFile));
    }

    auto setIntOptionFromEnv = [&](const char* envVar, const char* optionName) {
        const auto strVal = main::ClientContext::getEnvVariable(envVar);
        if (strVal.empty()) {
            return;
        }
        int64_t value = 0;
        function::CastString::operation(
            common::string_t{strVal.c_str(), strVal.length()}, value);
        context->setExtensionOption(optionName, common::Value::createValue(value));
    };
    setIntOptionFromEnv(HTTPReadBufferSizeConfig::HTTP_READ_BUFFER_SIZE_ENV_VAR,
        HTTPReadBufferSizeConfig::HTTP_READ_BUFFER_SIZE_OPTION);
    setIntOptionFromEnv(HTTPMetadataReadBufferSizeConfig::HTTP_METADATA_READ_BUFFER_SIZE_ENV_VAR,
        HTTPMetadataReadBufferSizeConfig::HTTP_METADATA_READ_BUFFER_SIZE_OPTION);
    setIntOptionFromEnv(HTTPReadCacheBlocksConfig::HTTP_READ_CACHE_BLOCKS_ENV_VAR,
        HTTPReadCacheBlocksConfig::HTTP_READ_CACHE_BLOCKS_OPTION);
}

} // namespace httpfs_extension
} // namespace lbug
