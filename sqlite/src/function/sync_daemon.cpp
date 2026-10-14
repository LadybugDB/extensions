#include "function/sync_daemon.h"

#include <chrono>
#include <unordered_map>

#include "common/exception/exception.h"
#include "main/client_context.h"
#include "main/database.h"
#include <format>

namespace lbug {
namespace sqlite_extension {

namespace {

std::mutex daemonsMtx;
std::unordered_map<std::string, std::unique_ptr<SyncDaemon>> daemons;

constexpr int64_t DEFAULT_INTERVAL_MS = 1000;
constexpr int64_t MIN_INTERVAL_MS = 10;

} // namespace

SyncDaemon::SyncDaemon(main::Database* db, std::string normPath, int64_t intervalMs)
    : normPath{std::move(normPath)}, intervalMs{intervalMs} {
    // Persistent session for all ticks: sequential use on one thread, no
    // contention. Liveness probes go through it (they throw once the
    // database is closed).
    tickContext = std::make_unique<main::ClientContext>(db);
}

SyncResult SyncDaemon::start(main::Database* db, const std::string& path, int64_t intervalMs) {
    SyncResult out;
    if (db == nullptr) {
        out.error = "No database";
        return out;
    }
    if (intervalMs <= 0) {
        intervalMs = DEFAULT_INTERVAL_MS;
    }
    if (intervalMs < MIN_INTERVAL_MS) {
        intervalMs = MIN_INTERVAL_MS;
    }
    std::lock_guard<std::mutex> lock(daemonsMtx);
    if (daemons.contains(path)) {
        out.error = std::format("Sync daemon already running for '{}'", path);
        return out;
    }
    auto daemon = std::unique_ptr<SyncDaemon>(new SyncDaemon(db, path, intervalMs));
    daemon->worker = std::thread([raw = daemon.get()] { raw->run(); });
    daemons[path] = std::move(daemon);
    out.ok = true;
    out.stats.status = std::format("Sync daemon started for '{}' every {}ms", path, intervalMs);
    return out;
}

SyncResult SyncDaemon::stop(const std::string& path) {
    SyncResult out;
    std::unique_ptr<SyncDaemon> daemon;
    {
        std::lock_guard<std::mutex> lock(daemonsMtx);
        auto it = daemons.find(path);
        if (it == daemons.end()) {
            out.error = std::format("No sync daemon running for '{}'", path);
            return out;
        }
        it->second->stopFlag.store(true);
        daemon = std::move(it->second);
        daemons.erase(it);
    }
    if (daemon->worker.joinable()) {
        daemon->worker.join();
    }
    out.ok = true;
    out.stats.status = std::format("Sync daemon stopped for '{}'", path);
    return out;
}

std::vector<DaemonStatusRow> SyncDaemon::status() {
    std::lock_guard<std::mutex> lock(daemonsMtx);
    std::vector<DaemonStatusRow> rows;
    for (auto& [path, daemon] : daemons) {
        DaemonStatusRow row;
        row.sync = path;
        row.running = !daemon->stopFlag.load();
        row.intervalMs = daemon->intervalMs;
        std::lock_guard<std::mutex> stateLock(daemon->stateMtx);
        row.appliedFrames = daemon->lastStats.appliedFrames;
        row.upserted = daemon->lastStats.upserted;
        row.deleted = daemon->lastStats.deleted;
        row.consecutiveErrors = daemon->consecutiveErrors;
        row.lastError = daemon->lastError;
        rows.push_back(std::move(row));
    }
    return rows;
}

void SyncDaemon::run() {
    while (!stopFlag.load()) {
        // Liveness probe doubles as the close detector: it throws once
        // the database is gone, ending the thread before teardown races.
        try {
            auto probe = tickContext->query("RETURN 1");
            if (!probe->isSuccess()) {
                break;
            }
        } catch (const std::exception&) {
            break;
        }
        tickOnce();
        // Interruptible sleep so stop() joins promptly.
        int64_t waited = 0;
        int64_t waitTarget = intervalMs;
        {
            std::lock_guard<std::mutex> lock(stateMtx);
            if (consecutiveErrors > 0) {
                waitTarget = std::min<int64_t>(intervalMs * 5, 30000);
            }
        }
        while (!stopFlag.load() && waited < waitTarget) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waited += 10;
        }
    }
}

void SyncDaemon::tickOnce() {
    // Ticks run on the daemon's own session context, never the CALL's
    // (whose mutex is held by the running CALL).
    SyncEngine engine;
    SyncResult result;
    try {
        result = engine.sync(tickContext.get(), normPath);
    } catch (const common::Exception& e) {
        result.error = e.what();
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    std::lock_guard<std::mutex> lock(stateMtx);
    if (result.ok) {
        lastStats = result.stats;
        consecutiveErrors = 0;
        lastError.clear();
    } else {
        consecutiveErrors++;
        lastError = result.error;
    }
}

} // namespace sqlite_extension
} // namespace lbug
