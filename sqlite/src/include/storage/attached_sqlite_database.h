#pragma once

#include <cctype>
#include <string>
#include <vector>

#include "common/exception/exception.h"
#include "connector/sqlite_connector.h"
#include "main/attached_database.h"

namespace lbug {
namespace sqlite_extension {

class AttachedSqliteDatabase final : public main::AttachedDatabase {
public:
    AttachedSqliteDatabase(std::string dbName, std::string dbType,
        std::unique_ptr<extension::CatalogExtension> catalog,
        std::unique_ptr<SqliteConnector> connector)
        : main::AttachedDatabase{std::move(dbName), std::move(dbType), std::move(catalog)},
          connector_{std::move(connector)} {}

    const SqliteConnector& getConnector() const { return *connector_; }

    static std::string stripQuotes(const std::string& name) {
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
            std::string out;
            for (auto i = 1u; i + 1 < name.size(); i++) {
                if (name[i] == '"' && i + 1 < name.size() - 1 && name[i + 1] == '"') {
                    out.push_back('"');
                    i++;
                } else {
                    out.push_back(name[i]);
                }
            }
            return out;
        }
        return name;
    }

    std::vector<std::string> getTableColumnNames(const std::string& tableName) const override {
        // Accept bare table names as well as `schema.table` references (the
        // scan descriptions qualify tables as `"main"."table"`). Dots
        // inside quoted identifiers are not supported -- matching the
        // optimizer's own limitation.
        auto unqualified = tableName;
        auto dotPos = tableName.rfind('.');
        if (dotPos != std::string::npos) {
            unqualified = tableName.substr(dotPos + 1);
        }
        unqualified = stripQuotes(unqualified);
        // Mirror the pg_client contract: when the table has src/dst endpoint
        // columns (rel tables), list them first so the join push-down
        // optimizer finds the FK columns at positions 0 and 1.
        std::string srcCol, dstCol;
        for (auto& fk : connector_->getForeignKeys(unqualified)) {
            auto lower = fk.fromColumn;
            for (auto& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower.rfind("src", 0) == 0) {
                srcCol = fk.fromColumn;
            } else if (lower.rfind("dst", 0) == 0 || lower.rfind("dest", 0) == 0) {
                dstCol = fk.fromColumn;
            }
        }
        std::vector<std::string> columnNames;
        try {
            auto columns = connector_->getTableInfo(unqualified);
            if (!srcCol.empty()) {
                columnNames.push_back(srcCol);
            }
            if (!dstCol.empty()) {
                columnNames.push_back(dstCol);
            }
            for (auto& col : columns) {
                if (col.name != srcCol && col.name != dstCol) {
                    columnNames.push_back(col.name);
                }
            }
        } catch (const common::Exception&) {
            return {};
        }
        return columnNames;
    }

private:
    std::unique_ptr<SqliteConnector> connector_;
};

} // namespace sqlite_extension
} // namespace lbug
