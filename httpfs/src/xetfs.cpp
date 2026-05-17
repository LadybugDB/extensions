#include "xetfs.h"

#include "common/exception/io.h"
#include "common/string_utils.h"
#include <format>

namespace lbug {
namespace httpfs_extension {

using namespace common;

namespace {

static constexpr std::string_view XET_PREFIX = "xet://";
static constexpr std::string_view HF_BASE_URL = "https://huggingface.co/";

std::vector<std::string> splitPath(std::string_view path) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        result.emplace_back(path.substr(start, end - start));
        start = end + 1;
        if (end == path.size()) {
            break;
        }
    }
    return result;
}

std::string joinSegments(const std::vector<std::string>& segments, size_t start) {
    std::string result;
    for (auto i = start; i < segments.size(); ++i) {
        if (!result.empty()) {
            result += "/";
        }
        result += segments[i];
    }
    return result;
}

std::string buildResolveURL(std::string_view repoPrefix, const std::vector<std::string>& segments) {
    if (segments.size() < 4) {
        throw IOException{
            "Xet URL must include namespace, repository, revision, and file path components."};
    }
    const auto filePath = joinSegments(segments, 3);
    if (filePath.empty()) {
        throw IOException{"Xet URL must include a file path."};
    }
    return std::format("{}{}{}{}/{}/resolve/{}/{}", HF_BASE_URL, repoPrefix,
        repoPrefix.empty() ? "" : "/", segments[0], segments[1], segments[2], filePath);
}

std::string buildResolveURLWithExplicitResolve(std::string_view repoPrefix,
    const std::vector<std::string>& segments) {
    if (segments.size() < 5 || segments[2] != "resolve") {
        return buildResolveURL(repoPrefix, segments);
    }
    const auto filePath = joinSegments(segments, 4);
    if (filePath.empty()) {
        throw IOException{"Xet URL must include a file path."};
    }
    return std::format("{}{}{}{}/{}/resolve/{}/{}", HF_BASE_URL, repoPrefix,
        repoPrefix.empty() ? "" : "/", segments[0], segments[1], segments[3], filePath);
}

std::string makeAbsoluteRedirectURL(const std::string& sourceURL, const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    const auto [host, hostPath] = HTTPFileSystem::parseUrl(sourceURL);
    if (location.starts_with('/')) {
        return host + location;
    }
    const auto lastSlash = hostPath.find_last_of('/');
    const auto basePath =
        lastSlash == std::string::npos ? std::string{"/"} : hostPath.substr(0, lastSlash + 1);
    return host + basePath + location;
}

std::unique_ptr<httplib::Client> getNoRedirectClient(const std::string& host) {
    auto client = HTTPFileSystem::getClient(host);
    client->set_follow_location(false);
    client->set_url_encode(false);
    return client;
}

std::unique_ptr<HTTPResponse> synthesizeHeadResponse(const HTTPResponse& response,
    const std::string& url, const std::string& contentLength) {
    httplib::Response res;
    res.status = 200;
    res.reason = "OK";
    for (auto& [name, value] : response.headers) {
        if (StringUtils::caseInsensitiveEquals(name, "Content-Length")) {
            continue;
        }
        res.headers.emplace(name, value);
    }
    res.headers.emplace("Content-Length", contentLength);
    return std::make_unique<HTTPResponse>(res, url);
}

} // namespace

std::unique_ptr<common::FileInfo> XetFileSystem::openFile(const std::string& path,
    common::FileOpenFlags flags, main::ClientContext* context) {
    if (flags.flags & FileFlags::WRITE) {
        throw IOException{"Writing to xet:// URLs is not supported."};
    }
    return HTTPFileSystem::openFile(toHuggingFaceURL(path), flags, context);
}

std::vector<std::string> XetFileSystem::glob(main::ClientContext* /*context*/,
    const std::string& path) const {
    // Keep xet:// paths routed to XetFileSystem after bind-time glob expansion.
    return {path};
}

bool XetFileSystem::canHandleFile(const std::string_view path) const {
    return path.rfind(XET_PREFIX, 0) == 0;
}

bool XetFileSystem::fileOrPathExists(const std::string& path, main::ClientContext* context) {
    return HTTPFileSystem::fileOrPathExists(toHuggingFaceURL(path), context);
}

