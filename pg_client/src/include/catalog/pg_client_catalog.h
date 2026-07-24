#pragma once

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/vector/value_vector.h"
#include "extension/catalog_extension.h"

namespace lbug {
namespace binder {
struct AttachOption;
} // namespace binder
} // namespace lbug
#include <libpq-fe.h>
#include <vector>

namespace lbug {
namespace pg_client_extension {

class PgClientConnector;

struct PgClientColumnInfo {
    std::string name;
    common::LogicalType type;
};

common::LogicalType pgTypeNameToLogicalType(const std::string& pgType);

class PgClientCatalog : public extension::CatalogExtension {
public:
    PgClientCatalog(std::string connStr, std::string catalogName,
        std::string defaultSchemaName, main::ClientContext* context,
        const PgClientConnector& connector);

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

    std::vector<PgClientColumnInfo> getTableColumnInfo(
        const std::string& tableName) const;
    std::vector<std::string> getColumnNames(
        const std::vector<PgClientColumnInfo>& columns) const;
    std::vector<common::LogicalType> getColumnTypes(
        const std::vector<PgClientColumnInfo>& columns) const;

    std::string connStr;
    std::string catalogName;
    std::string defaultSchemaName;
    const PgClientConnector& connector;
    main::ClientContext* context_;
};

} // namespace pg_client_extension
} // namespace lbug
