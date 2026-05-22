#include "catalog/adbc_catalog.h"

#include "catalog/adbc_table_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "function/adbc_scan.h"
#include "main/client_context.h"
#include "main/database.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"

namespace lbug {
namespace adbc_extension {

void ADBCCatalog::init() {
    for (auto& tableName : connector.getTableNames()) {
        createForeignTable(tableName);
    }
}

void ADBCCatalog::createForeignTable(const std::string& tableName) {
    auto columns = connector.getTableSchema(schemaName, tableName);
    std::vector<std::string> columnNames;
    std::vector<common::LogicalType> columnTypes;
    for (auto& [name, type] : columns) {
        columnNames.push_back(name);
        columnTypes.push_back(type.copy());
    }
    auto scanInfo = std::make_shared<ADBCTableScanInfo>(tableName, columnNames,
        copyVector(columnTypes), connector);
    auto attachedEntry = std::make_unique<catalog::ADBCTableCatalogEntry>(tableName,
        getADBCScanFunction(scanInfo), scanInfo);
    for (auto i = 0u; i < columnNames.size(); i++) {
        attachedEntry->addProperty(binder::PropertyDefinition{
            binder::ColumnDefinition{columnNames[i], columnTypes[i].copy()}});
    }
    auto attachedEntryPtr = attachedEntry.get();
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(attachedEntry));
    auto primaryKeyName = columnNames[0];
    auto mainTableEntry = std::make_unique<catalog::NodeTableCatalogEntry>(tableName,
        primaryKeyName, tableName, catalog::ShadowTag{});
    for (auto i = 0u; i < columnNames.size(); i++) {
        mainTableEntry->addProperty(binder::PropertyDefinition{
            binder::ColumnDefinition{columnNames[i], columnTypes[i].copy()}});
    }
    mainTableEntry->setReferencedEntry(attachedEntryPtr);
    context->getDatabase()->getCatalog()->addTableEntry(std::move(mainTableEntry));
    auto mainEntry = context->getDatabase()->getCatalog()->getTableCatalogEntry(
        &transaction::DUMMY_TRANSACTION, tableName);
    lbug::storage::StorageManager::Get(*context)->createTable(mainEntry);
}

} // namespace adbc_extension
} // namespace lbug
