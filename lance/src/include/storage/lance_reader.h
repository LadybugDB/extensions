#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/data_chunk/data_chunk.h"
#include "connector/duckdb_connector.h"
#include "connector/duckdb_result_converter.h"
#include "connector/duckdb_type_converter.h"
#include "connector/lance_connector.h"

namespace lbug {
namespace lance_extension {

// One dataset column: its name, its DuckDB type spelling, and the converted Ladybug type.
struct LanceDatasetColumn {
    std::string name;
    std::string duckdbType;
    common::LogicalType ladybugType;
};

// Stateful range reader over one Lance dataset. Issues one DuckDB/SQL query per
// range (`SELECT <casts> FROM '<path>' LIMIT/OFFSET`, which lance-duckdb pushes
// into the Lance scan) and decodes result chunks into Ladybug DataChunks with
// DuckDBResultConverter. One reader serves one scan state; ranges are opened
// sequentially (a new range replaces the previous query), so no locking is
// needed. Mirrors the ParquetReader surface used by the icebug-disk tables.
class LanceReader {
public:
    // One projected output column: the dataset column to read, the DuckDB CAST
    // target aligning it with the catalog type, and the Ladybug output type.
    struct ProjectedColumn {
        std::string datasetName;
        std::string castSpelling;
        common::LogicalType ladybugType;
    };

    LanceReader(std::shared_ptr<LanceConnector> connector, std::string datasetPath,
        std::vector<ProjectedColumn> projection, main::ClientContext* context);

    const std::vector<ProjectedColumn>& getProjection() const { return projection; }
    std::vector<common::LogicalType> getProjectionTypes() const {
        std::vector<common::LogicalType> types;
        types.reserve(projection.size());
        for (auto& col : projection) {
            types.push_back(col.ladybugType.copy());
        }
        return types;
    }

    // Issues a new range query, replacing any active one. limit == UINT64_MAX scans to end.
    void openRange(uint64_t offset, uint64_t limit);
    // Fetches the next chunk of the active range into a fresh `out` (whose vectors must
    // match the projection in order). Returns rows read; 0 means exhausted.
    uint64_t next(common::DataChunk& out);

    // Metadata helpers (each opens a short-lived connection).
    static std::vector<LanceDatasetColumn> fetchSchema(LanceConnector& connector,
        main::ClientContext* context, const std::string& datasetPath);
    static uint64_t countRows(LanceConnector& connector, main::ClientContext* context,
        const std::string& datasetPath);

    // DuckDB CAST target spelling for a catalog type, so decoded values have the physical
    // layout DuckDBResultConverter expects. Throws for STRUCT/MAP/UNION and other types
    // with no lossless spelling (see docs/lance.md, supported types).
    static std::string toDuckDBCastSpelling(const common::LogicalType& type);

private:
    std::string buildRangeQuery(uint64_t offset, uint64_t limit) const;

    std::shared_ptr<LanceConnector> connector;
    std::string datasetPath;
    std::vector<ProjectedColumn> projection;
    main::ClientContext* context;
    std::unique_ptr<duckdb::Connection> connection;
    std::unique_ptr<duckdb::QueryResult> activeResult;
    duckdb_extension::DuckDBResultConverter converter;
};

} // namespace lance_extension
} // namespace lbug
