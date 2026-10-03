#pragma once

#include "binder/ddl/property_definition.h"
#include "connector/adbc_connector.h"
#include "extension/catalog_extension.h"

namespace lbug {
namespace adbc_extension {

class ADBCCatalog final : public extension::CatalogExtension {
public:
    ADBCCatalog(std::string catalogName, std::string schemaName, main::ClientContext* context,
        const ADBCConnector& connector)
        : catalogName{std::move(catalogName)}, schemaName{std::move(schemaName)}, context{context},
          connector{connector} {}

    void init() override;

private:
    void createForeignTable(const std::string& tableName);

private:
    std::string catalogName;
    std::string schemaName;
    main::ClientContext* context;
    const ADBCConnector& connector;
};

} // namespace adbc_extension
} // namespace lbug
