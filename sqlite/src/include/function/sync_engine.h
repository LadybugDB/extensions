#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lbug {
namespace main {
class ClientContext;
}
namespace sqlite_extension {

// One sync tick over an attached SQLite database: tail the WAL, resolve
// row ops, and apply them as Cypher through the client context. Each
// executed statement runs in its own transaction (at-least-once
// semantics); the cursor row is written last and replays are idempotent,
// so crashes converge on retry.
// Sync identity is the normalized absolute file path. Syncing does not
// require ATTACH (the engine opens the file itself); in fact the same file
// must NOT be attached into the same graph while synced, because attached
// shadows share bare table names with the synced native tables and the
// catalog forbids the duplicate (CREATE would silently no-op and writes
// would bind the foreign shadow).
struct SyncStats {
    int64_t tables = 0;
    int64_t upserted = 0;
    int64_t deleted = 0;
    int64_t appliedFrames = 0;
    std::string status;
};

struct SyncResult {
    bool ok = false;
    std::string error;
    SyncStats stats;
};

class SyncEngine {
public:
    // dbPath is a SQLite file path (absolute or relative to the process
    // working directory). The engine opens it read-only itself.
    SyncResult sync(main::ClientContext* context, const std::string& dbPath);

    // Explicit registration (register_node/register_edge semantics):
    // upserts a mapping-catalog row that takes precedence over inference.
    SyncResult registerNode(main::ClientContext* context, const std::string& dbPath,
        const std::string& sqliteTable, const std::string& label, const std::string& idColumn);
    SyncResult registerEdge(main::ClientContext* context, const std::string& dbPath,
        const std::string& sqliteTable, const std::string& label, const std::string& fromCol,
        const std::string& toCol);
};

} // namespace sqlite_extension
} // namespace lbug
