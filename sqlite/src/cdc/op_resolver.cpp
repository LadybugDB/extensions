#include "cdc/op_resolver.h"

#include <algorithm>
#include <set>
#include <utility>

#include <format>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

namespace {

// FNV-1a/64 over a page image: cheap change detection for reconcile().
uint64_t hashImage(const uint8_t* data, size_t size) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; i++) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

const TableSchema* findSchema(const std::vector<TableSchema>& schemas, const std::string& name) {
    for (auto& s : schemas) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

void OpResolver::trackRow(TableState& state, const DecodedRow& row) {
    if (!state.pkFromRowid && state.pkRecordIndex >= 0 &&
        static_cast<size_t>(state.pkRecordIndex) < row.values.size()) {
        state.identities[row.rowid] = row.values[state.pkRecordIndex];
    }
    if (state.retainRecords) {
        state.records[row.rowid] = row.values;
    }
}

void OpResolver::emitDelete(const std::string& table, TableState& state, int64_t rowid,
    OpBatch& batch) {
    RowDelete del;
    del.table = table;
    del.rowid = rowid;
    auto it = state.identities.find(rowid);
    if (it != state.identities.end()) {
        del.identity = it->second;
        del.hasIdentity = true;
        state.identities.erase(it);
    }
    auto rit = state.records.find(rowid);
    if (rit != state.records.end()) {
        del.record = std::move(rit->second);
        del.hasRecord = true;
        state.records.erase(rit);
    }
    batch.deletes.push_back(std::move(del));
}

bool OpResolver::decodeLeaf(uint32_t pageNo, const PageFetcher& fetcher, uint32_t pageSize,
    std::vector<DecodedRow>& rows, uint64_t& imageHash, std::string& error) {
    const uint8_t* image = fetcher(pageNo);
    if (image == nullptr) {
        error = std::format("Page {} unavailable", pageNo);
        return false;
    }
    imageHash = hashImage(image, pageSize);
    auto decoded = decodeTableLeafPage(image, pageSize, pageSize, 0, 1, fetcher);
    if (!decoded.ok) {
        error = decoded.error;
        return false;
    }
    rows = std::move(decoded.rows);
    return true;
}

ResolveResult OpResolver::bootstrap(const std::vector<TableSchema>& schemas,
    const PageFetcher& fetcher, uint32_t pageSize) {
    return fullDiff(schemas, fetcher, pageSize, true /* blind */);
}

ResolveResult OpResolver::reconcile(const std::vector<TableSchema>& schemas,
    const PageFetcher& fetcher, uint32_t pageSize) {
    return fullDiff(schemas, fetcher, pageSize, false /* blind */);
}

ResolveResult OpResolver::fullDiff(const std::vector<TableSchema>& schemas,
    const PageFetcher& fetcher, uint32_t pageSize, bool blind) {
    ResolveResult out;
    // Drop state for tables that no longer exist; every tracked row becomes
    // a delete so the applier converges.
    for (auto it = tables_.begin(); it != tables_.end();) {
        if (findSchema(schemas, it->first) == nullptr) {
            out.batch.droppedTables.push_back(it->first);
            for (auto& [pageNo, page] : it->second.pages) {
                for (auto rowid : page.rowids) {
                    emitDelete(it->first, it->second, rowid, out.batch);
                }
            }
            it = tables_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& schema : schemas) {
        if (schema.withoutRowid) {
            tables_.erase(schema.name);
            out.batch.skippedTables.push_back(schema.name);
            continue;
        }
        auto walk = collectLeafPages(schema.rootPage, fetcher, pageSize);
        if (!walk.ok) {
            out.error = walk.error;
            return out;
        }
        TableState newState;
        newState.rootPage = schema.rootPage;
        newState.pkRecordIndex = schema.pkRecordIndex;
        newState.pkFromRowid = schema.pkFromRowid;
        newState.retainRecords = schema.retainRecords;
        auto stateIt = tables_.find(schema.name);
        bool haveState =
            !blind && stateIt != tables_.end() && stateIt->second.rootPage == schema.rootPage;
        TableState* oldState = haveState ? &stateIt->second : nullptr;
        std::set<std::pair<std::string, int64_t>> upserted; // determinism
        std::vector<RowOp> tableUpserts;
        for (auto pageNo : walk.leaves) {
            const uint8_t* image = fetcher(pageNo);
            if (image == nullptr) {
                out.error = std::format("Page {} unavailable", pageNo);
                return out;
            }
            uint64_t hash = hashImage(image, pageSize);
            auto oldPageIt = oldState != nullptr ? oldState->pages.find(pageNo) :
                                                   decltype(oldState->pages)::const_iterator{};
            bool pageKnown = oldState != nullptr && oldPageIt != oldState->pages.end();
            if (pageKnown && oldPageIt->second.imageHash == hash) {
                newState.pages[pageNo] = oldPageIt->second;
                continue;
            }
            std::vector<DecodedRow> rows;
            uint64_t decodedHash = 0;
            std::string error;
            if (!decodeLeaf(pageNo, fetcher, pageSize, rows, decodedHash, error)) {
                out.error = error;
                return out;
            }
            PageState pageState;
            pageState.imageHash = decodedHash;
            for (auto& row : rows) {
                pageState.rowids.insert(row.rowid);
                // Changed pages re-emit all current rows (idempotent
                // upserts at apply time); identities refresh for deletes.
                trackRow(newState, row);
                RowOp op;
                op.table = schema.name;
                op.rowid = row.rowid;
                op.values = row.values;
                tableUpserts.push_back(std::move(op));
                upserted.emplace(schema.name, row.rowid);
            }
            newState.pages[pageNo] = std::move(pageState);
        }
        if (oldState != nullptr) {
            // Carry retained identities/records forward so deletes of rows
            // that vanished before this diff still resolve.
            for (auto& [rowid, identity] : oldState->identities) {
                newState.identities.emplace(rowid, identity);
            }
            for (auto& [rowid, record] : oldState->records) {
                newState.records.emplace(rowid, record);
            }
            for (auto& [pageNo, page] : oldState->pages) {
                auto newPageIt = newState.pages.find(pageNo);
                for (auto rowid : page.rowids) {
                    bool stillPresent = newPageIt != newState.pages.end() &&
                                        newPageIt->second.rowids.contains(rowid);
                    if (!stillPresent && !upserted.contains({schema.name, rowid})) {
                        emitDelete(schema.name, newState, rowid, out.batch);
                    }
                }
            }
        }
        // Root change (VACUUM/rebuild) with prior state: rows on removed
        // pages were emitted above via the old-state diff.
        tables_[schema.name] = std::move(newState);
        out.batch.upserts.insert(out.batch.upserts.end(), tableUpserts.begin(), tableUpserts.end());
    }
    auto byTableRowid = [](const auto& a, const auto& b) {
        return a.table != b.table ? a.table < b.table : a.rowid < b.rowid;
    };
    std::sort(out.batch.upserts.begin(), out.batch.upserts.end(), byTableRowid);
    std::sort(out.batch.deletes.begin(), out.batch.deletes.end(), byTableRowid);
    std::sort(out.batch.droppedTables.begin(), out.batch.droppedTables.end());
    std::sort(out.batch.skippedTables.begin(), out.batch.skippedTables.end());
    out.ok = true;
    return out;
}

ResolveResult OpResolver::applyFrames(const std::vector<TableSchema>& schemas,
    const std::vector<uint32_t>& dirtyPages, const PageFetcher& fetcher, uint32_t pageSize) {
    ResolveResult out;
    std::unordered_set<uint32_t> dirty(dirtyPages.begin(), dirtyPages.end());
    // Tables that vanished: drop state, emit deletes for tracked rows.
    for (auto it = tables_.begin(); it != tables_.end();) {
        if (findSchema(schemas, it->first) == nullptr) {
            out.batch.droppedTables.push_back(it->first);
            for (auto& [pageNo, page] : it->second.pages) {
                for (auto rowid : page.rowids) {
                    emitDelete(it->first, it->second, rowid, out.batch);
                }
            }
            it = tables_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& schema : schemas) {
        if (schema.withoutRowid) {
            tables_.erase(schema.name);
            out.batch.skippedTables.push_back(schema.name);
            continue;
        }
        auto stateIt = tables_.find(schema.name);
        if (stateIt == tables_.end() || stateIt->second.rootPage != schema.rootPage) {
            // New table, or rebuilt root: bootstrap just this table. Rows
            // from a dropped incarnation were emitted above when prior
            // state existed under a different root... except the state key
            // matched but the root differs: emit old rows as deletes first.
            if (stateIt != tables_.end()) {
                for (auto& [pageNo, page] : stateIt->second.pages) {
                    for (auto rowid : page.rowids) {
                        emitDelete(schema.name, stateIt->second, rowid, out.batch);
                    }
                }
                tables_.erase(stateIt);
            }
            auto walk = collectLeafPages(schema.rootPage, fetcher, pageSize);
            if (!walk.ok) {
                out.error = walk.error;
                return out;
            }
            TableState fresh;
            fresh.rootPage = schema.rootPage;
            fresh.pkRecordIndex = schema.pkRecordIndex;
            fresh.pkFromRowid = schema.pkFromRowid;
            fresh.retainRecords = schema.retainRecords;
            for (auto pageNo : walk.leaves) {
                std::vector<DecodedRow> rows;
                uint64_t hash = 0;
                std::string error;
                if (!decodeLeaf(pageNo, fetcher, pageSize, rows, hash, error)) {
                    out.error = error;
                    return out;
                }
                PageState pageState;
                pageState.imageHash = hash;
                for (auto& row : rows) {
                    pageState.rowids.insert(row.rowid);
                    trackRow(fresh, row);
                    RowOp op;
                    op.table = schema.name;
                    op.rowid = row.rowid;
                    op.values = row.values;
                    out.batch.upserts.push_back(std::move(op));
                }
                fresh.pages[pageNo] = std::move(pageState);
            }
            tables_[schema.name] = std::move(fresh);
            continue;
        }
        TableState& state = stateIt->second;
        auto walk = collectLeafPages(schema.rootPage, fetcher, pageSize);
        if (!walk.ok) {
            out.error = walk.error;
            return out;
        }
        std::set<int64_t> upsertedRowids;
        auto emitRows = [&](const std::vector<DecodedRow>& rows) {
            for (auto& row : rows) {
                RowOp op;
                op.table = schema.name;
                op.rowid = row.rowid;
                op.values = row.values;
                out.batch.upserts.push_back(std::move(op));
                upsertedRowids.insert(row.rowid);
            }
        };
        // Removed pages: all tracked rowids are candidate deletes.
        for (auto it = state.pages.begin(); it != state.pages.end();) {
            if (!walk.leaves.contains(it->first)) {
                for (auto rowid : it->second.rowids) {
                    if (!upsertedRowids.contains(rowid)) {
                        emitDelete(schema.name, state, rowid, out.batch);
                    }
                }
                it = state.pages.erase(it);
            } else {
                ++it;
            }
        }
        for (auto pageNo : walk.leaves) {
            auto pageIt = state.pages.find(pageNo);
            bool isNew = pageIt == state.pages.end();
            if (!isNew && !dirty.contains(pageNo)) {
                continue; // Clean page: state already current.
            }
            std::vector<DecodedRow> rows;
            uint64_t hash = 0;
            std::string error;
            if (!decodeLeaf(pageNo, fetcher, pageSize, rows, hash, error)) {
                out.error = error;
                return out;
            }
            PageState fresh;
            fresh.imageHash = hash;
            for (auto& row : rows) {
                fresh.rowids.insert(row.rowid);
                trackRow(state, row);
            }
            if (!isNew) {
                for (auto rowid : pageIt->second.rowids) {
                    if (!fresh.rowids.contains(rowid) && !upsertedRowids.contains(rowid)) {
                        emitDelete(schema.name, state, rowid, out.batch);
                    }
                }
            }
            emitRows(rows);
            state.pages[pageNo] = std::move(fresh);
        }
        // A row deleted from page A but re-inserted (moved) on page B in
        // the same batch was already emitted as a delete above; retract it:
        // the upsert wins because the row provably exists post-commit.
        out.batch.deletes.erase(std::remove_if(out.batch.deletes.begin(), out.batch.deletes.end(),
                                    [&](const RowDelete& d) {
                                        return d.table == schema.name &&
                                               upsertedRowids.contains(d.rowid);
                                    }),
            out.batch.deletes.end());
    }
    auto byTableRowid = [](const auto& a, const auto& b) {
        return a.table != b.table ? a.table < b.table : a.rowid < b.rowid;
    };
    std::sort(out.batch.upserts.begin(), out.batch.upserts.end(), byTableRowid);
    std::sort(out.batch.deletes.begin(), out.batch.deletes.end(), byTableRowid);
    std::sort(out.batch.droppedTables.begin(), out.batch.droppedTables.end());
    std::sort(out.batch.skippedTables.begin(), out.batch.skippedTables.end());
    out.ok = true;
    return out;
}

size_t OpResolver::trackedRowCount(const std::string& table) const {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        return 0;
    }
    size_t n = 0;
    for (auto& [pageNo, page] : it->second.pages) {
        n += page.rowids.size();
    }
    return n;
}

std::vector<std::string> OpResolver::trackedTables() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (auto& [name, state] : tables_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
