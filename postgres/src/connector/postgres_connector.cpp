#include "connector/postgres_connector.h"

#include <format>

namespace lbug {
namespace postgres_extension {

void PostgresConnector::connect(const std::string& dbPath, const std::string& catalogName,
    const std::string& schemaName, main::ClientContext* /*context*/) {
    // Creates an in-memory duckdb instance, then install httpfs and attach postgres.
    instance = std::make_unique<duckdb::DuckDB>(nullptr);
    connection = std::make_unique<duckdb::Connection>(*instance);
    executeQuery("install postgres;");
    executeQuery("load postgres;");
    executeQuery(std::format("attach '{}' as {} (TYPE postgres, SCHEMA {}, read_only);", dbPath,
        catalogName, schemaName));
}

std::shared_ptr<duckdb_extension::DuckDBTableScanInfo> PostgresConnector::getTableScanInfo(
    std::string query, std::vector<common::LogicalType> columnTypes,
    std::vector<std::string> columnNames) const {
    auto internalIDColumnName = columnNames.empty() ? "id" : columnNames[0];
    return std::make_shared<duckdb_extension::DuckDBTableScanInfo>(std::move(query),
        std::move(columnTypes), std::move(columnNames), *this, std::move(internalIDColumnName));
}

} // namespace postgres_extension
} // namespace lbug
