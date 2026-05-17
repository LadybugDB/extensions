#pragma once

#include "httpfs.h"

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
};

} // namespace httpfs_extension
} // namespace lbug
