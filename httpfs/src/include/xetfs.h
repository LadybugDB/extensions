#pragma once

#include "common/types/types.h"
#include "httpfs.h"
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace lbug {
namespace httpfs_extension {

class XetFileSystem final : public HTTPFileSystem {
public:
    std::unique_ptr<common::FileInfo> openFile(const std::string& path, common::FileOpenFlags flags,
        main::ClientContext* context = nullptr) override;

    std::vector<std::string> glob(main::ClientContext* context,
        const std::string& path) const override;

    bool canHandleFile(const std::string_view path) const override;

    bool fileOrPathExists(const std::string& path, main::ClientContext* context = nullptr) override;

    static std::string toHuggingFaceURL(const std::string& path);

protected:
    std::unique_ptr<HTTPResponse> headRequest(common::FileInfo* fileInfo, const std::string& url,
        HeaderMap headerMap) const override;

    std::unique_ptr<HTTPResponse> getRangeRequest(common::FileInfo* fileInfo,
        const std::string& url, HeaderMap headerMap, uint64_t fileOffset, char* buffer,
        uint64_t bufferLen) const override;

private:
    // Resolved-redirect targets are process-wide: the signed URLs for a given
    // source path are stable across HTTPFileInfo instances. Guarded by
    // resolvedTargetsMtx; entries expire after RESOLVE_TARGET_TTL so signature
    // rotation cannot poison reads for long.
    static constexpr std::chrono::seconds RESOLVE_TARGET_TTL{120};
    struct ResolvedTarget {
        std::string targetUrl;
        std::chrono::steady_clock::time_point expiresAt;
    };
    static bool takeMemoizedResolveTarget(const std::string& url, std::string& targetOut);
    static void recordMemoizedResolveTarget(const std::string& url, const std::string& target);
    static void evictMemoizedResolveTarget(const std::string& url);
    static std::mutex resolvedTargetsMtx;
    static std::unordered_map<std::string, ResolvedTarget> resolvedTargets;

    /** Issues one range request against `url`, following 3xx hops manually with
     * per-host locking. Records the FIRST redirect hop of `originalUrl` into
     * the memoization cache. `buffer` may be null to fetch without copying. */
    std::unique_ptr<HTTPResponse> getRangeFollowingRedirects(common::FileInfo* fileInfo,
        const std::string& url, const std::string& originalUrl, HeaderMap headerMap,
        uint64_t fileOffset, char* buffer, uint64_t bufferLen, int redirectsRemaining) const;
};

} // namespace httpfs_extension
} // namespace lbug
