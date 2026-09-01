#include "httpfs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "common/cast.h"
#include "common/exception/io.h"
#include "common/exception/not_implemented.h"
#include "common/string_utils.h"
#include "extension/extension.h"
#include "httpfs_extension.h"
#include "transaction/transaction.h"
#include <format>
#include <cstdio>

#ifdef __WASM__
#include <emscripten.h>
#endif

namespace lbug {
namespace httpfs_extension {

using namespace lbug::common;

std::unordered_map<std::string, std::unique_ptr<httplib::Client>>
    HTTPFileSystem::sharedNoRedirectClients;
std::mutex HTTPFileSystem::httpPoolMtx;
std::mutex HTTPFileSystem::hostLocksMtx;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> HTTPFileSystem::hostLocks;
std::unordered_map<std::string, uint64_t> HTTPFileSystem::httpSizeCache;
std::mutex HTTPFileSystem::httpSizeCacheMtx;

HTTPResponse::HTTPResponse(httplib::Response& res, std::string url)
    : code{res.status}, error{res.reason}, url{std::move(url)}, body{res.body} {
    for (auto& [name, value] : res.headers) {
        headers[name] = value;
    }
}

HTTPResponse::HTTPResponse(int code, std::string error, HeaderMap headers, std::string url,
    std::string body)
    : code{code}, error{std::move(error)}, headers{std::move(headers)}, url{std::move(url)},
      body{std::move(body)} {}

#ifdef __WASM__
namespace {

std::string trimHeaderValue(std::string value) {
    auto begin = std::find_if_not(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

HeaderMap parseResponseHeaders(const std::string& rawHeaders) {
    HeaderMap headers;
    std::istringstream stream(rawHeaders);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        headers[line.substr(0, separator)] = trimHeaderValue(line.substr(separator + 1));
    }
    return headers;
}

std::unique_ptr<HTTPResponse> runBrowserRequest(const std::string& method, const std::string& url,
    HeaderMap& headerMap, const uint8_t* inputBuffer = nullptr, uint64_t inputBufferLen = 0) {
    std::vector<const char*> headerPairs;
    headerPairs.reserve(headerMap.size() * 2);
    for (auto& entry : headerMap) {
        headerPairs.push_back(entry.first.c_str());
        headerPairs.push_back(entry.second.c_str());
    }

    // clang-format off
    void* rawResponse = reinterpret_cast<void*>(EM_ASM_PTR({
        var method = UTF8ToString($0);
        var url = UTF8ToString($1);
        var headerCount = $2;
        var headerPairs = $3;
        var bodyPtr = $4;
        var bodyLen = $5;
        var status = 0;
        var statusText = "";
        var responseHeaders = "";
        var responseBytes = new Uint8Array(0);

        try {
            var xhr = new XMLHttpRequest();
            xhr.open(method, url, false);
            xhr.responseType = "arraybuffer";

            for (var i = 0; i < headerCount * 2; i += 2) {
                var namePtr = HEAPU32[(headerPairs >> 2) + i];
                var valuePtr = HEAPU32[(headerPairs >> 2) + i + 1];
                var name = UTF8ToString(namePtr);
                if (name === "Host" || name === "User-Agent") {
                    continue;
                }
                xhr.setRequestHeader(name, UTF8ToString(valuePtr));
            }

            if (bodyLen > 0) {
                xhr.send(HEAPU8.slice(bodyPtr, bodyPtr + bodyLen));
            } else {
                xhr.send(null);
            }

            status = xhr.status;
            statusText = xhr.statusText || "";
            responseHeaders = xhr.getAllResponseHeaders() || "";
            if (xhr.response) {
                responseBytes = new Uint8Array(xhr.response);
            }
        } catch (error) {
            status = 0;
            statusText = String(error);
        }

        var statusTextLen = lengthBytesUTF8(statusText);
        var headersLen = lengthBytesUTF8(responseHeaders);
        var bodyOutLen = responseBytes.byteLength;
        var statusTextOffset = 16;
        var headersOffset = statusTextOffset + statusTextLen + 1;
        var bodyOffset = headersOffset + headersLen + 1;
        var result = _malloc(bodyOffset + bodyOutLen);

        HEAPU32[result >> 2] = status;
        HEAPU32[(result >> 2) + 1] = statusTextLen;
        HEAPU32[(result >> 2) + 2] = headersLen;
        HEAPU32[(result >> 2) + 3] = bodyOutLen;
        stringToUTF8(statusText, result + statusTextOffset, statusTextLen + 1);
        stringToUTF8(responseHeaders, result + headersOffset, headersLen + 1);
        HEAPU8.set(responseBytes, result + bodyOffset);
        return result;
    }, method.c_str(), url.c_str(), headerMap.size(), headerPairs.data(), inputBuffer,
        inputBufferLen));
    // clang-format on

    if (rawResponse == nullptr) {
        throw IOException(std::format("Browser HTTP {} to '{}' failed", method, url));
    }

    auto* bytes = static_cast<uint8_t*>(rawResponse);
    auto* fields = reinterpret_cast<uint32_t*>(rawResponse);
    auto code = static_cast<int>(fields[0]);
    auto statusTextLen = fields[1];
    auto headersLen = fields[2];
    auto bodyLen = fields[3];
    auto statusTextOffset = 16;
    auto headersOffset = statusTextOffset + statusTextLen + 1;
    auto bodyOffset = headersOffset + headersLen + 1;

    std::string statusText(reinterpret_cast<char*>(bytes + statusTextOffset), statusTextLen);
    std::string rawHeaders(reinterpret_cast<char*>(bytes + headersOffset), headersLen);
    std::string body(reinterpret_cast<char*>(bytes + bodyOffset), bodyLen);
    free(rawResponse);

    return std::make_unique<HTTPResponse>(code, std::move(statusText),
        parseResponseHeaders(rawHeaders), url, std::move(body));
}

std::unique_ptr<HTTPResponse> runBrowserRequestWithRetry(const std::string& method,
    const std::string& url, HeaderMap headerMap, const uint8_t* inputBuffer = nullptr,
    uint64_t inputBufferLen = 0) {
    uint64_t tries = 0;
    while (true) {
        auto response = runBrowserRequest(method, url, headerMap, inputBuffer, inputBufferLen);
        switch (response->code) {
        case 408:
        case 418:
        case 429:
        case 503:
        case 504:
            break;
        default:
            return response;
        }
        tries++;
        if (tries > HTTPParams::DEFAULT_RETRIES) {
            return response;
        }
        if (tries > 1) {
            auto sleepTime = (uint64_t)((float)HTTPParams::DEFAULT_RETRY_WAIT_MS *
                                        pow(HTTPParams::DEFAULT_RETRY_BACKOFF, tries - 2));
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
        }
    }
}

} // namespace
#endif

HTTPFileInfo::HTTPFileInfo(std::string path, FileSystem* fileSystem, int flags,
    main::ClientContext* context)
    : FileInfo{std::move(path), fileSystem}, flags{flags}, length{0}, fileOffset{0},
      httpConfig{context}, cachedFileInfo{nullptr} {}

void HTTPFileInfo::initMetadata() {
    // The HTTP client must be initialized on every open, including the fast
    // path below: a size-cache hit skips the HEAD request, but any subsequent
    // read still dereferences httpClient. Leaving it null here caused a
    // SIGSEGV on the first ranged GET of every repeat open of the same URL
    // (https://github.com/LadybugDB/ladybug/issues/880).
    initializeClient();
    // Remote files are immutable during a query; reuse the file size learned by
    // an earlier openFile() for the same URL instead of paying a fresh HEAD +
    // redirect round trip per open. This is what turns N opens of the same
    // parquet file (~450 in a rel scan) into a single HEAD.
    {
        std::lock_guard<std::mutex> lck{HTTPFileSystem::httpSizeCacheMtx};
        auto it = HTTPFileSystem::httpSizeCache.find(path);
        if (it != HTTPFileSystem::httpSizeCache.end()) {
            length = it->second;
            return;
        }
    }

    auto hfs = fileSystem->ptrCast<HTTPFileSystem>();
    auto res = hfs->headRequest(this->ptrCast<HTTPFileInfo>(), path, {});
    std::string rangeLength;
    if (res->code != 200) {
        if ((flags & FileFlags::WRITE) && res->code == 404) {
            if (!(flags & FileFlags::CREATE_IF_NOT_EXISTS) &&
                !(flags & FileFlags::CREATE_AND_TRUNCATE_IF_EXISTS)) {
                throw IOException(std::format("Unable to open URL: \"{}\" for writing: file does "
                                              "not exist and CREATE flag is not set",
                    path));
            }
            length = 0;
            return;
        } else if ((flags & FileFlags::READ_ONLY) && res->code != 404) {
            // HEAD request fail, use Range request for another try (read only one byte).
            auto rangeRequest =
                hfs->getRangeRequest(this, this->path, {}, 0, nullptr /* buffer */, 2);
            if (rangeRequest->code != 206) {
                // LCOV_EXCL_START
                throw IOException(std::format("Unable to connect to URL \"{}\": {} ({})",
                    this->path, res->code, res->error));
                // LCOV_EXCL_STOP
            }
            auto rangeFound = rangeRequest->headers["Content-Range"].find("/");

            if (rangeFound == std::string::npos ||
                rangeRequest->headers["Content-Range"].size() < rangeFound + 1) {
                // LCOV_EXCL_START
                throw IOException(std::format("Unknown Content-Range Header \"The value of "
                                              "Content-Range Header\":  ({})",
                    rangeRequest->headers["Content-Range"]));
                // LCOV_EXCL_STOP
            }

            rangeLength = rangeRequest->headers["Content-Range"].substr(rangeFound + 1);
            if (rangeLength == "*") {
                // LCOV_EXCL_START
                throw IOException(
                    std::format("Unknown total length of the document \"{}\": {} ({})", this->path,
                        res->code, res->error));
                // LCOV_EXCL_STOP
            }
            res = std::move(rangeRequest);
        } else {
            // LCOV_EXCL_START
            throw IOException(std::format("Unable to connect to URL \"{}\": {} ({})", res->url,
                std::to_string(res->code), res->error));
            // LCOV_EXCL_STOP
        }
    }

    // The read buffer is now allocated lazily per fetched block inside the LRU
    // cache, so there is nothing to pre-allocate here.
    if (res->headers.find("Content-Length") == res->headers.end() ||
        res->headers["Content-Length"].empty()) {
        // LCOV_EXCL_START
        throw IOException{"Not able to get Content-length of the given file"};
        // LCOV_EXCL_STOP
    } else {
        try {
            if (res->headers.find("Content-Range") == res->headers.end() ||
                res->headers["Content-Range"].empty()) {
                length = std::stoll(res->headers["Content-Length"]);
            } else {
                length = std::stoll(rangeLength);
            }
        } catch (std::invalid_argument& e) {
            // LCOV_EXCL_START
            throw IOException(std::format("Invalid Content-Length header received: {}",
                res->headers["Content-Length"]));
            // LCOV_EXCL_STOP
        } catch (std::out_of_range& e) {
            // LCOV_EXCL_START
            throw IOException(std::format("Invalid Content-Length header received: {}",
                res->headers["Content-Length"]));
            // LCOV_EXCL_STOP
        }
    }
    if (length > 0) {
        std::lock_guard<std::mutex> lck{HTTPFileSystem::httpSizeCacheMtx};
        HTTPFileSystem::httpSizeCache[path] = length;
    }
}

void HTTPFileInfo::initialize(main::ClientContext* context) {
#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
    // Content-addressed (xet://) files may persist fetched spans across runs;
    // everything else must opt in through the http_cache_blocks option/env var.
    if (httpConfig.prefetchDepth > 0 || httpConfig.cacheBlocks || immutableContent) {
        auto* selfFs = fileSystem->ptrCast<HTTPFileSystem>();
        selfFs->ensureBlockCache(context, httpConfig.cacheBlocks || immutableContent);
    }
#endif
    if (httpConfig.cacheFile && !VirtualFileSystem::GetUnsafe(*context)->isCompressedFile(path)) {
        auto hfs = fileSystem->ptrCast<HTTPFileSystem>();
        cachedFileInfo = hfs->getCachedFileManager().getCachedFileInfo(this,
            transaction::Transaction::Get(*context)->getID());
        return;
    }
    initMetadata();
}

void HTTPFileInfo::initializeClient() {
#ifndef __WASM__
    auto [host, hostPath] = HTTPFileSystem::parseUrl(path);
    httpClient = HTTPFileSystem::getClient(host.c_str());
#endif
}

std::unique_ptr<common::FileInfo> HTTPFileSystem::openFile(const std::string& path,
    FileOpenFlags flags, main::ClientContext* context) {
    return openFileWithImmutability(path, flags, context, /*immutableContent=*/false);
}

std::unique_ptr<common::FileInfo> HTTPFileSystem::openFileWithImmutability(const std::string& path,
    FileOpenFlags flags, main::ClientContext* context, bool immutableContent) {
    if (context->getCurrentSetting(HTTPCacheFileConfig::HTTP_CACHE_FILE_OPTION).getValue<bool>()) {
        initCachedFileManager(context);
    }
    auto httpFileInfo = std::make_unique<HTTPFileInfo>(path, this, flags.flags, context);
#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
    // Must be set before initialize(): the persistent-cache decision depends on
    // whether the content is content-addressed (xet://).
    httpFileInfo->immutableContent = immutableContent;
#else
    (void)immutableContent;
#endif
    httpFileInfo->initialize(context);
    return httpFileInfo;
}

std::vector<std::string> HTTPFileSystem::glob(main::ClientContext* /*context*/,
    const std::string& path) const {
    // Glob is not supported on HTTPFS, simply return the path itself.
    return {path};
}

bool HTTPFileSystem::canHandleFile(const std::string_view path) const {
    return path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0;
}

bool HTTPFileSystem::fileOrPathExists(const std::string& path, main::ClientContext* context) {
    FileOpenFlags flags{FileFlags::READ_ONLY};
    flags.lockType = FileLockType::READ_LOCK;
    try {
        auto fileInfo = openFile(path, flags, context);
        auto httpFileInfo = fileInfo->constPtrCast<HTTPFileInfo>();
        if (httpFileInfo->cachedFileInfo != nullptr) {
            return true;
        }
        if (httpFileInfo->length == 0) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void HTTPFileSystem::cleanUP(main::ClientContext* context) {
    if (cachedFileManager != nullptr) {
        cachedFileManager->cleanUP(context);
    }
}

HTTPFileInfo::CachedBlock* HTTPFileInfo::lookupCachedBlock(uint64_t pos) {
    for (auto it = readCache.begin(); it != readCache.end(); ++it) {
        if (pos >= it->offset && pos < it->offset + it->length) {
            // Promote to MRU.
            if (it != readCache.begin()) {
                readCache.splice(readCache.begin(), readCache, it);
            }
            return &*readCache.begin();
        }
    }
    return nullptr;
}

void HTTPFileInfo::cacheBlock(uint64_t offset, uint64_t length,
    std::unique_ptr<uint8_t[]> data) {
    CachedBlock block{offset, length, std::move(data)};
    readCache.push_front(std::move(block));
    while (readCache.size() > httpConfig.readCacheBlocks) {
        readCache.pop_back();
    }
}

#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
void HTTPFileSystem::ensureBlockCache(main::ClientContext* context, bool wantPersistentCache) {
    std::lock_guard<std::mutex> lck{initRemoteCachesMtx};
    if ((blockCache && blockCache->enabled()) ||
        (prefetchPool && !wantPersistentCache)) {
        return;
    }
    const HTTPConfig config{context};
#if !defined(__WASM__)
    if (prefetchPool == nullptr && config.prefetchDepth > 0) {
        // A modest pool suffices: depth scales the number of workers needed.
        prefetchPool = std::make_unique<PrefetchPool>(std::min<size_t>(config.prefetchDepth,
            /*cap*/ 8));
    }
#endif
    if (!wantPersistentCache || blockCache != nullptr) {
        return;
    }
    try {
        const auto localDir = extension::ExtensionUtils::getLocalDirForExtension(context,
            StringUtils::getLower(HttpfsExtension::EXTENSION_NAME));
        auto cacheRoot = FileSystem::joinPath(localDir, ".block_cache");
        if (blockCache == nullptr) {
            blockCache = std::make_unique<PersistentBlockCache>();
        }
        blockCache->init(cacheRoot, config.cacheBlocksMaxMB * 1024ull * 1024ull);
        if (!blockCache->enabled()) {
            blockCache.reset();
        }
    } catch (Exception&) {
        // Cache disabled on failure; reads still work over the network.
        if (blockCache != nullptr) {
            blockCache.reset();
        }
    }
}

HTTPFileSystem::PrefetchZone* HTTPFileSystem::zoneFor(const std::string& url) const {
    std::lock_guard<std::mutex> lck{zoneMapMtx};
    auto it = zones.find(url);
    if (it == zones.end()) {
        // Piecewise construction: PrefetchZone holds a mutex and must be
        // constructed in place (non-movable).
        it = zones.emplace(std::piecewise_construct, std::forward_as_tuple(url),
            std::make_tuple())
                 .first;
    }
    return &it->second;
}
#endif

// Number of bytes served out of a span beginning at spanStart. A request may
// be partially covered by the tail of a cached/prefetched span; the remainder
// is fetched in subsequent loop iterations.
inline uint64_t copySpanPortion(char* dest, const char* spanData, uint64_t spanStart,
    uint64_t spanLen, uint64_t pos, uint64_t remaining) {
    const auto spanOffset = pos - spanStart;
    if (spanOffset >= spanLen) {
        return 0;
    }
    const auto len = std::min<uint64_t>(spanLen - spanOffset, remaining);
    std::memcpy(dest, spanData + spanOffset, len);
    return len;
}

#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
void HTTPFileSystem::submitPrefetch(const HTTPFileInfo& fileInfo, uint64_t nextOffset,
    uint64_t blockSize) const {
    if (!prefetchPool || blockSize == 0 || fileInfo.httpConfig.prefetchDepth == 0) {
        return;
    }
    auto* self = const_cast<HTTPFileSystem*>(this);
    auto* zone = zoneFor(fileInfo.path);
    for (uint64_t i = 0; i < fileInfo.httpConfig.prefetchDepth; ++i) {
        const auto start = nextOffset + i * blockSize;
        if (start >= fileInfo.length) {
            break;
        }
        const auto len = std::min<uint64_t>(blockSize, fileInfo.length - start);
        const std::string key = std::format("{}:{}", start, len);
        {
            std::lock_guard<std::mutex> lck{zone->mtx};
            if (zone->inflight.contains(key) || zone->ready.contains(key)) {
                continue;
            }
            zone->inflight.insert(key);
        }
        const std::string url = fileInfo.path;
        struct InflightGuard {
            PrefetchZone* zone;
            const std::string& key;
            bool released = false;
            ~InflightGuard() {
                if (!released) {
                    std::lock_guard<std::mutex> lck{zone->mtx};
                    zone->inflight.erase(key);
                }
            }
        } inflightGuard{zone, key};

        const bool submitted = prefetchPool->trySubmit(
            [self, zone, url, start, len, key]() mutable {
                try {
                    std::vector<char> data(len);
                    // XetFileSystem ignores the FileInfo* argument for range
                    // fetches, so a null pointer is safe here. Virtual dispatch
                    // ensures we land on the concrete implementation owning this
                    // file's URL scheme.
                    auto response = self->getRangeRequest(nullptr, url, {}, start,
                        reinterpret_cast<char*>(data.data()), len);
                    if (response != nullptr && response->code < 300 &&
                        response->body.size() == len) {
                        auto payload = std::make_shared<std::vector<char>>(std::move(data));
                        {
                            std::lock_guard<std::mutex> lck{zone->mtx};
                            if (zone->ready.size() >= PrefetchZone::MAX_READY_ENTRIES) {
                                // Bounded window: prefer evicting entries that were
                                // already consumed at least once.
                                bool evicted = false;
                                for (auto it = zone->ready.begin(); it != zone->ready.end();
                                     ++it) {
                                    if (it->second.consumeCount > 0) {
                                        zone->ready.erase(it);
                                        evicted = true;
                                        break;
                                    }
                                }
                                if (!evicted) {
                                    zone->inflight.erase(key);
                                    return; // drop this speculative result entirely
                                }
                            }
                            zone->ready[key] =
                                ReadyEntry{std::move(payload), /*consumeCount=*/0};
                        }
                        if (const char* te = std::getenv("LBUG_HTTPFS_TRACE"); te && te[0] == '1') {
                            std::fprintf(stderr, "[httpfs] PREFETCH DONE start=%llu len=%llu\n",
                                (unsigned long long)start, (unsigned long long)len);
                            std::fflush(stderr);
                        }
                    } else if (response != nullptr) {
                        if (const char* te = std::getenv("LBUG_HTTPFS_TRACE"); te && te[0] == '1') {
                            std::fprintf(stderr,
                                "[httpfs] PREFETCH BADRESP code=%d size=%zu want=%llu\n",
                                response->code, response->body.size(), (unsigned long long)len);
                            std::fflush(stderr);
                        }
                    }
                } catch (const common::Exception& e) {
                    if (const char* te = std::getenv("LBUG_HTTPFS_TRACE"); te && te[0] == '1') {
                        std::fprintf(stderr, "[httpfs] PREFETCH FAIL %s\n", e.what());
                        std::fflush(stderr);
                    }
                }
                {
                    std::lock_guard<std::mutex> lck{zone->mtx};
                    zone->inflight.erase(key);
                }
            });
        if (submitted) {
            inflightGuard.released = true;
        }
    }
}
#endif

void HTTPFileSystem::readFromFile(common::FileInfo& fileInfo, void* buffer, uint64_t numBytes,
    uint64_t position) const {
    auto& httpFileInfo = fileInfo.cast<HTTPFileInfo>();
    auto numBytesToRead = numBytes;
    auto bufferOffset = 0;
    httpFileInfo.fileOffset = position;
#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
    PrefetchZone* zone = nullptr;
    if (httpFileInfo.immutableContent && prefetchPool != nullptr) {
        zone = zoneFor(httpFileInfo.path);
    }
    PersistentBlockCache* diskCache =
        httpFileInfo.immutableContent ? blockCache.get() : nullptr;
    if (!diskCache && httpFileInfo.httpConfig.cacheBlocks) {
        // Opt-in persistent cache for non-content-addressed remote files.
        // Currently only reachable for immutable datasets because cache init
        // happens on open; kept here for future schemes.
        diskCache = blockCache.get();
    }
#endif
    while (numBytesToRead > 0) {
        auto currentPos = position + bufferOffset;

        // L1: serve from the in-memory LRU cache.
        {
            std::lock_guard<std::mutex> lck{httpFileInfo.readCacheMtx};
            if (auto* block = httpFileInfo.lookupCachedBlock(currentPos)) {
                const auto served = copySpanPortion((char*)buffer + bufferOffset,
                    reinterpret_cast<const char*>(block->data.get()), block->offset, block->length,
                    currentPos, numBytesToRead);
                bufferOffset += served;
                numBytesToRead -= served;
                httpFileInfo.fileOffset += served;
                if (served > 0) {
                    continue;
                }
            }
        }

        // Determine the span size used for all layers below (same rule as the
        // original implementation so byte ranges stay identical).
        const uint64_t blockSize =
            (numBytesToRead <= httpFileInfo.httpConfig.metadataReadBufferSize)
                ? httpFileInfo.httpConfig.metadataReadBufferSize
                : httpFileInfo.httpConfig.readBufferSize;
        const uint64_t fetchStart = (currentPos / blockSize) * blockSize;
        if (fetchStart >= httpFileInfo.length) {
            break;
        }
        const uint64_t fetchLen =
            std::min<uint64_t>(blockSize, httpFileInfo.length - fetchStart);
        if (fetchLen == 0) {
            break;
        }
#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
        const bool haveRemoteOptimizations = zone != nullptr || diskCache != nullptr;

        if (haveRemoteOptimizations) {
            // L2: prefetched landing zone (parallel fetches already completed).
            if (zone != nullptr) {
                std::shared_ptr<std::vector<char>> payload;
                {
                    std::lock_guard<std::mutex> lck{zone->mtx};
                    const std::string key = std::format("{}:{}", fetchStart, fetchLen);
                    if (auto it = zone->ready.find(key); it != zone->ready.end()) {
                        payload = it->second.payload;
                        ++it->second.consumeCount;
                    }
                }
                if (payload != nullptr && payload->size() == fetchLen) {
                    auto blockData = std::make_unique<uint8_t[]>(fetchLen);
                    std::memcpy(blockData.get(), payload->data(), fetchLen);
                    {
                        std::lock_guard<std::mutex> lck{httpFileInfo.readCacheMtx};
                        httpFileInfo.cacheBlock(fetchStart, fetchLen, std::move(blockData));
                    }
                    if (const char* te = std::getenv("LBUG_HTTPFS_TRACE"); te && te[0] == '1') {
                        std::fprintf(stderr, "[httpfs] ZONE HIT pos=%llu\n",
                            (unsigned long long)currentPos);
                        std::fflush(stderr);
                    }
                    continue; // loop serves from the LRU now
                }
            }

            // L3: persistent cross-run cache (immutable spans only).
            if (diskCache != nullptr && diskCache->enabled()) {
                auto blockData = std::make_unique<uint8_t[]>(fetchLen);
                if (diskCache->get(httpFileInfo.path, fetchStart, fetchLen,
                        reinterpret_cast<char*>(blockData.get()))) {
                    {
                        std::lock_guard<std::mutex> lck{httpFileInfo.readCacheMtx};
                        httpFileInfo.cacheBlock(fetchStart, fetchLen, std::move(blockData));
                    }
                    continue; // loop serves from the LRU now
                }
            }
        }
#endif // HTTPFS_REMOTE_READ_OPTIMIZATIONS

        // L4: network fetch of the aligned span into a staging buffer.
        auto blockData = std::make_unique<uint8_t[]>(fetchLen);
        if (const char* traceEnv = std::getenv("LBUG_HTTPFS_TRACE");
            traceEnv != nullptr && traceEnv[0] == '1') {
            std::fprintf(stderr,
                "[httpfs] MISS pos=%llu nLeft=%llu bs=%llu zone=%d disk=%d imm=%d depth=%llu\n",
                (unsigned long long)currentPos, (unsigned long long)numBytesToRead,
#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
                (unsigned long long)blockSize, zone != nullptr, diskCache != nullptr,
#else
                (unsigned long long)blockSize, false, false,
#endif
                httpFileInfo.immutableContent,
                (unsigned long long)httpFileInfo.httpConfig.prefetchDepth);
            std::fflush(stderr);
        }
        getRangeRequest(&httpFileInfo, httpFileInfo.path, {}, fetchStart,
            reinterpret_cast<char*>(blockData.get()), fetchLen);

#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
        if (haveRemoteOptimizations) {
            // Write-through to the persistent cache and pipeline further reads.
            if (diskCache != nullptr && diskCache->enabled()) {
                diskCache->put(httpFileInfo.path, fetchStart, fetchLen,
                    reinterpret_cast<const char*>(blockData.get()));
            }
            if (zone != nullptr) {
                submitPrefetch(httpFileInfo, fetchStart + fetchLen, blockSize);
            }
        }
#endif // HTTPFS_REMOTE_READ_OPTIMIZATIONS

        {
            std::lock_guard<std::mutex> lck{httpFileInfo.readCacheMtx};
            httpFileInfo.cacheBlock(fetchStart, fetchLen, std::move(blockData));
        }
        // Loop again: the next iteration will serve the request from the cache.
    }
}

int64_t HTTPFileSystem::readFile(common::FileInfo& fileInfo, void* buf, size_t numBytes) const {
    auto& httpFileInfo = fileInfo.constCast<HTTPFileInfo>();
    if (httpFileInfo.cachedFileInfo != nullptr) {
        return httpFileInfo.cachedFileInfo->readFile(buf, numBytes);
    }
    auto maxNumBytesToRead = httpFileInfo.length - httpFileInfo.fileOffset;
    numBytes = std::min<uint64_t>(maxNumBytesToRead, numBytes);
    if (httpFileInfo.fileOffset > httpFileInfo.getFileSize()) {
        return 0;
    }
    readFromFile(fileInfo, buf, numBytes, httpFileInfo.fileOffset);
    return numBytes;
}

void HTTPFileSystem::syncFile(const common::FileInfo&) const {
    throw NotImplementedException("syncFile is not supported in HTTPFileSystem");
}

int64_t HTTPFileSystem::seek(common::FileInfo& fileInfo, uint64_t offset, int whence) const {
    auto& httpFileInfo = fileInfo.cast<HTTPFileInfo>();
    if (httpFileInfo.cachedFileInfo != nullptr) {
        httpFileInfo.cachedFileInfo->seek(offset, whence);
        return offset;
    }
    httpFileInfo.fileOffset = offset;
    return offset;
}

uint64_t HTTPFileSystem::getFileSize(const common::FileInfo& fileInfo) const {
    auto& httpFileInfo = fileInfo.constCast<HTTPFileInfo>();
    if (httpFileInfo.cachedFileInfo != nullptr) {
        return httpFileInfo.cachedFileInfo->getFileSize();
    }
    return httpFileInfo.length;
}

std::unique_ptr<httplib::Client> HTTPFileSystem::getClient(const std::string& host) {
    auto client = std::make_unique<httplib::Client>(host);
    client->set_follow_location(true);
    client->set_keep_alive(HTTPParams::DEFAULT_KEEP_ALIVE);
    // TODO(Chang): Windows CI is missing some certificates, which causes tests to fail. Enable the
    // certificate verification after fixing the certificate issue.
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    client->enable_server_certificate_verification(false);
#endif
    client->set_write_timeout(HTTPParams::DEFAULT_TIMEOUT);
    client->set_read_timeout(HTTPParams::DEFAULT_TIMEOUT);
    client->set_connection_timeout(HTTPParams::DEFAULT_TIMEOUT);
    client->set_decompress(false);
    return client;
}

// Build a no-follow httplib client for `host` that does not automatically
// follow redirects. The Xet resolve endpoint returns 302 Location headers which
// the filesystem follows itself, so redirects must be surfaced rather than
// absorbed (which would also point the pooled connection at the wrong host).
static httplib::Client* makeNoRedirectClient(const std::string& host) {
    auto client = std::make_unique<httplib::Client>(host);
    client->set_follow_location(false);
    client->set_url_encode(false);
    client->set_keep_alive(HTTPParams::DEFAULT_KEEP_ALIVE);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    client->enable_server_certificate_verification(false);
#endif
    client->set_write_timeout(HTTPParams::DEFAULT_TIMEOUT);
    client->set_read_timeout(HTTPParams::DEFAULT_TIMEOUT);
    client->set_connection_timeout(HTTPParams::DEFAULT_TIMEOUT);
    client->set_decompress(false);
    return client.release();
}

void HTTPFileSystem::lockHttp(const std::string& host) {
    std::mutex* hostMutex = nullptr;
    {
        std::lock_guard<std::mutex> lck{hostLocksMtx};
        auto it = hostLocks.find(host);
        if (it == hostLocks.end()) {
            it = hostLocks.emplace(host, std::make_shared<std::mutex>()).first;
        }
        hostMutex = it->second.get();
    }
    // Lock the host mutex outside of hostLocksMtx so other hosts stay free.
    hostMutex->lock();
}

void HTTPFileSystem::unlockHttp(const std::string& host) {
    std::shared_ptr<std::mutex> hostMutex;
    {
        std::lock_guard<std::mutex> lck{hostLocksMtx};
        auto it = hostLocks.find(host);
        if (it == hostLocks.end()) {
            return;
        }
        hostMutex = it->second;
    }
    hostMutex->unlock();
}

httplib::Client* HTTPFileSystem::getSharedNoRedirectClient(const std::string& host) {
    std::lock_guard<std::mutex> lck{httpPoolMtx};
    auto it = sharedNoRedirectClients.find(host);
    if (it == sharedNoRedirectClients.end()) {
        std::unique_ptr<httplib::Client> client{makeNoRedirectClient(host)};
        it = sharedNoRedirectClients.emplace(host, std::move(client)).first;
    }
    return it->second.get();
}

httplib::Client* HTTPFileSystem::evictAndGetSharedNoRedirectClient(const std::string& host) {
    std::lock_guard<std::mutex> lck{httpPoolMtx};
    sharedNoRedirectClients.erase(host);
    std::unique_ptr<httplib::Client> client{makeNoRedirectClient(host)};
    auto it = sharedNoRedirectClients.emplace(host, std::move(client)).first;
    return it->second.get();
}

std::unique_ptr<httplib::Headers> HTTPFileSystem::getHTTPHeaders(HeaderMap& headerMap) {
    auto headers = std::make_unique<httplib::Headers>();
    for (auto& entry : headerMap) {
        headers->insert(entry);
    }
    return headers;
}

std::pair<std::string, std::string> HTTPFileSystem::parseUrl(const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        throw IOException("URL needs to start with http:// or https://");
    }
    auto hostPathPos = url.find('/', 8);
    // LCOV_EXCL_START
    if (hostPathPos == std::string::npos) {
        throw IOException("URL needs to contain a '/' after the host");
    }
    // LCOV_EXCL_STOP
    auto host = url.substr(0, hostPathPos);
    auto hostPath = url.substr(hostPathPos);
    // LCOV_EXCEL_START
    if (hostPath.empty()) {
        throw IOException("URL needs to contain a path");
    }
    // LCOV_EXCEL_STOP
    return {host, hostPath};
}

std::unique_ptr<HTTPResponse> HTTPFileSystem::runRequestWithRetry(
    const std::function<httplib::Result(void)>& request, const std::string& url, std::string method,
    const std::function<void(void)>& retry) {
    uint64_t tries = 0;
    while (true) {
        std::exception_ptr exception = nullptr;
        httplib::Error err = httplib::Error::Success;
        httplib::Response response;
        int status = 0;

        try {
            auto res = request();
            err = res.error();
            if (err == httplib::Error::Success) {
                status = res->status;
                response = res.value();
            }
        } catch (IOException& e) {
            exception = std::current_exception();
        }

        if (err == httplib::Error::Success) {
            switch (status) {
            case 408: // Request Timeout
            case 418: // Server is pretending to be a teapot
            case 429: // Rate limiter hit
            case 503: // Server has error
            case 504: // Server has error
                break;
            default:
                return std::make_unique<HTTPResponse>(response, url);
            }
        }

        tries += 1;

        if (tries <= HTTPParams::DEFAULT_RETRIES) {
            if (tries > 1) {
                auto sleepTime = (uint64_t)((float)HTTPParams::DEFAULT_RETRY_WAIT_MS *
                                            pow(HTTPParams::DEFAULT_RETRY_BACKOFF, tries - 2));
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
            }
            if (retry) {
                retry();
            }
        } else {
            if (exception) {
                std::rethrow_exception(exception);
            } else if (err == httplib::Error::Success) {
                // LCOV_EXCL_START
                throw IOException(std::format("Request returned HTTP {} for HTTP {} to '{}'",
                    status, method, url));
                // LCOV_EXCL_STOP
            } else {
                // LCOV_EXCL_START
                throw IOException(
                    std::format("{} error for HTTP {} to '{}'", to_string(err), method, url));
                // LCOV_EXCL_STOP
            }
        }
    }
}

std::unique_ptr<HTTPResponse> HTTPFileSystem::headRequest(FileInfo* fileInfo,
    const std::string& url, HeaderMap headerMap) const {
#ifdef __WASM__
    static_cast<void>(fileInfo);
    return runBrowserRequestWithRetry("HEAD", url, std::move(headerMap));
#else
    auto httpFileInfo = dynamic_cast_checked<HTTPFileInfo*>(fileInfo);
    // Defensive: guard against any open path that did not go through
    // initMetadata() (e.g. a future fast-path open) leaving the client null.
    if (!httpFileInfo->httpClient) {
        httpFileInfo->initializeClient();
    }
    auto parsedURL = parseUrl(url);
    auto host = parsedURL.first;
    auto hostPath = parsedURL.second;
    auto headers = getHTTPHeaders(headerMap);

    std::function<httplib::Result(void)> request(
        [&]() { return httpFileInfo->httpClient->Head(hostPath.c_str(), *headers); });

    std::function<void(void)> retry([&]() { httpFileInfo->httpClient = getClient(host); });

    return runRequestWithRetry(request, url, "HEAD", retry);
#endif
}

std::unique_ptr<HTTPResponse> HTTPFileSystem::getRangeRequest(FileInfo* fileInfo,
    const std::string& url, HeaderMap headerMap, uint64_t fileOffset, char* buffer,
    uint64_t bufferLen) const {
#ifdef __WASM__
    static_cast<void>(fileInfo);
    headerMap["Range"] = std::format("bytes={}-{}", fileOffset, fileOffset + bufferLen - 1);
    auto response = runBrowserRequestWithRetry("GET", url, std::move(headerMap));
    if (response->code == 0 || response->code >= 400) {
        auto error = std::format("HTTP GET error on '{}' (HTTP {})", url, response->code);
        if (response->code == 416) {
            error += "Try confirm the server supports range requests.";
        }
        throw IOException(error);
    }
    if (response->code < 300 && buffer != nullptr) {
        if (response->body.size() > bufferLen) {
            throw IOException("Server sent back more data than expected.");
        }
        memcpy(buffer, response->body.data(), response->body.size());
    }
    return response;
#else
    auto httpFileInfo = dynamic_cast_checked<HTTPFileInfo*>(fileInfo);
    // Defensive: guard against any open path that did not go through
    // initMetadata() (e.g. a future fast-path open) leaving the client null.
    if (!httpFileInfo->httpClient) {
        httpFileInfo->initializeClient();
    }
    auto parsedURL = parseUrl(url);
    auto host = parsedURL.first;
    auto hostPath = parsedURL.second;
    auto headers = getHTTPHeaders(headerMap);

    headers->insert(std::make_pair("Range",
        std::format("bytes={}-{}", fileOffset, fileOffset + bufferLen - 1)));

    uint64_t bufferOffset = 0;

    std::function<httplib::Result(void)> request([&]() {
        return httpFileInfo->httpClient->Get(
            hostPath.c_str(), *headers,
            [&](const httplib::Response& response) {
                if (response.status >= 400) {
                    // LCOV_EXCL_START
                    auto error =
                        std::format("HTTP GET error on '{}' (HTTP {})", url, response.status);
                    if (response.status == 416) {
                        error += "Try confirm the server supports range requests.";
                    }
                    throw IOException(error);
                    // LCOV_EXCL_STOP
                }
                if (response.status < 300) {
                    bufferOffset = 0;
                    if (response.has_header("Content-Length")) {
                        auto contentLen = stoll(response.get_header_value("Content-Length", 0));
                        if ((uint64_t)contentLen != bufferLen) {
                            // LCOV_EXCL_START
                            throw IOException(std::format(
                                "Server returned: {}, HTTP GET error: Content-Length from server "
                                "mismatches requested "
                                "range, server may not support range requests.",
                                response.status));
                            // LCOV_EXCL_STOP
                        }
                    }
                }
                return true;
            },
            [&](const char* data, size_t dataLen) {
                if (buffer != nullptr) {
                    if (dataLen + bufferOffset > bufferLen) {
                        // LCOV_EXCL_START
                        // To avoid corruption of memory, we bail out.
                        throw IOException("Server sent back more data than expected.");
                        // LCOV_EXCL_STOP
                    }
                    memcpy(buffer + bufferOffset, data, dataLen);
                    bufferOffset += dataLen;
                }
                return true;
            });
    });
    std::function<void(void)> retryFunc([&]() { httpFileInfo->httpClient = getClient(host); });
    return runRequestWithRetry(request, url, "GET Range", retryFunc);
#endif
}

std::unique_ptr<HTTPResponse> HTTPFileSystem::postRequest(common::FileInfo* fileInfo,
    const std::string& url, HeaderMap headerMap, std::unique_ptr<uint8_t[]>& outputBuffer,
    uint64_t& outputBufferLen, const uint8_t* inputBuffer, uint64_t inputBufferLen,
    std::string /*params*/) const {
#ifdef __WASM__
    static_cast<void>(fileInfo);
    headerMap["Content-Type"] = "application/octet-stream";
    auto response =
        runBrowserRequestWithRetry("POST", url, std::move(headerMap), inputBuffer, inputBufferLen);
    if (response->body.size() > outputBufferLen) {
        auto newBuffer = std::make_unique<uint8_t[]>(response->body.size());
        outputBuffer = std::move(newBuffer);
        outputBufferLen = response->body.size();
    }
    if (!response->body.empty()) {
        memcpy(outputBuffer.get(), response->body.data(), response->body.size());
    }
    return response;
#else
    auto httpFileInfo = dynamic_cast_checked<HTTPFileInfo*>(fileInfo);
    if (!httpFileInfo->httpClient) {
        httpFileInfo->initializeClient();
    }
    auto hostPath = parseUrl(url).second;
    auto headers = getHTTPHeaders(headerMap);
    uint64_t outputBufferPos = 0;

    std::function<httplib::Result(void)> request([&]() {
        auto client = httpFileInfo->httpClient.get();
        httplib::Request req;
        req.method = "POST";
        req.path = hostPath;
        req.headers = *headers;
        req.headers.emplace("Content-Type", "application/octet-stream");
        req.content_receiver = [&](const char* data, size_t dataLen, uint64_t /*offset*/,
                                   uint64_t /*totalLen*/) {
            if (outputBufferPos + dataLen > outputBufferLen) {
                auto newBufferSize =
                    std::max<uint64_t>(outputBufferPos + dataLen, outputBufferLen * 2);
                auto newBuffer = std::make_unique<uint8_t[]>(newBufferSize);
                memcpy(newBuffer.get(), outputBuffer.get(), outputBufferLen);
                outputBuffer = std::move(newBuffer);
                outputBufferLen = newBufferSize;
            }
            memcpy(outputBuffer.get() + outputBufferPos, data, dataLen);
            outputBufferPos += dataLen;
            return true;
        };
        req.body.assign(reinterpret_cast<const char*>(inputBuffer), inputBufferLen);
        return client->send(req);
    });
    return runRequestWithRetry(request, url, "POST");
#endif
}

std::unique_ptr<HTTPResponse> HTTPFileSystem::putRequest(common::FileInfo* fileInfo,
    const std::string& url, HeaderMap headerMap, const uint8_t* inputBuffer,
    uint64_t inputBufferLen, std::string /*params*/) const {
#ifdef __WASM__
    static_cast<void>(fileInfo);
    headerMap["Content-Type"] = "application/octet-stream";
    return runBrowserRequestWithRetry("PUT", url, std::move(headerMap), inputBuffer,
        inputBufferLen);
#else
    auto httpFileInfo = dynamic_cast_checked<HTTPFileInfo*>(fileInfo);
    if (!httpFileInfo->httpClient) {
        httpFileInfo->initializeClient();
    }
    auto hostPath = parseUrl(url).second;
    auto headers = getHTTPHeaders(headerMap);
    std::function<httplib::Result(void)> request([&]() {
        auto client = httpFileInfo->httpClient.get();
        return client->Put(hostPath.c_str(), *headers, reinterpret_cast<const char*>(inputBuffer),
            inputBufferLen, "application/octet-stream");
    });

    return runRequestWithRetry(request, url, "PUT");
#endif
}

void HTTPFileSystem::initCachedFileManager(main::ClientContext* context) {
    std::unique_lock<std::mutex> lck{cachedFileManagerMtx};
    if (cachedFileManager == nullptr) {
        cachedFileManager = std::make_unique<CachedFileManager>(context);
    }
}

} // namespace httpfs_extension
} // namespace lbug
