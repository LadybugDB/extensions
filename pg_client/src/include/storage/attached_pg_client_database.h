#pragma once

#include <cctype>
#include <format>
#include "connector/pg_client_connector.h"
#include "main/attached_database.h"

namespace lbug {
namespace pg_client_extension {

class AttachedPgClientDatabase final : public main::AttachedDatabase {
public:
    AttachedPgClientDatabase(std::string dbName, std::string dbType,
        std::unique_ptr<extension::CatalogExtension> catalog,
        std::unique_ptr<PgClientConnector> connector,
        std::string schemaName = "public")
        : main::AttachedDatabase{std::move(dbName), std::move(dbType), std::move(catalog)},
          connector_{std::move(connector)}, schemaName_{std::move(schemaName)} {}

    const PgClientConnector& getConnector() const { return *connector_; }

    std::vector<std::string> getTableColumnNames(const std::string& tableName) const override {
        // The foreign join push-down optimizer expects the first two columns
        // to be the src and dst FK columns respectively. For FK-based rel
        // tables (rel_*), the FK columns may not be the first two columns
        // in ordinal_position order (the PK often comes first).
        //
        // To satisfy this contract, query FK constraints and put any detected
        // src/dst FK columns first, followed by remaining columns in ordinal
        // order. For non-rel tables (node_*), ordinal order is fine since
        // only the first column (PK) is used as the node ID column.

        // First, try to detect FK columns for this table
        auto fkQuery = std::format(
            "SELECT kcu.column_name, ccu.table_name "
            "FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu "
            "  ON tc.constraint_name = kcu.constraint_name "
            "  AND tc.table_schema = kcu.table_schema "
            "JOIN information_schema.constraint_column_usage ccu "
            "  ON ccu.constraint_name = tc.constraint_name "
            "  AND ccu.table_schema = tc.table_schema "
            "WHERE tc.constraint_type = 'FOREIGN KEY' "
            "  AND tc.table_schema = '{}' "
            "  AND tc.table_name = '{}'",
            schemaName_, tableName);

        auto fkResult = connector_->executeQuery(fkQuery);
        std::string srcFkCol, dstFkCol;
        for (auto& row : fkResult.rows) {
            auto colName = row.cells[0].value;
            auto lowerCol = colName;
            for (auto& c : lowerCol) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lowerCol.rfind("src", 0) == 0 || lowerCol.rfind("from", 0) == 0) {
                srcFkCol = colName;
            } else if (lowerCol.rfind("dst", 0) == 0 || lowerCol.rfind("dest", 0) == 0 ||
                       lowerCol.rfind("to", 0) == 0) {
                dstFkCol = colName;
            }
        }

        // Query all columns ordered by ordinal_position
        auto query = std::format(
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_schema = '{}' AND table_name = '{}' "
            "ORDER BY ordinal_position",
            schemaName_, tableName);

        auto result = connector_->executeQuery(query);
        if (!result.success) {
            return {};
        }

        std::vector<std::string> columnNames;
        // Add FK columns first if detected
        if (!srcFkCol.empty()) {
            columnNames.push_back(srcFkCol);
        }
        if (!dstFkCol.empty()) {
            columnNames.push_back(dstFkCol);
        }
        // Add remaining columns (excluding already-added FK columns)
        for (auto& row : result.rows) {
            auto name = row.cells[0].value;
            if (name != srcFkCol && name != dstFkCol) {
                columnNames.push_back(name);
            }
        }
        return columnNames;
    }

private:
    std::unique_ptr<PgClientConnector> connector_;
    std::string schemaName_;
};

} // namespace pg_client_extension
} // namespace lbug
