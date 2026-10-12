#pragma once

#include <string>
#include <vector>

#include "common/types/types.h"
#include "connector/sqlite_connector.h"
#include "extension/catalog_extension.h"

namespace lbug {
namespace binder {
struct AttachOption;
} // namespace binder

namespace sqlite_extension {

class SqliteCatalog : public extension::CatalogExtension {
public:
    SqliteCatalog(std::string dbPath, std::string attachedDbName, main::ClientContext* context,
        const SqliteConnector& connector);

    void init() override;

private:
    void createForeignTable(const std::string& tableName);
    void createForeignRelTable(const std::string& tableName);

    bool bindPropertyDefinitions(const std::string& tableName,
        std::vector<binder::PropertyDefinition>& propertyDefinitions);

private:
    std::string dbPath;
    std::string dbName;
    const SqliteConnector& connector;
    main::ClientContext* context_;
};

} // namespace sqlite_extension
} // namespace lbug
