#include "cdc/sync_mapping.h"

#include <cctype>
#include <unordered_map>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

namespace {

std::string toLower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool startsWithLower(const std::string& name, const char* prefix) {
    auto lower = toLower(name);
    std::string pre(prefix);
    return lower.size() >= pre.size() && lower.compare(0, pre.size(), pre) == 0;
}

// Endpoint-column convention shared with the catalog and the join
// push-down optimizer: src*/dst*/dest* prefixes (e.g. src_id, dst).
bool isSrcColumn(const std::string& lowerName) {
    return lowerName.rfind("src", 0) == 0;
}

bool isDstColumn(const std::string& lowerName) {
    return lowerName.rfind("dst", 0) == 0 || lowerName.rfind("dest", 0) == 0;
}

const SyncTable* findTable(const std::vector<SyncTable>& tables, const std::string& name) {
    for (auto& t : tables) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

bool mapNode(const SyncTable& table, TableMapping& mapping, std::string& skipReason) {
    if (table.withoutRowid) {
        skipReason = "WITHOUT ROWID tables are not supported";
        return false;
    }
    mapping.kind = MappingKind::Node;
    mapping.sqliteTable = table.name;
    mapping.label = table.name;
    if (table.ipkIndex >= 0) {
        mapping.idFromRowid = true;
        mapping.idRecordIndex = static_cast<size_t>(table.ipkIndex);
        mapping.idColumn = table.columns[table.ipkIndex].name;
        return true;
    }
    int pkCount = 0;
    size_t pkIndex = 0;
    for (size_t i = 0; i < table.columns.size(); i++) {
        if (table.columns[i].isPrimaryKey) {
            pkCount++;
            pkIndex = i;
        }
    }
    if (pkCount != 1) {
        skipReason = "node table requires a single-column primary key";
        return false;
    }
    mapping.idFromRowid = false;
    mapping.idRecordIndex = pkIndex;
    mapping.idColumn = table.columns[pkIndex].name;
    return true;
}

} // namespace

MappingResult inferMappings(const std::vector<SyncTable>& tables) {
    MappingResult out;
    // Pass 1: nodes. Pass 2: edges (endpoints must resolve to nodes).
    std::unordered_map<std::string, const TableMapping*> nodes;
    std::vector<TableMapping> nodeMappings;
    for (auto& table : tables) {
        if (!startsWithLower(table.name, "node_")) {
            continue;
        }
        TableMapping mapping;
        std::string reason;
        if (mapNode(table, mapping, reason)) {
            nodeMappings.push_back(std::move(mapping));
        } else {
            out.skipped.push_back({table.name, reason});
        }
    }
    for (auto& m : nodeMappings) {
        nodes[m.sqliteTable] = &m;
    }
    out.mappings.insert(out.mappings.end(), nodeMappings.begin(), nodeMappings.end());

    for (auto& table : tables) {
        bool isRel = startsWithLower(table.name, "rel_");
        bool isCsr = startsWithLower(table.name, "csr_rel_");
        if (!isRel && !isCsr) {
            if (!startsWithLower(table.name, "node_")) {
                out.skipped.push_back({table.name, "no node_/rel_ prefix; kept as foreign scan"});
            }
            continue;
        }
        // Endpoint detection from FK metadata + naming convention. Strict:
        // without FK-backed endpoints resolving to mapped nodes the edge
        // cannot be oriented safely, so it is skipped, not guessed.
        const SyncForeignKey* srcFk = nullptr;
        const SyncForeignKey* dstFk = nullptr;
        for (auto& fk : table.foreignKeys) {
            auto lower = toLower(fk.fromColumn);
            if (isSrcColumn(lower) && srcFk == nullptr) {
                srcFk = &fk;
            } else if (isDstColumn(lower) && dstFk == nullptr) {
                dstFk = &fk;
            }
        }
        if (srcFk == nullptr || dstFk == nullptr) {
            out.skipped.push_back(
                {table.name, "edge table requires src/dst foreign keys to mapped node tables"});
            continue;
        }
        auto srcNode = nodes.find(srcFk->referencedTable);
        auto dstNode = nodes.find(dstFk->referencedTable);
        if (srcNode == nodes.end() || dstNode == nodes.end()) {
            out.skipped.push_back({table.name, "edge endpoints must reference mapped node tables"});
            continue;
        }
        auto findCol = [&](const std::string& col) -> int {
            for (size_t i = 0; i < table.columns.size(); i++) {
                if (table.columns[i].name == col) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        int srcIdx = findCol(srcFk->fromColumn);
        int dstIdx = findCol(dstFk->fromColumn);
        if (srcIdx < 0 || dstIdx < 0) {
            out.skipped.push_back({table.name, "endpoint column missing from table"});
            continue;
        }
        TableMapping mapping;
        mapping.kind = MappingKind::Edge;
        mapping.sqliteTable = table.name;
        mapping.label = table.name;
        mapping.csrContract = isCsr;
        mapping.srcTable = srcFk->referencedTable;
        mapping.dstTable = dstFk->referencedTable;
        mapping.srcLabel = srcNode->second->label;
        mapping.dstLabel = dstNode->second->label;
        mapping.srcColIndex = static_cast<size_t>(srcIdx);
        mapping.dstColIndex = static_cast<size_t>(dstIdx);
        mapping.srcPkColumn = srcNode->second->idColumn;
        mapping.dstPkColumn = dstNode->second->idColumn;
        mapping.srcPkFromRowid = srcNode->second->idFromRowid;
        mapping.dstPkFromRowid = dstNode->second->idFromRowid;
        auto pkDecltype = [&](const SyncTable* nodeTable, const TableMapping* node) {
            for (auto& col : nodeTable->columns) {
                if (col.name == node->idColumn) {
                    return col.decltype_;
                }
            }
            return std::string{};
        };
        auto srcTable = findTable(tables, srcFk->referencedTable);
        auto dstTable = findTable(tables, dstFk->referencedTable);
        mapping.srcPkDecltype = pkDecltype(srcTable, srcNode->second);
        mapping.dstPkDecltype = pkDecltype(dstTable, dstNode->second);
        out.mappings.push_back(std::move(mapping));
    }
    return out;
}

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
