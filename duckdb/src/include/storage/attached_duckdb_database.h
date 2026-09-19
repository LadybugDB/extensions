#pragma once
#include <string>
#include <vector>

#include "connector/duckdb_connector.h"
#include "main/attached_database.h"
#include <format>

namespace lbug {
namespace duckdb_extension {

// Splits a possibly qualified SQL table reference into its parts, honouring
// double-quoted identifiers: `"catalog".schema.table` -> [catalog, schema,
// table]. Surrounding quotes are stripped from each part.
static std::vector<std::string> splitQualifiedTableName(const std::string& tableName) {
    std::vector<std::string> parts;
    std::string current;
    bool inQuotes = false;
    for (auto c : tableName) {
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (c == '.' && !inQuotes) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    parts.push_back(current);
    return parts;
}

static std::string escapeSingleQuotes(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (auto c : value) {
        if (c == '\'') {
            result += "''";
        } else {
            result += c;
        }
    }
    return result;
}

class AttachedDuckDBDatabase : public main::AttachedDatabase {
public:
    AttachedDuckDBDatabase(std::string dbName, std::string dbType,
        std::unique_ptr<extension::CatalogExtension> catalog,
        std::unique_ptr<DuckDBConnector> connector)
        : main::AttachedDatabase{std::move(dbName), std::move(dbType), std::move(catalog)},
          connector{std::move(connector)} {}

    const DuckDBConnector& getConnector() const { return *connector; }

    std::unique_ptr<duckdb::MaterializedQueryResult> executeQuery(
        const std::string& query) const override {
        return connector->executeQuery(query);
    }

    std::vector<std::string> getTableColumnNames(const std::string& tableName) const override {
        // Accepts both bare table names and qualified `catalog[.schema].table`
        // references (as produced by the SQL push-down optimizer). Scoping the
        // information_schema lookup by catalog/schema keeps same-named tables
        // in different schemas or catalogs of one attached database apart.
        auto parts = splitQualifiedTableName(tableName);
        auto unqualified = parts.back();
        std::string filters;
        if (parts.size() == 3) {
            filters = std::format(" AND table_catalog = '{}' AND table_schema = '{}'",
                escapeSingleQuotes(parts[0]), escapeSingleQuotes(parts[1]));
        } else if (parts.size() == 2) {
            filters = std::format(" AND table_schema = '{}'", escapeSingleQuotes(parts[0]));
        }
        std::string query = std::format("SELECT column_name FROM information_schema.columns WHERE "
                                        "table_name = '{}'{} ORDER BY ordinal_position",
            escapeSingleQuotes(unqualified), filters);

        auto result = connector->executeQuery(query);
        if ((!result || result->RowCount() == 0) && !filters.empty()) {
            // Fall back to the unqualified lookup: some engines report
            // catalog/schema names differently than the attached alias.
            query = std::format("SELECT column_name FROM information_schema.columns WHERE "
                                "table_name = '{}' ORDER BY ordinal_position",
                escapeSingleQuotes(unqualified));
            result = connector->executeQuery(query);
        }
        if (!result || result->RowCount() == 0) {
            return {};
        }

        std::vector<std::string> columnNames;
        for (auto i = 0u; i < result->RowCount(); i++) {
            columnNames.push_back(result->GetValue(0, i).GetValue<std::string>());
        }
        return columnNames;
    }

protected:
    std::unique_ptr<DuckDBConnector> connector;
};

} // namespace duckdb_extension
} // namespace lbug
