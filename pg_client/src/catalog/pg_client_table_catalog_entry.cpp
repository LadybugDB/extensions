#include "catalog/pg_client_table_catalog_entry.h"

#include "binder/bound_scan_source.h"
#include "binder/expression/variable_expression.h"
#include "common/constants.h"
#include "function/pg_client_scan.h"

namespace lbug {
namespace catalog {

PgClientTableCatalogEntry::PgClientTableCatalogEntry(std::string name,
    std::optional<function::TableFunction> scanFunction,
    std::shared_ptr<pg_client_extension::PgClientTableScanInfo> scanInfo)
    : TableCatalogEntry{CatalogEntryType::FOREIGN_TABLE_ENTRY, std::move(name)},
      scanFunction{std::move(scanFunction)}, scanInfo{std::move(scanInfo)} {}

common::TableType PgClientTableCatalogEntry::getTableType() const {
    return common::TableType::FOREIGN;
}

std::unique_ptr<binder::BoundTableScanInfo> PgClientTableCatalogEntry::getBoundScanInfo(
    main::ClientContext* context, const std::string& nodeUniqueName) {
    auto columnNames = scanInfo->getColumnNames();
    auto columnTypes = scanInfo->getColumnTypes(*context);
    binder::expression_vector columns;

    // Add rowid as _ID (internal ID) if nodeUniqueName is provided
    if (!nodeUniqueName.empty()) {
        auto idUniqueName = nodeUniqueName + "." + std::string(common::InternalKeyword::ID);
        columns.push_back(std::make_shared<binder::VariableExpression>(common::LogicalType::INT64(),
            idUniqueName, "rowid"));
    }

    for (auto i = 0u; i < columnNames.size(); i++) {
        std::string uniqueName = columnNames[i];
        if (!nodeUniqueName.empty()) {
            uniqueName = nodeUniqueName + "." + columnNames[i];
        }
        columns.push_back(std::make_shared<binder::VariableExpression>(std::move(columnTypes[i]),
            uniqueName, columnNames[i]));
    }

    // Build column names for PG query (no rowid — PG doesn't have a rowid pseudo-column)
    // The internal ID column will be synthesized by the table function from row indices
    std::vector<std::string> pgColumnNames = columnNames;

    auto bindData =
        std::make_unique<pg_client_extension::PgClientScanBindData>(
            scanInfo->getTemplateQuery(*context),
            pgColumnNames, scanInfo->getConnector(), std::move(columns));
    return std::make_unique<binder::BoundTableScanInfo>(scanFunction, std::move(bindData));
}

std::unique_ptr<TableCatalogEntry> PgClientTableCatalogEntry::copy() const {
    auto other = std::make_unique<PgClientTableCatalogEntry>(name, scanFunction, scanInfo);
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo>
PgClientTableCatalogEntry::getBoundExtraCreateInfo(transaction::Transaction*) const {
    UNREACHABLE_CODE;
}

} // namespace catalog
} // namespace lbug
