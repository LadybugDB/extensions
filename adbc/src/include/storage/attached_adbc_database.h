#pragma once

#include "connector/adbc_connector.h"
#include "main/attached_database.h"

namespace lbug {
namespace adbc_extension {

class AttachedADBCDatabase final : public main::AttachedDatabase {
public:
    AttachedADBCDatabase(std::string dbName, std::string dbType,
        std::unique_ptr<extension::CatalogExtension> catalog,
        std::unique_ptr<ADBCConnector> connector)
        : main::AttachedDatabase{std::move(dbName), std::move(dbType), std::move(catalog)},
          connector{std::move(connector)} {}

    std::vector<std::string> getTableColumnNames(const std::string& tableName) const override {
        std::vector<std::string> result;
        for (auto& [name, type] : connector->getTableSchema("", tableName)) {
            (void)type;
            result.push_back(name);
        }
        return result;
    }

private:
    std::unique_ptr<ADBCConnector> connector;
};

} // namespace adbc_extension
} // namespace lbug
