#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cdc/btree_page_decoder.h"
#include "cdc/wal_tailer.h"

namespace lbug {
namespace sqlite_extension {
namespace cdc {

// Table identity for the resolver. Only rowid tables are supported;
// WITHOUT ROWID tables are reported in OpBatch::skippedTables.
struct TableSchema {
    std::string name;
    uint32_t rootPage = 0;
    bool withoutRowid = false;

    bool operator==(const TableSchema& other) const {
        return name == other.name && rootPage == other.rootPage &&
               withoutRowid == other.withoutRowid;
    }
};

struct RowOp {
    std::string table;
    int64_t rowid = 0;
    // Record values in schema column order. The slot at an INTEGER PRIMARY
    // KEY position holds NULL; the rowid is authoritative there.
    std::vector<SqliteValue> values;
};

struct RowDelete {
    std::string table;
    int64_t rowid = 0;
};

struct OpBatch {
    std::vector<RowOp> upserts;
    std::vector<RowDelete> deletes;
    std::vector<std::string> droppedTables;
    std::vector<std::string> skippedTables;
};

struct ResolveResult {
    bool ok = false;
    std::string error;
    OpBatch batch;
};

// Replication cursor: WAL salts identify the log generation, appliedFrames
// counts verified frames consumed from it. Advanced transactionally with
// the applied ops (phase 3); the resolver itself is stateless w.r.t. it.
struct WalClock {
    uint32_t salt1 = 0;
    uint32_t salt2 = 0;
    size_t appliedFrames = 0;
};

// True when the WAL restarted since the clock was recorded (checkpoint,
// clean shutdown, wraparound): frames may have been folded into the DB
// unseen, so the caller must reconcile() instead of applying incrementally.
// A zero clock means "never synced": bootstrap(), not reconcile().
inline bool walRestarted(const WalClock& clock, const WalHeader& header) {
    if (clock.salt1 == 0 && clock.salt2 == 0) {
        return false;
    }
    return clock.salt1 != header.salt1 || clock.salt2 != header.salt2;
}

// Resolves WAL page images into row ops without table scans. Tracks
// pageNo -> {rowids} per table (plus a page-image hash); per commit it
// decodes only dirty leaf pages and diffs page rowid sets for deletes.
// Identity is the rowid for rowid tables (== the INTEGER PRIMARY KEY
// value when one exists); the applier maps that to Ladybug keys.
class OpResolver {
public:
    // Full snapshot: decodes every leaf of every schema table, emits all
    // rows as upserts. Clears previous state. The only full scan, ever
    // (Debezium snapshot phase equivalent).
    ResolveResult bootstrap(const std::vector<TableSchema>& schemas, const PageFetcher& fetcher,
        uint32_t pageSize);

    // Incremental step. dirtyPages are page numbers touched since the last
    // call (union of the commit frames' page numbers); fetcher must serve
    // post-commit newest images. Treated atomically: upserts collected
    // first, deletes computed as (old sets - new sets) minus rowids
    // upserted anywhere in the batch, so intra-commit page moves
    // reconcile. Output is sorted by (table, rowid) for determinism.
    ResolveResult applyFrames(const std::vector<TableSchema>& schemas,
        const std::vector<uint32_t>& dirtyPages, const PageFetcher& fetcher, uint32_t pageSize);

    // Full re-diff after a WAL restart: walks current state and emits only
    // genuine differences. Pages whose image hash matches are skipped
    // without decoding, so a no-op restart reconciles nearly free.
    ResolveResult reconcile(const std::vector<TableSchema>& schemas, const PageFetcher& fetcher,
        uint32_t pageSize);

    size_t trackedRowCount(const std::string& table) const;
    std::vector<std::string> trackedTables() const;

private:
    struct PageState {
        std::unordered_set<int64_t> rowids;
        uint64_t imageHash = 0;
    };
    struct TableState {
        uint32_t rootPage = 0;
        std::unordered_map<uint32_t, PageState> pages;
    };
    std::unordered_map<std::string, TableState> tables_;

    static bool decodeLeaf(uint32_t pageNo, const PageFetcher& fetcher, uint32_t pageSize,
        std::vector<DecodedRow>& rows, uint64_t& imageHash, std::string& error);
    ResolveResult fullDiff(const std::vector<TableSchema>& schemas, const PageFetcher& fetcher,
        uint32_t pageSize, bool blind);
};

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
