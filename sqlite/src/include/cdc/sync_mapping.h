#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

// Sync mapping: which SQLite tables materialize into which Ladybug tables,
// mirroring pg_ladybug's register_node()/register_edge() semantics
// (label <-> table, id column, endpoint columns, props) with upsert
// behavior. The node_*/rel_*/csr_rel_* naming convention auto-populates
// this catalog; explicit register_* calls (phase 3b) override it.

// One SQLite table as seen by the sync planner.
struct SyncColumn {
    std::string name;
    std::string decltype_;
    bool isPrimaryKey = false;
};

struct SyncForeignKey {
    std::string fromColumn;
    std::string referencedTable;
};

struct SyncTable {
    std::string name;
    std::vector<SyncColumn> columns;
    // Position of a single INTEGER PRIMARY KEY column, else -1.
    int ipkIndex = -1;
    std::vector<SyncForeignKey> foreignKeys;
    bool withoutRowid = false;
};

enum class MappingKind : uint8_t { Node, Edge };

struct TableMapping {
    MappingKind kind = MappingKind::Node;
    std::string sqliteTable;
    // Ladybug table name. Convention keeps the SQLite name verbatim
    // (node_person -> node_person), matching pg_client catalog behavior.
    std::string label;
    // Node identity. Rowid-sourced for INTEGER PRIMARY KEY tables (the
    // record slot holds NULL; the cell rowid is authoritative).
    bool idFromRowid = false;
    size_t idRecordIndex = 0;
    std::string idColumn;
    // Edge-only: endpoint columns in the edge record plus the endpoint
    // nodes' identity columns for MATCH.
    std::string srcTable;
    std::string dstTable;
    std::string srcLabel;
    std::string dstLabel;
    size_t srcColIndex = 0;
    size_t dstColIndex = 0;
    std::string srcPkColumn;
    std::string dstPkColumn;
    bool srcPkFromRowid = false;
    bool dstPkFromRowid = false;
    // Raw decltypes of the endpoint PK columns (mapped to Cypher types
    // by the statement builder for typed MATCH).
    std::string srcPkDecltype;
    std::string dstPkDecltype;
    // True for csr_rel_* tables (relaxed-offset contract for later
    // bulk-load; the applier treats them as edges initially).
    bool csrContract = false;
};

struct SkippedTable {
    std::string name;
    std::string reason;
};

struct MappingResult {
    std::vector<TableMapping> mappings;
    std::vector<SkippedTable> skipped;
};

// Infers mappings from the naming convention:
//   node_*          -> node table (single-column PK required)
//   rel_*/csr_rel_* -> edge table (FK-backed src/dst endpoints required,
//                      both ends resolving to mapped node tables)
//   anything else   -> skipped (keeps today's foreign-scan behavior)
MappingResult inferMappings(const std::vector<SyncTable>& tables);

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
