#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cdc/op_resolver.h"
#include "cdc/sync_mapping.h"

namespace lbug {
namespace sqlite_extension {
namespace cdc {

// Cypher statement builder for the sync applier: turns resolved row ops
// into executable statements (DDL + UNWIND-batched DML + cursor write).
// Pure string building, no engine dependency; execution is phase 3b.

// SQLite decltype affinity -> Cypher column type.
std::string cypherTypeForDecl(const std::string& decltype_);

// Backtick-quoted identifier.
std::string quoteCypherIdent(const std::string& name);

// Rendered literal or type error (strict: storage must match the declared
// column type, mirroring the scan's enforcement).
struct RenderResult {
    bool ok = false;
    std::string literal;
    std::string error;
};

RenderResult renderCypherValue(const SqliteValue& value, const std::string& cypherType);

// A mapping bound to record layout: which record positions feed the id,
// the SET props (nodes) or endpoint + prop columns (edges).
struct BoundTarget {
    const TableMapping* mapping = nullptr;
    struct Prop {
        std::string name;
        std::string type;
        size_t recordIndex = 0;
    };
    // All record columns in schema order (for DDL).
    std::vector<Prop> columns;
    // SET/map properties in record order, excluding the id column (nodes)
    // and the endpoint columns (edges).
    std::vector<Prop> props;
    // Cypher type of the node identity (always INT64 when rowid-sourced).
    std::string idType = "INT64";
    // Edge endpoint identity types, for typed MATCH.
    std::string srcIdType = "INT64";
    std::string dstIdType = "INT64";
};

// recordColumns are in schema (record) order.
BoundTarget bindTarget(const TableMapping& mapping, const std::vector<SyncColumn>& recordColumns);

std::string buildNodeDdl(const BoundTarget& target);
std::string buildRelDdl(const BoundTarget& target);
std::string buildCursorDdl();

// Chunked UNWIND statements (batchSize rows each; must be >= 1).
std::vector<std::string> buildNodeUpserts(const BoundTarget& target, const std::vector<RowOp>& ops,
    size_t batchSize = 500);
std::vector<std::string> buildNodeDeletes(const BoundTarget& target,
    const std::vector<RowDelete>& ops, size_t batchSize = 2000);
std::vector<std::string> buildEdgeUpserts(const BoundTarget& target, const std::vector<RowOp>& ops,
    size_t batchSize = 500);
std::vector<std::string> buildEdgeDeletes(const BoundTarget& target, const std::vector<RowOp>& ops,
    size_t batchSize = 2000);
std::string buildCursorUpsert(const std::string& syncName, uint32_t salt1, uint32_t salt2,
    size_t appliedFrames);

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
