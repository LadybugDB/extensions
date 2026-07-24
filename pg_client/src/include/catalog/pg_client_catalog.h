#pragma once

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/vector/value_vector.h"
#include "extension/catalog_extension.h"
#include <libpq-fe.h>

namespace lbug {
namespace pg_client_extension {

class PgClientCatalog : public extension::CatalogExtension {
public:
    PgClientCatalog(std::string connStr, std::string catalogName,
        std::string defaultSchemaName, main::ClientContext* context,
        PGconn* pgConn);

    void init() override;

    static std::string bindSchemaName(const binder::AttachOption& options,
        const std::string& defaultName);

private:
    void createForeignNodeTable(const std::string& tableName,
        const std::vector<binder::PropertyDefinition>& properties,
        const std::string& primaryKey);
    void createForeignRelTable(const std::string& tableName,
        const std::vector<binder::PropertyDefinition>& properties,
        const std::string& srcTable, const std::string& dstTable);

    std::string connStr;
    std::string catalogName;
    std::string defaultSchemaName;
    PGconn* pgConn;
    main::ClientContext* context_;
};

} // namespace pg_client_extension
} // namespace lbug
