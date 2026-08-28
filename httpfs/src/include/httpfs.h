#pragma once

#include "cached_file_manager.h"
#include "common/file_system/local_file_system.h"
#include "http_config.h"
#include "httplib.h"
#include "main/client_context.h"
#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
#include "block_cache.h"
#include "prefetch_pool.h"
#endif
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#define O_ACCMODE 0x0003
#endif

namespace lbug {
namespace httpfs_extension {

using HeaderMap = common::case_insensitive_map_t<std::string>;

struct HTTPResponse {
    HTTPResponse(httplib::Response& res, std::string url);
    HTTPResponse(int code, std::string error, HeaderMap headers, std::string url, std::string body);

    int code;
    std::string error;
    HeaderMap headers;
    std::string url;
    std::string body;
};

struct HTTPParams {
    // TODO(Ziyi): Make them configurable.
    static constexpr uint64_t DEFAULT_TIMEOUT = 30000;
    static constexpr uint64_t DEFAULT_RETRIES = 3;
    static constexpr uint64_t DEFAULT_RETRY_WAIT_MS = 100;
    static constexpr float DEFAULT_RETRY_BACKOFF = 4;
    static constexpr bool DEFAULT_KEEP_ALIVE = true;
};

struct HTTPFileInfo : public common::FileInfo {
    HTTPFileInfo(std::string path, common::FileSystem* fileSystem, int flags,
        main::ClientContext* context);

    virtual void initialize(main::ClientContext* context);

    virtual void initializeClient();

    void initMetadata();

    // We keep a http client stored for connection reuse with keep-alive headers.
    std::unique_ptr<httplib::Client> httpClient;

    int flags;
    uint64_t length;
    uint64_t fileOffset;
    HTTPConfig httpConfig;
    std::unique_ptr<common::FileInfo> cachedFileInfo;

    // A small LRU cache of fetched byte ranges, keyed by their starting file
    // offset. Replaces the previous single sliding-window read buffer so that
    // revisited regions are served from memory instead of re-fetched.
    // All readCache mutations/lookups must hold `readCacheMtx`; concurrent
    // readers of the same remote file are possible.
    struct CachedBlock {
        uint64_t offset;
        uint64_t length;
        std::unique_ptr<uint8_t[]> data;
    };
    std::list<CachedBlock> readCache;
    mutable std::mutex readCacheMtx;

    // True when the remote content is immutable for this path (e.g. xet://
    // content-addressed datasets). Set by XetFileSystem::openFile. Gates use of
    // the persistent cross-run block cache without any freshness checks.
    bool immutableContent = false;

    // Returns a pointer to the cached block covering `pos` (i.e. offset <= pos <
    // offset+length), or nullptr. On a hit the block is promoted to the MRU
    // position. Caller must hold readCacheMtx.
    CachedBlock* lookupCachedBlock(uint64_t pos);
    // Inserts a freshly fetched block (taking ownership of `data`) at the MRU
    // position, evicting the LRU block when the cache is over capacity. Caller
    // must hold readCacheMtx.
    void cacheBlock(uint64_t offset, uint64_t length, std::unique_ptr<uint8_t[]> data);
};

class HTTPFileSystem : public common::FileSystem {
    friend struct HTTPFileInfo;

public:
    std::unique_ptr<common::FileInfo> openFile(const std::string& path, common::FileOpenFlags flags,
        main::ClientContext* context = nullptr) override;

    std::vector<std::string> glob(main::ClientContext* context,
        const std::string& path) const override;

    bool canHandleFile(const std::string_view path) const override;

    bool fileOrPathExists(const std::string& path, main::ClientContext* context = nullptr) override;

    static std::unique_ptr<httplib::Client> getClient(const std::string& host);

    static std::unique_ptr<httplib::Headers> getHTTPHeaders(HeaderMap& headerMap);

    void syncFile(const common::FileInfo& fileInfo) const override;

    static std::pair<std::string, std::string> parseUrl(const std::string& url);

    CachedFileManager& getCachedFileManager() { return *cachedFileManager; }

    void cleanUP(main::ClientContext* context) override;

    // Returns a process-wide shared no-follow httplib client for `host`, creating
    // and caching it on first use. Because each client owns one persistent
    // keep-alive TLS socket that is reused across requests, sharing a client per
    // host (instead of creating a fresh one per file info / per retry) is what
    // avoids a TCP+TLS reconnect on every request. Callers must serialize access
    // to a pooled client around a request via lockHttp()/unlockHttp().
    static httplib::Client* getSharedNoRedirectClient(const std::string& host);

    // Drops the pooled client for `host` (if present) and returns a freshly
    // created one, so a retry after a failed/dead connection does not keep
    // reusing the same broken socket for the rest of the run.
    static httplib::Client* evictAndGetSharedNoRedirectClient(const std::string& host);

    // Global lock guarding access to any pooled no-follow client. Requests that
    // use a pooled client must hold this for the duration of the request. It must
    // be released before handling a redirect (which recurses into another
    // lockHttp() on the same thread), otherwise the non-recursive mutex
    // self-deadlocks.
    static void lockHttp(const std::string& host);
    static void unlockHttp(const std::string& host);