std::string XetFileSystem::toHuggingFaceURL(const std::string& path) {
    if (path.rfind(XET_PREFIX, 0) != 0) {
        throw IOException{"Xet URL needs to start with xet://"};
    }

    auto suffix = std::string_view{path}.substr(XET_PREFIX.size());
    if (suffix.rfind("huggingface.co/", 0) == 0) {
        return std::format("{}{}", HF_BASE_URL,
            suffix.substr(std::string_view{"huggingface.co/"}.size()));
    }
    if (suffix.rfind("hf.co/", 0) == 0) {
        return std::format("{}{}", HF_BASE_URL, suffix.substr(std::string_view{"hf.co/"}.size()));
    }

    const auto segments = splitPath(suffix);
    if (segments.empty() || segments[0].empty()) {
        throw IOException{"Xet URL must include a Hugging Face repository path."};
    }
    if (segments[0] == "models" || segments[0] == "model") {
        return buildResolveURLWithExplicitResolve("",
            std::vector<std::string>{segments.begin() + 1, segments.end()});
    }
    if (segments[0] == "datasets" || segments[0] == "dataset") {
        return buildResolveURLWithExplicitResolve("datasets",
            std::vector<std::string>{segments.begin() + 1, segments.end()});
    }
    if (segments[0] == "spaces" || segments[0] == "space") {
        return buildResolveURLWithExplicitResolve("spaces",
            std::vector<std::string>{segments.begin() + 1, segments.end()});
    }
    return buildResolveURLWithExplicitResolve("", segments);
}

std::unique_ptr<HTTPResponse> XetFileSystem::headRequest(common::FileInfo* /*fileInfo*/,
    const std::string& url, HeaderMap headerMap) const {
    const auto [host, hostPath] = HTTPFileSystem::parseUrl(url);
    auto headers = getHTTPHeaders(headerMap);
    auto client = getNoRedirectClient(host);

    std::function<httplib::Result(void)> request(
        [&]() { return client->Head(hostPath.c_str(), *headers); });
    std::function<void(void)> retry([&]() { client = getNoRedirectClient(host); });

    auto response = runRequestWithRetry(request, url, "HEAD", retry);
    if (response->code >= 300 && response->code < 400 &&
        response->headers.contains("x-linked-size")) {
        return synthesizeHeadResponse(*response, url, response->headers["x-linked-size"]);
    }
    if (response->code >= 300 && response->code < 400 && response->headers.contains("Location")) {
        return headRequest(nullptr, makeAbsoluteRedirectURL(url, response->headers["Location"]),
            headerMap);
    }
    return response;
}

std::unique_ptr<HTTPResponse> XetFileSystem::getRangeRequest(common::FileInfo* /*fileInfo*/,
    const std::string& url, HeaderMap headerMap, uint64_t fileOffset, char* buffer,
    uint64_t bufferLen) const {
    const auto [host, hostPath] = HTTPFileSystem::parseUrl(url);
    auto headers = getHTTPHeaders(headerMap);
    headers->insert(std::make_pair("Range",
        std::format("bytes={}-{}", fileOffset, fileOffset + bufferLen - 1)));
    auto client = getNoRedirectClient(host);

    std::function<httplib::Result(void)> request(
        [&]() { return client->Get(hostPath.c_str(), *headers); });
    std::function<void(void)> retry([&]() { client = getNoRedirectClient(host); });

    auto response = runRequestWithRetry(request, url, "GET Range", retry);
    if (response->code >= 300 && response->code < 400 && response->headers.contains("Location")) {
        return getRangeRequest(nullptr, makeAbsoluteRedirectURL(url, response->headers["Location"]),
            headerMap, fileOffset, buffer, bufferLen);
    }
    if (response->code >= 400) {
        throw IOException(std::format("HTTP GET error on '{}' (HTTP {})", url, response->code));
    }
    if (response->code < 300) {
        if (response->body.size() != bufferLen) {
            throw IOException(std::format("HTTP GET error: response body size {} does not match "
                                          "requested range size {}.",
                response->body.size(), bufferLen));
        }
        if (buffer != nullptr) {
            memcpy(buffer, response->body.data(), bufferLen);
        }
    }
    return response;
}

} // namespace httpfs_extension
} // namespace lbug
