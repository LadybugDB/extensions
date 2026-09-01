#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/file_system/file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "gtest/gtest.h"
#include "httpfs.h"
#include "httpfs_extension.h"
#include "httplib.h"
#include "main/client_context.h"
#include "main/connection.h"
#include "main/database.h"

using namespace lbug;
using namespace lbug::common;
using namespace lbug::httpfs_extension;

namespace {

// Serves a directory over HTTP with range-request support so the httpfs code
// paths can be exercised without network access.
class LocalHttpServer {
public:
    explicit LocalHttpServer(const std::string& mountDir) {
        if (!server_.set_mount_point("/", mountDir.c_str())) {
            throw std::runtime_error("Failed to mount directory " + mountDir);
        }
        server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response&) {
            if (req.method == "HEAD") {
                ++headCount_;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        // The vendored httplib enables SO_REUSEPORT by default on Linux, which
        // allows multiple sockets to bind the same port and load-balances
        // connections between them. When ctest runs these tests in parallel,
        // two processes' servers then share a port and requests land on the
        // wrong server, making request counts flaky (observed as headCount()==0
        // under `ctest -j10`). Restrict to SO_REUSEADDR so a second bind fails
        // and the loop below picks a genuinely free port instead.
        server_.set_socket_options([](socket_t sock) {
            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                sizeof(yes));
        });
        for (int port = 18123; port < 18153; ++port) {
            if (server_.bind_to_port("127.0.0.1", port)) {
                port_ = port;
                break;
            }
        }
        if (port_ == 0) {
            throw std::runtime_error("Failed to bind local http server");
        }
        thread_ = std::thread([this]() { server_.listen_after_bind(); });
        while (!server_.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~LocalHttpServer() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string urlFor(const std::string& fileName) const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/" + fileName;
    }

    int headCount() const { return headCount_; }

private:
    httplib::Server server_;
    std::thread thread_;
    std::atomic<int> headCount_{0};
    int port_ = 0;
};

std::string makeTestContent(size_t size) {
    std::string content;
    content.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        content.push_back(static_cast<char>(i % 251));
    }
    return content;
}

class HttpFileSystemTest : public ::testing::Test {
public:
    void SetUp() override {
        dir_ = ::testing::TempDir() + "httpfs_test";
        std::filesystem::create_directories(dir_);
        content_ = makeTestContent(100000);
        server_ = std::make_unique<LocalHttpServer>(dir_);

        database_ = std::make_unique<main::Database>(":memory:");
        connection_ = std::make_unique<main::Connection>(database_.get());
        context_ = connection_->getClientContext();
        HttpfsExtension::load(context_);
        vfs_ = VirtualFileSystem::GetUnsafe(*context_);
    }

    // Writes the fixture content under a unique name and points url_ at it.
    // Each test uses its own file name because the remote file size cache is
    // process-global and would otherwise leak across tests.
    void setupDataFile(const std::string& fileName) {
        std::ofstream out(dir_ + "/" + fileName, std::ios::binary | std::ios::trunc);
        out.write(content_.data(), static_cast<std::streamsize>(content_.size()));
        url_ = server_->urlFor(fileName);
    }

    void TearDown() override {
        connection_.reset();
        database_.reset();
        server_.reset();
    }

    std::unique_ptr<FileInfo> openUrl() {
        return vfs_->openFile(url_, FileOpenFlags(FileFlags::READ_ONLY), context_);
    }

protected:
    std::string content_;
    std::string dir_;
    std::string url_;
    std::unique_ptr<LocalHttpServer> server_;
    std::unique_ptr<main::Database> database_;
    std::unique_ptr<main::Connection> connection_;
    main::ClientContext* context_ = nullptr;
    VirtualFileSystem* vfs_ = nullptr;
};

// Regression test for https://github.com/LadybugDB/ladybug/issues/880:
// the second open of the same URL used to hit the process-wide size cache and
// return before initializing the HTTP client, so the first ranged read on that
// handle dereferenced a null httplib client and crashed.
TEST_F(HttpFileSystemTest, ReadAfterRepeatOpenOfSizeCachedUrl) {
    setupDataFile("data_repeat_open.bin");
    // First open: performs the HEAD request and populates the size cache.
    auto firstHandle = openUrl();
    ASSERT_EQ(content_.size(), firstHandle->getFileSize());

    std::vector<char> buf(content_.size());
    firstHandle->readFromFile(buf.data(), buf.size(), 0);
    EXPECT_EQ(0, std::memcmp(buf.data(), content_.data(), content_.size()));

    // Second open of the same URL: size-cache hit, HEAD is skipped.
    auto secondHandle = openUrl();
    ASSERT_EQ(content_.size(), secondHandle->getFileSize());

    // This read used to SIGSEGV because httpClient was left null.
    std::fill(buf.begin(), buf.end(), '\0');
    secondHandle->readFromFile(buf.data(), buf.size(), 0);
    EXPECT_EQ(0, std::memcmp(buf.data(), content_.data(), content_.size()));

    // And a read at a non-zero offset for good measure.
    secondHandle->readFromFile(buf.data(), 4096, content_.size() - 4096);
    EXPECT_EQ(0, std::memcmp(buf.data(), content_.data() + content_.size() - 4096, 4096));
}

TEST_F(HttpFileSystemTest, RepeatOpenDoesNotIssueSecondHead) {
    setupDataFile("data_head_count.bin");
    // The size cache exists to avoid a HEAD per open; make sure the client
    // initialization fix did not reintroduce the HEAD round trip.
    auto firstHandle = openUrl();
    ASSERT_EQ(content_.size(), firstHandle->getFileSize());
    auto secondHandle = openUrl();
    ASSERT_EQ(content_.size(), secondHandle->getFileSize());
    EXPECT_EQ(1, server_->headCount());
}

} // namespace
