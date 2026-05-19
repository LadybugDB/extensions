#pragma once

#include "connector/duckdb_connector.h"

namespace lbug {
namespace postgres_extension {

class PostgresConnector : public duckdb_extension::DuckDBConnector {
public:
    void connect(const std::string& dbPath, const std::string& catalogName,
        const std::string& schemaName, main::ClientContext* context) override;

    std::shared_ptr<duckdb_extension::DuckDBTableScanInfo> getTableScanInfo(std::string query,
        std::vector<common::LogicalType> columnTypes,
        std::vector<std::string> columnNames) const override;
};

} // namespace postgres_extension
} // namespace lbug
