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
//
// Callers treat a 2-part reference as `schema.table` and a 3-part reference as
// `catalog.schema.table`, matching the convention used by the SQL push-down
// optimizer (foreign table scans are described as `"catalog".schema.table`).
// References with more than 3 parts have no defined meaning and are looked up
// by their unqualified table name.
inline std::vector<std::string> splitQualifiedTableName(const std::string& tableName) {
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

inline std::string escapeSingleQuotes(const std::string& value) {
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
        // Candidate filters from most to least specific. Some engines report
        // the catalog name differently than the attached alias, so a fully
        // qualified lookup may miss while a schema-scoped one hits. The
        // unqualified lookup is the last resort: it keeps attached databases
        // from older extension builds (which only match bare names) working,
        // but it can match a same-named table in another schema when the
        // qualification is wrong, so it must stay last.
        std::vector<std::string> filterCandidates;
        if (parts.size() == 3) {
            filterCandidates.push_back(std::format(" AND table_catalog = '{}' AND table_schema "
                                                   "= '{}'",
                escapeSingleQuotes(parts[0]), escapeSingleQuotes(parts[1])));
            filterCandidates.push_back(
                std::format(" AND table_schema = '{}'", escapeSingleQuotes(parts[1])));
        } else if (parts.size() == 2) {
            filterCandidates.push_back(
                std::format(" AND table_schema = '{}'", escapeSingleQuotes(parts[0])));
        }
        filterCandidates.emplace_back("");
        for (auto& filters : filterCandidates) {
            std::string query =
                std::format("SELECT column_name FROM information_schema.columns WHERE table_name "
                            "= '{}'{} ORDER BY ordinal_position",
                    escapeSingleQuotes(unqualified), filters);
            auto result = connector->executeQuery(query);
            if (result && result->RowCount() != 0) {
                std::vector<std::string> columnNames;
                for (auto i = 0u; i < result->RowCount(); i++) {
                    columnNames.push_back(result->GetValue(0, i).GetValue<std::string>());
                }
                return columnNames;
            }
        }
        return {};
    }

protected:
    std::unique_ptr<DuckDBConnector> connector;
};

} // namespace duckdb_extension
} // namespace lbug
