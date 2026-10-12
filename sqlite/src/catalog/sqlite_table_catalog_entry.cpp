#include "catalog/sqlite_table_catalog_entry.h"

#include "binder/bound_scan_source.h"
#include "binder/expression/variable_expression.h"
#include "common/constants.h"
#include "function/sqlite_scan.h"

namespace lbug {
namespace catalog {

SqliteTableCatalogEntry::SqliteTableCatalogEntry(std::string name,
    std::optional<function::TableFunction> scanFunction,
    std::shared_ptr<sqlite_extension::SqliteTableScanInfo> scanInfo)
    : TableCatalogEntry{CatalogEntryType::FOREIGN_TABLE_ENTRY, std::move(name)},
      scanFunction{std::move(scanFunction)}, scanInfo{std::move(scanInfo)} {}

common::TableType SqliteTableCatalogEntry::getTableType() const {
    return common::TableType::FOREIGN;
}

std::unique_ptr<binder::BoundTableScanInfo> SqliteTableCatalogEntry::getBoundScanInfo(
    main::ClientContext* context, const std::string& nodeUniqueName) {
    auto columnNames = scanInfo->getColumnNames();
    auto columnTypes = scanInfo->getColumnTypes(*context);
    binder::expression_vector columns;

    // Add a synthesized internal ID if the scan runs under a node pattern.
    // SQLite's own rowid is deliberately not used: WITHOUT ROWID tables have
    // none, and INTEGER PRIMARY KEY aliasing would make IDs inconsistent
    // across tables. Row indices are stable within a single scan.
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

    auto bindData =
        std::make_unique<sqlite_extension::SqliteScanBindData>(scanInfo->getTemplateQuery(*context),
            scanInfo->getColumnNames(), scanInfo->getConnector(), std::move(columns));
    return std::make_unique<binder::BoundTableScanInfo>(scanFunction, std::move(bindData));
}

std::unique_ptr<TableCatalogEntry> SqliteTableCatalogEntry::copy() const {
    auto other = std::make_unique<SqliteTableCatalogEntry>(name, scanFunction, scanInfo);
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo>
SqliteTableCatalogEntry::getBoundExtraCreateInfo(transaction::Transaction*) const {
    UNREACHABLE_CODE;
}

} // namespace catalog
} // namespace lbug
