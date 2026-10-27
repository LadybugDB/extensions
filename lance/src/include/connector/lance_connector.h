#pragma once

#include "connector/duckdb_connector.h"

namespace lbug {
namespace lance_extension {

// DuckDB-backed connector for Lance datasets. Creates an in-memory DuckDB
// instance with the lance (and httpfs, for object-store URIs) extensions
// loaded, so Lance datasets can be queried with the same engine that powers
// lance-duckdb: `SELECT ... FROM '<path>.lance'` (replacement scan),
// `COPY ... TO ... (FORMAT lance)`, and friends.
class LanceConnector : public duckdb_extension::DuckDBConnector {
public:
    void connect(const std::string& dbPath, const std::string& catalogName,
        const std::string& schemaName, main::ClientContext* context) override;

    // Opens an additional connection on the shared instance for one scan state.
    // DuckDB connections must not run concurrent queries, so every scan state
    // owns its connection and issues its range queries without locking.
    std::unique_ptr<duckdb::Connection> newConnection(main::ClientContext* context);

    std::unique_ptr<duckdb::QueryResult> executeOn(duckdb::Connection& connection,
        const std::string& query) const;

    // SQL builders. Identifiers are double-quoted, string literals single-quoted.
    static std::string escapeLiteral(const std::string& value);
    static std::string quoteIdent(const std::string& ident);
    static std::string scanAllQuery(const std::string& datasetPath);
    static std::string countRowsQuery(const std::string& datasetPath);
    static std::string schemaQuery(const std::string& datasetPath);
};

} // namespace lance_extension
} // namespace lbug
