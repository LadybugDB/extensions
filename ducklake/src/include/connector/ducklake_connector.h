#pragma once

#include "connector/duckdb_connector.h"

namespace lbug {
namespace ducklake_extension {

class DuckLakeConnector : public duckdb_extension::DuckDBConnector {
public:
    void connect(const std::string& dbPath, const std::string& catalogName,
        const std::string& schemaName, main::ClientContext* context) override;
};

} // namespace ducklake_extension
} // namespace lbug
