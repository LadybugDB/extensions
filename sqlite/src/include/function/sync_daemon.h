#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "function/sync_engine.h"

namespace lbug {
namespace main {
class ClientContext;
class Database;
} // namespace main
namespace sqlite_extension {

// Background sync daemon: one thread per synced file, each tick running
// the same SyncEngine::sync as a manual CALL. Lives in process memory;
// restart it after a process restart (the cursor is durable, resolver
// memory is not -- same caveat as manual sync).

struct DaemonStatusRow {
    std::string sync;
    bool running = false;
    int64_t intervalMs = 0;
    int64_t appliedFrames = 0;
    int64_t upserted = 0;
    int64_t deleted = 0;
    int64_t consecutiveErrors = 0;
    std::string lastError;
};

class SyncDaemon {
public:
    // Starts ticking path every intervalMs (must be > 0). Fails when a
    // daemon for path already runs.
    static SyncResult start(main::Database* db, const std::string& path, int64_t intervalMs);
    // Stops the daemon for path (joins the thread). Fails when absent.
    static SyncResult stop(const std::string& path);
    static std::vector<DaemonStatusRow> status();

private:
    SyncDaemon(main::Database* db, std::string normPath, int64_t intervalMs);

    void run();
    void tickOnce();

    std::string normPath;
    int64_t intervalMs;
    std::atomic<bool> stopFlag{false};
    std::thread worker;
    // Persistent session for all ticks (see .cpp).
    std::unique_ptr<main::ClientContext> tickContext;
    mutable std::mutex stateMtx;
    SyncStats lastStats;
    int64_t consecutiveErrors = 0;
    std::string lastError;
};

} // namespace sqlite_extension
} // namespace lbug
