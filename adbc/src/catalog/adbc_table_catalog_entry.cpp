#include "catalog/adbc_table_catalog_entry.h"

#include "binder/bound_scan_source.h"
#include "binder/expression/variable_expression.h"
#include "common/constants.h"

namespace lbug {
namespace catalog {

ADBCTableCatalogEntry::ADBCTableCatalogEntry(std::string name,
    std::optional<function::TableFunction> scanFunction,
    std::shared_ptr<adbc_extension::ADBCTableScanInfo> scanInfo)
    : TableCatalogEntry{CatalogEntryType::FOREIGN_TABLE_ENTRY, std::move(name)},
      scanFunction{std::move(scanFunction)}, scanInfo{std::move(scanInfo)} {}

common::TableType ADBCTableCatalogEntry::getTableType() const {
    return common::TableType::FOREIGN;
}

std::unique_ptr<binder::BoundTableScanInfo> ADBCTableCatalogEntry::getBoundScanInfo(
    main::ClientContext* /*context*/, const std::string& nodeUniqueName) {
    binder::expression_vector columns;
    std::vector<std::string> scanColumnNames;
    std::vector<common::LogicalType> scanColumnTypes;
    if (!nodeUniqueName.empty()) {
        auto idUniqueName = nodeUniqueName + "." + std::string(common::InternalKeyword::ID);
        columns.push_back(std::make_shared<binder::VariableExpression>(common::LogicalType::INT64(),
            idUniqueName, scanInfo->columnNames[0]));
        scanColumnNames.push_back(scanInfo->columnNames[0]);
        scanColumnTypes.push_back(common::LogicalType::INT64());
    }
    for (auto i = 0u; i < scanInfo->columnNames.size(); i++) {
        auto uniqueName = nodeUniqueName.empty() ? scanInfo->columnNames[i] :
                                                   nodeUniqueName + "." + scanInfo->columnNames[i];
        columns.push_back(std::make_shared<binder::VariableExpression>(
            scanInfo->columnTypes[i].copy(), uniqueName, scanInfo->columnNames[i]));
        scanColumnNames.push_back(scanInfo->columnNames[i]);
        scanColumnTypes.push_back(scanInfo->columnTypes[i].copy());
    }
    auto boundScanInfo = std::make_shared<adbc_extension::ADBCTableScanInfo>(scanInfo->tableName,
        std::move(scanColumnNames), std::move(scanColumnTypes), scanInfo->connector);
    auto bindData = std::make_unique<adbc_extension::ADBCScanBindData>(std::move(boundScanInfo),
        std::move(columns));
    return std::make_unique<binder::BoundTableScanInfo>(scanFunction, std::move(bindData));
}

std::unique_ptr<TableCatalogEntry> ADBCTableCatalogEntry::copy() const {
    auto other = std::make_unique<ADBCTableCatalogEntry>(name, scanFunction, scanInfo);
    other->copyFrom(*this);
    return other;
}

std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo>
ADBCTableCatalogEntry::getBoundExtraCreateInfo(transaction::Transaction*) const {
    UNREACHABLE_CODE;
}

} // namespace catalog
} // namespace lbug