    // Shared implementation of openFile() allowing scheme wrappers (XetFileSystem)
    // to declare their content immutable before initialization wires caches.
    // Available in all builds; the immutableContent flag only takes effect when
    // HTTPFS_REMOTE_READ_OPTIMIZATIONS is enabled.
    virtual std::unique_ptr<common::FileInfo> openFileWithImmutability(const std::string& path,
        common::FileOpenFlags flags, main::ClientContext* context, bool immutableContent);

protected:
    void readFromFile(common::FileInfo& fileInfo, void* buffer, uint64_t numBytes,
        uint64_t position) const override;

    int64_t readFile(common::FileInfo& fileInfo, void* buf, size_t numBytes) const override;

    int64_t seek(common::FileInfo& fileInfo, uint64_t offset, int whence) const override;

    uint64_t getFileSize(const common::FileInfo& fileInfo) const override;

    static std::unique_ptr<HTTPResponse> runRequestWithRetry(
        const std::function<httplib::Result(void)>& request, const std::string& url,
        std::string method, const std::function<void(void)>& retry = {});

    virtual std::unique_ptr<HTTPResponse> headRequest(common::FileInfo* fileInfo,
        const std::string& url, HeaderMap headerMap) const;

    virtual std::unique_ptr<HTTPResponse> getRangeRequest(common::FileInfo* fileInfo,
        const std::string& url, HeaderMap headerMap, uint64_t fileOffset, char* buffer,
        uint64_t bufferLen) const;

    virtual std::unique_ptr<HTTPResponse> postRequest(common::FileInfo* fileInfo,
        const std::string& url, HeaderMap headerMap, std::unique_ptr<uint8_t[]>& outputBuffer,
        uint64_t& outputBufferLen, const uint8_t* inputBuffer, uint64_t inputBufferLen,
        std::string params = "") const;

    virtual std::unique_ptr<HTTPResponse> putRequest(common::FileInfo* fileInfo,
        const std::string& url, HeaderMap headerMap, const uint8_t* inputBuffer,
        uint64_t inputBufferLen, std::string params = "") const;

    void initCachedFileManager(main::ClientContext* context);

private:
    std::unique_ptr<CachedFileManager> cachedFileManager;
    std::mutex cachedFileManagerMtx;

    // Process-wide HTTP connection pool, keyed by host. Each entry is a client
    // with one persistent keep-alive TLS socket reused across all requests to
    // that host. Guarded by httpPoolMtx. Access to each pooled client is
    // serialized by its own host lock (see lockHttp(host)); locks must NEVER be
    // held across a redirect recursion (which re-enters on the same thread).
    static std::unordered_map<std::string, std::unique_ptr<httplib::Client>> sharedNoRedirectClients;
    static std::mutex httpPoolMtx;
    static std::mutex hostLocksMtx; // guards hostLocks insertion
    static std::unordered_map<std::string, std::shared_ptr<std::mutex>> hostLocks;

    // Process-wide cache of remote file sizes keyed by URL. Parquet files are
    // immutable during a query, so the very expensive HEAD round trip used to
    // learn a file's size only needs to happen once per URL instead of once per
    // openFile(). Guarded by httpSizeCacheMtx.
    static std::unordered_map<std::string, uint64_t> httpSizeCache;
    static std::mutex httpSizeCacheMtx;

#if HTTPFS_REMOTE_READ_OPTIMIZATIONS
public:
    // -------- layered remote-read optimizations (see docs in block_cache.h) ---
    // L1 per-file in-memory LRU -> L2 process-wide prefetch landing zone ->
    // L3 persistent cross-run disk cache -> L4 network.
    //
    // (lockHttp(host)/unlockHttp(host) are declared above with the pooled
    // connection machinery.)

    // Lazily creates read-optimization layers rooted at the extension's local
    // directory (idempotent). `wantPersistentCache` enables the cross-run disk
    // layer; pool creation happens whenever prefetching is enabled by config.
    void ensureBlockCache(main::ClientContext* context, bool wantPersistentCache);

    // Submits speculative fetches for the aligned blocks starting at
    // `nextOffset` into the prefetch landing zone. No-op when disabled or when
    // everything relevant is already cached/in-flight.
    void submitPrefetch(const HTTPFileInfo& fileInfo, uint64_t nextOffset,
        uint64_t blockSize) const;

private:
    // A completed prefetched span kept for repeated consumption by multiple
    // scan threads that each own an HTTPFileInfo for the same remote file.
    struct ReadyEntry {
        std::shared_ptr<std::vector<char>> payload;
        uint64_t consumeCount = 0;
    };
    struct PrefetchZone {
        static constexpr size_t MAX_READY_ENTRIES = 64;
        std::mutex mtx;
        std::unordered_map<std::string, ReadyEntry> ready;
        std::unordered_set<std::string> inflight;
    };
    mutable std::mutex zoneMapMtx;
    mutable std::unordered_map<std::string, PrefetchZone> zones; // key: resolved URL
    PrefetchZone* zoneFor(const std::string& url) const;

    std::unique_ptr<PersistentBlockCache> blockCache;
    std::unique_ptr<PrefetchPool> prefetchPool;
    std::mutex initRemoteCachesMtx;
#endif
};

} // namespace httpfs_extension
} // namespace lbug
