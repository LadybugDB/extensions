#include "function/sync_engine.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "cdc/op_resolver.h"
#include "cdc/sync_cypher.h"
#include "cdc/sync_mapping.h"
#include "cdc/wal_tailer.h"
#include "common/exception/runtime.h"
#include "common/types/value/value.h"
#include "connector/sqlite_connector.h"
#include "main/attached_database.h"
#include "main/client_context.h"
#include "main/database.h"
#include "main/database_manager.h"
#include "processor/result/flat_tuple.h"
#include "storage/attached_sqlite_database.h"
#include "storage/sqlite_storage.h"
#include "transaction/transaction.h"
#include <format>

namespace lbug {
namespace sqlite_extension {

namespace {

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

std::string escapeCypherString(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\'') {
            out += "''";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

constexpr const char* MAP_TABLE = "__sqlite_cdc_map";
constexpr const char* CURSOR_TABLE = "__sqlite_cdc_cursor";

// All identifiers are simple; quoteCypherIdent would return them raw.
std::string mapTableDdl() {
    return "CREATE NODE TABLE IF NOT EXISTS __sqlite_cdc_map(map_key STRING, sync_name STRING, "
           "sqlite_table STRING, kind STRING, label STRING, id_column STRING, from_col STRING, "
           "to_col STRING, PRIMARY KEY(map_key))";
}

// Normalizes a user-supplied file path for use as the sync identity.
std::string normalizeSyncPath(const std::string& dbPath) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(dbPath, ec);
    if (ec) {
        return dbPath;
    }
    return abs.lexically_normal().string();
}

// Opens the SQLite file read-only for introspection. Throws
// common::Exception with a message on failure.
std::unique_ptr<SqliteConnector> openSyncFile(const std::string& normPath) {
    auto connector = std::make_unique<SqliteConnector>();
    connector->connect(normPath);
    return connector;
}

// Refuses to sync a file that is also attached into this graph: attached
// shadows share bare table names with synced native tables, so DDL would
// silently no-op and writes would bind the foreign shadow.
void guardNotAttached(main::ClientContext* context, const std::string& normPath) {
    auto* dbManager = main::DatabaseManager::Get(*context);
    if (!dbManager) {
        return;
    }
    for (auto* attached : dbManager->getAttachedDatabases()) {
        if (attached == nullptr || attached->getDBType() != SqliteStorageExtension::DB_TYPE) {
            continue;
        }
        try {
            const auto& connector = attached->constCast<AttachedSqliteDatabase>().getConnector();
            if (normalizeSyncPath(connector.getDbPath()) == normPath) {
                throw common::RuntimeException(std::format(
                    "File '{}' is attached as '{}'; DETACH it before syncing (synced native "
                    "tables cannot share names with attached shadows)",
                    normPath, attached->getDBName()));
            }
        } catch (const common::RuntimeException&) {
            throw;
        } catch (const common::Exception&) {
            // Not a SQLite attached database; ignore.
        }
    }
}

bool execOk(main::ClientContext& conn, const std::string& cypher, std::string& error) {
    auto result = conn.query(cypher);
    if (!result->isSuccess()) {
        error = result->getErrorMessage() + " [while executing: " + cypher.substr(0, 200) +
                (cypher.size() > 200 ? "..." : "") + "]";
        return false;
    }
    return true;
}

struct ExplicitMapping {
    std::string kind;
    std::string label;
    std::string idColumn;
    std::string fromCol;
    std::string toCol;
};

// Process-lifetime resolver states keyed by normalized file path. The page
// -> rowid memory must survive across CALLs: deletes are only detectable
// by diffing against previously applied state. A zero cursor (this database
// never synced) resets the entry, so a fresh database always bootstraps
// blind instead of inheriting another database's memory. Known limitation:
// a process restart loses this memory; post-restart reconciles converge
// upserts but may miss deletes that landed while down (durable state is
// future work; the cursor itself is already durable).
std::mutex resolversMtx;
std::unordered_map<std::string, cdc::OpResolver> resolvers;

cdc::OpResolver& resolverFor(const std::string& key, bool reset) {
    std::lock_guard<std::mutex> lock(resolversMtx);
    if (reset) {
        resolvers[key] = cdc::OpResolver{};
    }
    return resolvers[key];
}

} // namespace

SyncResult SyncEngine::registerNode(main::ClientContext* context, const std::string& dbPath,
    const std::string& sqliteTable, const std::string& label, const std::string& idColumn) {
    SyncResult out;
    try {
        const std::string normPath = normalizeSyncPath(dbPath);
        guardNotAttached(context, normPath);
        auto connector = openSyncFile(normPath);
        main::ClientContext inner(context->getDatabase());
        const std::string& syncName = normPath;
        // Validate the table exists and resolve a usable id column.
        auto columns = connector->getTableInfo(sqliteTable);
        if (columns.empty()) {
            out.error = std::format("SQLite table '{}' not found", sqliteTable);
            return out;
        }
        std::string resolvedId = idColumn;
        if (resolvedId.empty()) {
            std::string singlePk;
            int pkCount = 0;
            for (auto& col : columns) {
                if (col.pkOrder > 0) {
                    pkCount++;
                    singlePk = col.name;
                }
            }
            if (pkCount != 1) {
                out.error = std::format("Table '{}' needs an explicit id_column "
                                        "(no single-column primary key)",
                    sqliteTable);
                return out;
            }
            resolvedId = singlePk;
        }
        std::string resolvedLabel = label.empty() ? sqliteTable : label;
        if (!execOk(inner, mapTableDdl(), out.error)) {
            return out;
        }
        std::string key = syncName + "." + sqliteTable;
        auto merge = std::format("MERGE (m:`{}` {{`map_key`: '{}'}}) SET m.`sync_name` = '{}', "
                                 "m.`sqlite_table` = '{}', m.`kind` = 'node', m.`label` = '{}', "
                                 "m.`id_column` = '{}', m.`from_col` = '', m.`to_col` = ''",
            MAP_TABLE, escapeCypherString(key), escapeCypherString(syncName),
            escapeCypherString(sqliteTable), escapeCypherString(resolvedLabel),
            escapeCypherString(resolvedId));
        if (!execOk(inner, merge, out.error)) {
            return out;
        }
        out.ok = true;
        out.stats.status =
            std::format("Registered {} as node {} (id {})", sqliteTable, resolvedLabel, resolvedId);
        return out;
    } catch (const common::Exception& e) {
        out.error = e.what();
        return out;
    }
}

SyncResult SyncEngine::registerEdge(main::ClientContext* context, const std::string& dbPath,
    const std::string& sqliteTable, const std::string& label, const std::string& fromCol,
    const std::string& toCol) {
    SyncResult out;
    try {
        const std::string normPath = normalizeSyncPath(dbPath);
        guardNotAttached(context, normPath);
        auto connector = openSyncFile(normPath);
        main::ClientContext inner(context->getDatabase());
        const std::string& syncName = normPath;
        auto columns = connector->getTableInfo(sqliteTable);
        if (columns.empty()) {
            out.error = std::format("SQLite table '{}' not found", sqliteTable);
            return out;
        }
        auto hasCol = [&](const std::string& col) {
            for (auto& c : columns) {
                if (c.name == col) {
                    return true;
                }
            }
            return false;
        };
        std::string resolvedFrom = fromCol;
        std::string resolvedTo = toCol;
        if (resolvedFrom.empty() || resolvedTo.empty()) {
            for (auto& fk : connector->getForeignKeys(sqliteTable)) {
                auto lower = fk.fromColumn;
                for (auto& c : lower) {
                    c = static_cast<char>(std::tolower(c));
                }
                if (resolvedFrom.empty() && lower.rfind("src", 0) == 0) {
                    resolvedFrom = fk.fromColumn;
                } else if (resolvedTo.empty() &&
                           (lower.rfind("dst", 0) == 0 || lower.rfind("dest", 0) == 0)) {
                    resolvedTo = fk.fromColumn;
                }
            }
        }
        if (resolvedFrom.empty() || resolvedTo.empty() || !hasCol(resolvedFrom) ||
            !hasCol(resolvedTo)) {
            out.error = std::format("Table '{}' needs explicit from_col/to_col "
                                    "(no src/dst foreign keys found)",
                sqliteTable);
            return out;
        }
        std::string resolvedLabel = label.empty() ? sqliteTable : label;
        if (!execOk(inner, mapTableDdl(), out.error)) {
            return out;
        }
        std::string key = syncName + "." + sqliteTable;
        auto merge = std::format("MERGE (m:`{}` {{`map_key`: '{}'}}) SET m.`sync_name` = '{}', "
                                 "m.`sqlite_table` = '{}', m.`kind` = 'edge', m.`label` = '{}', "
                                 "m.`id_column` = '', m.`from_col` = '{}', m.`to_col` = '{}'",
            MAP_TABLE, escapeCypherString(key), escapeCypherString(syncName),
            escapeCypherString(sqliteTable), escapeCypherString(resolvedLabel),
            escapeCypherString(resolvedFrom), escapeCypherString(resolvedTo));
        if (!execOk(inner, merge, out.error)) {
            return out;
        }
        out.ok = true;
        out.stats.status = std::format("Registered {} as edge {} ({} -> {})", sqliteTable,
            resolvedLabel, resolvedFrom, resolvedTo);
        return out;
    } catch (const common::Exception& e) {
        out.error = e.what();
        return out;
    }
}

SyncResult SyncEngine::sync(main::ClientContext* context, const std::string& dbPath) {
    SyncResult out;
    try {
        const std::string normPath = normalizeSyncPath(dbPath);
        guardNotAttached(context, normPath);
        auto connector = openSyncFile(normPath);
        main::ClientContext inner(context->getDatabase());
        const std::string& syncName = normPath;
        auto walBytes = readFileBytes(normPath + "-wal");
        auto dbBytes = readFileBytes(normPath);
        if (dbBytes.size() < 100) {
            out.error = std::format("SQLite file '{}' is missing or truncated", normPath);
            return out;
        }
        uint32_t pageSize =
            (static_cast<uint32_t>(dbBytes[16]) << 8) | static_cast<uint32_t>(dbBytes[17]);

        // Catalog tables (idempotent).
        if (!execOk(inner, mapTableDdl(), out.error) ||
            !execOk(inner, cdc::buildCursorDdl(), out.error)) {
            return out;
        }

        // Cursor for this sync.
        cdc::WalClock clock;
        {
            auto cursor = inner.query(std::format("MATCH (c:`{}` {{`sync`: '{}'}}) RETURN "
                                                  "c.`salt1`, c.`salt2`, c.`applied`",
                CURSOR_TABLE, escapeCypherString(syncName)));
            if (!cursor->isSuccess()) {
                out.error = cursor->getErrorMessage();
                return out;
            }
            if (cursor->getNumTuples() > 0) {
                auto tuple = cursor->getNext();
                auto* s1 = tuple->getValue(0);
                auto* s2 = tuple->getValue(1);
                auto* applied = tuple->getValue(2);
                if (s1 && !s1->isNull()) {
                    clock.salt1 = static_cast<uint32_t>(s1->getValue<int64_t>());
                }
                if (s2 && !s2->isNull()) {
                    clock.salt2 = static_cast<uint32_t>(s2->getValue<int64_t>());
                }
                if (applied && !applied->isNull()) {
                    clock.appliedFrames = static_cast<size_t>(applied->getValue<int64_t>());
                }
            }
        }

        // Schemas: tables + columns + FKs from the live connection.
        std::vector<cdc::SyncTable> syncTables;
        std::vector<cdc::TableSchema> resolverSchemas;
        {
            auto master = connector->executeQuery(
                "SELECT name, rootpage, sql FROM sqlite_master WHERE type = 'table' AND name NOT "
                "LIKE 'sqlite_%' ORDER BY name");
            if (!master.success) {
                out.error = master.errorMessage;
                return out;
            }
            for (auto& row : master.rows) {
                std::string name = row.cells[0].textVal;
                uint32_t root = 0;
                if (row.cells[1].storage == SqliteStorageClass::INTEGER) {
                    root = static_cast<uint32_t>(row.cells[1].intVal);
                }
                std::string sql =
                    row.cells[2].storage == SqliteStorageClass::TEXT ? row.cells[2].textVal : "";
                auto columns = connector->getTableInfo(name);
                if (columns.empty() || root == 0) {
                    continue;
                }
                cdc::SyncTable syncTable;
                syncTable.name = name;
                syncTable.withoutRowid = sql.find("WITHOUT ROWID") != std::string::npos;
                int pkCols = 0;
                int pkIdx = -1;
                bool pkIsInt = false;
                for (size_t i = 0; i < columns.size(); i++) {
                    auto upper = columns[i].declaredType;
                    for (auto& c : upper) {
                        c = static_cast<char>(std::toupper(c));
                    }
                    syncTable.columns.push_back(
                        {columns[i].name, columns[i].declaredType, columns[i].pkOrder > 0});
                    if (columns[i].pkOrder > 0) {
                        pkCols++;
                        pkIdx = static_cast<int>(i);
                        pkIsInt = upper.find("INT") != std::string::npos;
                    }
                }
                if (pkCols == 1) {
                    syncTable.ipkIndex = pkIsInt ? pkIdx : -1;
                }
                for (auto& fk : connector->getForeignKeys(name)) {
                    syncTable.foreignKeys.push_back({fk.fromColumn, fk.referencedTable});
                }
                syncTables.push_back(syncTable);
                cdc::TableSchema schema;
                schema.name = name;
                schema.rootPage = root;
                schema.withoutRowid = syncTable.withoutRowid;
                if (pkCols == 1) {
                    schema.pkRecordIndex = pkIdx;
                    schema.pkFromRowid = pkIsInt;
                }
                resolverSchemas.push_back(schema);
            }
        }

        // Explicit mappings override inference.
        std::unordered_map<std::string, ExplicitMapping> explicit_;
        {
            auto rows = inner.query(std::format("MATCH (m:`{}` {{`sync_name`: '{}'}}) RETURN "
                                                "m.`sqlite_table`, m.`kind`, m.`label`, "
                                                "m.`id_column`, m.`from_col`, m.`to_col`",
                MAP_TABLE, escapeCypherString(syncName)));
            if (!rows->isSuccess()) {
                out.error = rows->getErrorMessage();
                return out;
            }
            while (rows->hasNext()) {
                auto tuple = rows->getNext();
                auto* table = tuple->getValue(0);
                if (!table || table->isNull()) {
                    continue;
                }
                ExplicitMapping mapping;
                auto* kind = tuple->getValue(1);
                auto* label = tuple->getValue(2);
                auto* idCol = tuple->getValue(3);
                auto* fromCol = tuple->getValue(4);
                auto* toCol = tuple->getValue(5);
                mapping.kind = (kind && !kind->isNull()) ? kind->getValue<std::string>() : "";
                mapping.label = (label && !label->isNull()) ? label->getValue<std::string>() : "";
                mapping.idColumn =
                    (idCol && !idCol->isNull()) ? idCol->getValue<std::string>() : "";
                mapping.fromCol =
                    (fromCol && !fromCol->isNull()) ? fromCol->getValue<std::string>() : "";
                mapping.toCol = (toCol && !toCol->isNull()) ? toCol->getValue<std::string>() : "";
                explicit_[table->getValue<std::string>()] = std::move(mapping);
            }
        }

        auto inferred = cdc::inferMappings(syncTables);
        // Merge: explicit rows win per table. Nodes resolve before edges
        // (edge endpoints must find their node mappings regardless of
        // unordered_map iteration order), and inferred edges rebind to
        // final node labels in case an override renamed an endpoint.
        std::vector<cdc::TableMapping> mappings;
        std::unordered_map<std::string, const cdc::SyncTable*> tableByName;
        for (auto& t : syncTables) {
            tableByName[t.name] = &t;
        }
        std::unordered_map<std::string, cdc::TableMapping> inferredByTable;
        for (auto& m : inferred.mappings) {
            inferredByTable[m.sqliteTable] = m;
        }
        std::unordered_map<std::string, cdc::TableMapping> nodePool;
        for (auto& [name, m] : inferredByTable) {
            if (m.kind == cdc::MappingKind::Node) {
                nodePool[name] = m;
            }
        }
        auto lookupNode = [&](const std::string& table) -> const cdc::TableMapping* {
            auto it = nodePool.find(table);
            return it != nodePool.end() ? &it->second : nullptr;
        };
        // Pass 1: explicit nodes. Inference already resolved rowid-sourcing;
        // explicit rows override only what they name.
        for (auto& [tableName, exp] : explicit_) {
            if (exp.kind == "edge") {
                continue;
            }
            auto it = tableByName.find(tableName);
            if (it == tableByName.end()) {
                continue; // Registered table no longer exists; ignore.
            }
            auto inferredIt = inferredByTable.find(tableName);
            cdc::TableMapping m;
            m.sqliteTable = tableName;
            m.kind = cdc::MappingKind::Node;
            m.label = exp.label.empty() ? tableName : exp.label;
            const cdc::TableMapping* inf =
                inferredIt != inferredByTable.end() ? &inferredIt->second : nullptr;
            if (inf != nullptr && inf->kind == cdc::MappingKind::Node &&
                (exp.idColumn.empty() || exp.idColumn == inf->idColumn)) {
                m.idColumn = inf->idColumn;
                m.idFromRowid = inf->idFromRowid;
                m.idRecordIndex = inf->idRecordIndex;
            } else {
                m.idColumn = exp.idColumn;
                m.idFromRowid = false;
                for (size_t i = 0; i < it->second->columns.size(); i++) {
                    if (it->second->columns[i].name == exp.idColumn) {
                        m.idRecordIndex = i;
                    }
                }
                // A single INTEGER PK is rowid-sourced even when named
                // explicitly: the record slot holds NULL.
                int pkCount = 0;
                bool pkIsInt = false;
                for (auto& col : it->second->columns) {
                    if (col.isPrimaryKey) {
                        pkCount++;
                        auto upper = col.decltype_;
                        for (auto& c : upper) {
                            c = static_cast<char>(std::toupper(c));
                        }
                        pkIsInt = upper.find("INT") != std::string::npos;
                    }
                }
                if (pkCount == 1 && pkIsInt) {
                    m.idFromRowid = true;
                }
            }
            nodePool[tableName] = m;
            mappings.push_back(m);
        }
        // Pass 2: explicit edges. Endpoints resolve against the final node
        // set, so pass order cannot strand them.
        for (auto& [tableName, exp] : explicit_) {
            if (exp.kind != "edge") {
                continue;
            }
            auto it = tableByName.find(tableName);
            if (it == tableByName.end()) {
                continue; // Registered table no longer exists; ignore.
            }
            auto inferredIt = inferredByTable.find(tableName);
            cdc::TableMapping m;
            m.sqliteTable = tableName;
            m.kind = cdc::MappingKind::Edge;
            m.label = exp.label.empty() ? tableName : exp.label;
            const cdc::TableMapping* infEdge =
                inferredIt != inferredByTable.end() ? &inferredIt->second : nullptr;
            if (infEdge != nullptr && infEdge->kind == cdc::MappingKind::Edge &&
                (exp.fromCol.empty() ||
                    exp.fromCol == it->second->columns[infEdge->srcColIndex].name) &&
                (exp.toCol.empty() ||
                    exp.toCol == it->second->columns[infEdge->dstColIndex].name)) {
                // Inference already resolved everything; keep it (relabel).
                m = *infEdge;
                m.label = exp.label.empty() ? tableName : exp.label;
                mappings.push_back(std::move(m));
                continue;
            }
            // Resolve endpoints through live FK metadata.
            for (auto& fk : it->second->foreignKeys) {
                if (fk.fromColumn == exp.fromCol) {
                    m.srcTable = fk.referencedTable;
                }
                if (fk.fromColumn == exp.toCol) {
                    m.dstTable = fk.referencedTable;
                }
            }
            if (m.srcTable.empty() || m.dstTable.empty()) {
                continue; // Endpoints unresolvable; skip.
            }
            auto srcNode = lookupNode(m.srcTable);
            auto dstNode = lookupNode(m.dstTable);
            if (srcNode == nullptr || dstNode == nullptr) {
                continue; // Endpoints must be mapped node tables.
            }
            m.srcLabel = srcNode->label;
            m.srcPkColumn = srcNode->idColumn;
            m.srcPkFromRowid = srcNode->idFromRowid;
            m.dstLabel = dstNode->label;
            m.dstPkColumn = dstNode->idColumn;
            m.dstPkFromRowid = dstNode->idFromRowid;
            auto pkDecltype = [&](const std::string& nodeTable, const std::string& pkCol) {
                auto nit = tableByName.find(nodeTable);
                if (nit == tableByName.end()) {
                    return std::string{};
                }
                for (auto& col : nit->second->columns) {
                    if (col.name == pkCol) {
                        return col.decltype_;
                    }
                }
                return std::string{};
            };
            m.srcPkDecltype = pkDecltype(m.srcTable, m.srcPkColumn);
            m.dstPkDecltype = pkDecltype(m.dstTable, m.dstPkColumn);
            auto colIndex = [&](const std::string& col) -> int {
                for (size_t i = 0; i < it->second->columns.size(); i++) {
                    if (it->second->columns[i].name == col) {
                        return static_cast<int>(i);
                    }
                }
                return -1;
            };
            int scol = colIndex(exp.fromCol);
            int dcol = colIndex(exp.toCol);
            if (scol < 0 || dcol < 0) {
                continue;
            }
            m.srcColIndex = static_cast<size_t>(scol);
            m.dstColIndex = static_cast<size_t>(dcol);
            mappings.push_back(std::move(m));
        }
        // Pass 3: inferred tables with no explicit row.
        for (auto& [name, inf] : inferredByTable) {
            if (!explicit_.contains(name)) {
                mappings.push_back(inf);
            }
        }
        // Rebind every edge to the final node set: an override may have
        // renamed an endpoint or its identity column, and inferred edges
        // predate overrides.
        auto nodePkDecltype = [&](const std::string& nodeTable, const std::string& pkCol) {
            auto nit = tableByName.find(nodeTable);
            if (nit == tableByName.end()) {
                return std::string{};
            }
            for (auto& col : nit->second->columns) {
                if (col.name == pkCol) {
                    return col.decltype_;
                }
            }
            return std::string{};
        };
        for (auto it = mappings.begin(); it != mappings.end();) {
            if (it->kind != cdc::MappingKind::Edge) {
                ++it;
                continue;
            }
            auto srcNode = lookupNode(it->srcTable);
            auto dstNode = lookupNode(it->dstTable);
            if (srcNode == nullptr || dstNode == nullptr) {
                it = mappings.erase(it);
                continue;
            }
            it->srcLabel = srcNode->label;
            it->srcPkColumn = srcNode->idColumn;
            it->srcPkFromRowid = srcNode->idFromRowid;
            it->srcPkDecltype = nodePkDecltype(it->srcTable, it->srcPkColumn);
            it->dstLabel = dstNode->label;
            it->dstPkColumn = dstNode->idColumn;
            it->dstPkFromRowid = dstNode->idFromRowid;
            it->dstPkDecltype = nodePkDecltype(it->dstTable, it->dstPkColumn);
            ++it;
        }

        // Edge tables retain full records so deletes carry endpoints.
        std::unordered_map<std::string, const cdc::SyncTable*> syncByName;
        for (auto& t : syncTables) {
            syncByName[t.name] = &t;
        }
        for (auto& schema : resolverSchemas) {
            for (auto& m : mappings) {
                if (m.kind == cdc::MappingKind::Edge && m.sqliteTable == schema.name) {
                    schema.retainRecords = true;
                }
            }
        }
        out.stats.tables = static_cast<int64_t>(mappings.size());

        // Stale shadows (DETACH leaks them): a mapped label colliding
        // with a shadow whose database is gone would make DDL silently
        // no-op and bind writes to the foreign shadow. Drop those; refuse
        // when the owning database is still attached.
        {
            auto* catalog = context->getDatabase()->getCatalog();
            auto* dbManager = main::DatabaseManager::Get(*context);
            // NOTE: DUMMY_TRANSACTION reads snapshot-0 and sees dropped
            // entries as present; the active transaction sees the truth.
            auto* txn = transaction::Transaction::Get(*context);
            if (txn == nullptr) {
                txn = &transaction::DUMMY_TRANSACTION;
            }
            for (auto& m : mappings) {
                if (!catalog->containsTable(txn, m.label)) {
                    continue;
                }
                auto* entry = catalog->getTableCatalogEntry(txn, m.label);
                auto* nodeEntry = dynamic_cast<catalog::NodeTableCatalogEntry*>(entry);
                if (nodeEntry == nullptr || nodeEntry->getForeignDatabaseName().empty()) {
                    continue; // Native table (or non-node): nothing to heal.
                }
                std::string owner = nodeEntry->getForeignDatabaseName();
                if (dbManager && dbManager->hasAttachedDatabase(owner)) {
                    out.error = std::format(
                        "Table '{}' is shadowed by attached database '{}'; DETACH it before "
                        "syncing",
                        m.label, owner);
                    return out;
                }
                if (!execOk(inner, std::format("DROP TABLE {}", cdc::quoteCypherIdent(m.label)),
                        out.error)) {
                    return out;
                }
                out.stats.status += std::format("Dropped stale shadow {}. ", m.label);
            }
        }

        // Bound targets (nodes first for DDL order).
        struct BoundEntry {
            cdc::BoundTarget target;
            const cdc::SyncTable* syncTable = nullptr;
        };
        std::vector<BoundEntry> nodes;
        std::vector<BoundEntry> edges;
        for (auto& m : mappings) {
            auto sit = syncByName.find(m.sqliteTable);
            if (sit == syncByName.end()) {
                continue;
            }
            BoundEntry entry;
            entry.target = cdc::bindTarget(m, sit->second->columns);
            entry.syncTable = sit->second;
            if (m.kind == cdc::MappingKind::Node) {
                nodes.push_back(std::move(entry));
            } else {
                edges.push_back(std::move(entry));
            }
        }
        for (auto& entry : nodes) {
            if (!execOk(inner, cdc::buildNodeDdl(entry.target), out.error)) {
                return out;
            }
        }
        for (auto& entry : edges) {
            if (!execOk(inner, cdc::buildRelDdl(entry.target), out.error)) {
                return out;
            }
        }

        // WAL overlay fetcher: newest frame wins, then the DB file. Owned
        // buffers keep the whole sync zero-copy. NOTE: page 1 is never
        // decoded (no user table roots at page 1), so its 100-byte file
        // header needs no special-casing here; page-1 frames only drive
        // the schema-change decision below.
        struct Overlay {
            const std::vector<cdc::WalFrame>* frames = nullptr;
            size_t limit = 0;
            const std::vector<uint8_t>* db = nullptr;
            uint32_t pageSize = 4096;
            const uint8_t* fetch(uint32_t pageNo) const {
                for (size_t i = limit; i-- > 0;) {
                    if ((*frames)[i].pageNo == pageNo) {
                        return (*frames)[i].image.data();
                    }
                }
                // DB file packs pages at (N-1)*pageSize; page 1 embeds
                // the 100-byte header in its first bytes (never decoded
                // here: no user table roots at page 1).
                size_t off = (static_cast<size_t>(pageNo) - 1) * pageSize;
                if (off + pageSize <= db->size()) {
                    return db->data() + off;
                }
                return nullptr;
            }
        };

        // Resolver memory persists across CALLs (see resolverFor note).
        // A zero cursor means this database never synced: start blind.
        bool firstSync = clock.salt1 == 0 && clock.salt2 == 0 && clock.appliedFrames == 0;
        cdc::OpResolver& resolver = resolverFor(normPath, firstSync);
        auto applyBatch = [&](const cdc::OpBatch& batch) -> bool {
            // Upserts: nodes, then edges. Deletes: edges, then nodes.
            for (auto& entry : nodes) {
                std::vector<cdc::RowOp> tableOps;
                std::vector<cdc::RowDelete> tableDels;
                for (auto& op : batch.upserts) {
                    if (op.table == entry.target.mapping->sqliteTable) {
                        tableOps.push_back(op);
                    }
                }
                for (auto& del : batch.deletes) {
                    if (del.table == entry.target.mapping->sqliteTable) {
                        tableDels.push_back(del);
                    }
                }
                for (auto& stmt : cdc::buildNodeUpserts(entry.target, tableOps)) {
                    if (!execOk(inner, stmt, out.error)) {
                        return false;
                    }
                }
                out.stats.upserted += static_cast<int64_t>(tableOps.size());
                for (auto& stmt : cdc::buildNodeDeletes(entry.target, tableDels)) {
                    if (!execOk(inner, stmt, out.error)) {
                        return false;
                    }
                }
                out.stats.deleted += static_cast<int64_t>(tableDels.size());
            }
            for (auto& entry : edges) {
                std::vector<cdc::RowOp> tableOps;
                for (auto& op : batch.upserts) {
                    if (op.table == entry.target.mapping->sqliteTable) {
                        tableOps.push_back(op);
                    }
                }
                std::vector<cdc::RowOp> tableDelOps;
                for (auto& del : batch.deletes) {
                    if (del.table == entry.target.mapping->sqliteTable && del.hasRecord) {
                        cdc::RowOp op;
                        op.table = del.table;
                        op.rowid = del.rowid;
                        op.values = del.record;
                        tableDelOps.push_back(std::move(op));
                    }
                }
                for (auto& stmt : cdc::buildEdgeUpserts(entry.target, tableOps)) {
                    if (!execOk(inner, stmt, out.error)) {
                        return false;
                    }
                }
                out.stats.upserted += static_cast<int64_t>(tableOps.size());
                for (auto& stmt : cdc::buildEdgeDeletes(entry.target, tableDelOps)) {
                    if (!execOk(inner, stmt, out.error)) {
                        return false;
                    }
                }
                out.stats.deleted += static_cast<int64_t>(tableDelOps.size());
            }
            return true;
        };

        Overlay overlay{nullptr, 0, &dbBytes, pageSize};
        cdc::PageFetcher fetcher = [&](uint32_t pageNo) { return overlay.fetch(pageNo); };
        bool haveWal = !walBytes.empty();
        cdc::WalParseResult parsed;
        if (haveWal) {
            parsed = cdc::parseWal(walBytes.data(), walBytes.size());
            if (!parsed.ok) {
                out.error = parsed.error;
                return out;
            }
            pageSize = parsed.log.header.pageSize;
            overlay.pageSize = pageSize;
            overlay.frames = &parsed.log.frames;
        }
        if (overlay.frames == nullptr) {
            static const std::vector<cdc::WalFrame> empty;
            overlay.frames = &empty;
        }
        auto tableSchemas = [&]() {
            std::vector<cdc::TableSchema> schemas;
            // Only mapped tables need tracking; unmapped tables stay
            // foreign-scanned and cost nothing.
            for (auto& m : mappings) {
                for (auto& s : resolverSchemas) {
                    if (s.name == m.sqliteTable) {
                        schemas.push_back(s);
                    }
                }
                // Edge endpoints resolve through the edge record itself;
                // endpoint node tables need no tracking unless mapped.
            }
            return schemas;
        };

        if (!haveWal) {
            // No WAL (fully checkpointed or quiet DB): bootstrap from the
            // DB file when nothing was ever synced.
            if (clock.appliedFrames == 0 && clock.salt1 == 0) {
                Overlay dbOnly{nullptr, 0, &dbBytes, pageSize};
                cdc::PageFetcher dbFetcher = [&](uint32_t p) { return dbOnly.fetch(p); };
                auto r = resolver.bootstrap(tableSchemas(), dbFetcher, pageSize);
                if (!r.ok) {
                    out.error = r.error;
                    return out;
                }
                if (!applyBatch(r.batch)) {
                    return out;
                }
                out.stats.status = "Bootstrapped from database file (no WAL)";
            } else {
                out.stats.status = "No WAL file; nothing to apply";
            }
        } else if (cdc::walRestarted(clock, parsed.log.header) ||
                   clock.appliedFrames > parsed.log.frames.size()) {
            overlay.limit = parsed.log.frames.size();
            auto r = resolver.reconcile(tableSchemas(), fetcher, pageSize);
            if (!r.ok) {
                out.error = r.error;
                return out;
            }
            if (!applyBatch(r.batch)) {
                return out;
            }
            clock.salt1 = parsed.log.header.salt1;
            clock.salt2 = parsed.log.header.salt2;
            clock.appliedFrames = parsed.log.frames.size();
            out.stats.status = "Reconciled after WAL restart";
        } else {
            // Page-1 dirt in the new window means DDL: reconcile instead.
            bool page1Dirty = false;
            for (size_t i = clock.appliedFrames; i < parsed.log.frames.size(); i++) {
                if (parsed.log.frames[i].pageNo == 1) {
                    page1Dirty = true;
                    break;
                }
            }
            // Schema-set comparison against live schemas guards the rest.
            if (page1Dirty) {
                overlay.limit = parsed.log.frames.size();
                auto r = resolver.reconcile(tableSchemas(), fetcher, pageSize);
                if (!r.ok) {
                    out.error = r.error;
                    return out;
                }
                if (!applyBatch(r.batch)) {
                    return out;
                }
                clock.appliedFrames = parsed.log.frames.size();
                out.stats.status = "Reconciled around schema change";
            } else {
                for (auto& commit : parsed.log.commits) {
                    if (commit.lastFrame < clock.appliedFrames) {
                        continue;
                    }
                    std::vector<uint32_t> dirty;
                    for (size_t i = clock.appliedFrames; i <= commit.lastFrame; i++) {
                        dirty.push_back(parsed.log.frames[i].pageNo);
                    }
                    overlay.limit = commit.lastFrame + 1;
                    auto r = resolver.applyFrames(tableSchemas(), dirty, fetcher, pageSize);
                    if (!r.ok) {
                        out.error = r.error;
                        return out;
                    }
                    if (!applyBatch(r.batch)) {
                        return out;
                    }
                    clock.appliedFrames = commit.lastFrame + 1;
                }
                out.stats.status = "Applied incrementally";
            }
            clock.salt1 = parsed.log.header.salt1;
            clock.salt2 = parsed.log.header.salt2;
        }
        out.stats.appliedFrames = static_cast<int64_t>(clock.appliedFrames);
        // Cursor last: crash-before-cursor replays idempotently.
        auto cursor =
            cdc::buildCursorUpsert(syncName, clock.salt1, clock.salt2, clock.appliedFrames);
        if (!execOk(inner, cursor, out.error)) {
            return out;
        }
        out.ok = true;
        return out;
    } catch (const common::Exception& e) {
        out.error = e.what();
        return out;
    }
}

} // namespace sqlite_extension
} // namespace lbug
