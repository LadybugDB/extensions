#include "catalog/sqlite_catalog.h"

#include <optional>

#include "binder/bound_attach_info.h"
#include "binder/ddl/property_definition.h"
#include "binder/expression/variable_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/sqlite_table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "connector/sqlite_connector.h"
#include "connector/sqlite_type_converter.h"
#include "function/sqlite_scan.h"
#include "main/client_context.h"
#include "main/database.h"
#include "storage/storage_manager.h"
#include <format>

namespace lbug {
namespace sqlite_extension {

SqliteCatalog::SqliteCatalog(std::string dbPath, std::string attachedDbName,
    main::ClientContext* context, const SqliteConnector& connector)
    : CatalogExtension{}, dbPath{std::move(dbPath)}, dbName{std::move(attachedDbName)},
      connector{connector}, context_{context} {}

void SqliteCatalog::init() {
    auto tableNames = connector.listTables();
    if (tableNames.empty()) {
        return;
    }
    // Two-pass initialization: node tables must be registered before rel
    // tables so that rel tables can resolve their src/dst node table IDs.
    for (auto& tableName : tableNames) {
        auto lowerName = tableName;
        common::StringUtils::toLower(lowerName);
        if (lowerName.rfind("rel_", 0) == 0 || lowerName.rfind("csr_rel_", 0) == 0) {
            continue;
        }
        createForeignTable(tableName);
    }
    for (auto& tableName : tableNames) {
        auto lowerName = tableName;
        common::StringUtils::toLower(lowerName);
        if (lowerName.rfind("rel_", 0) == 0) {
            // Foreign-key-based rel table: scan-driven, optimizer generates a join.
            createForeignRelTable(tableName);
        } else if (lowerName.rfind("csr_rel_", 0) == 0) {
            // CSR-based rel table: the csr_rel_ prefix promises that the
            // foreign keys are usable as node offsets directly.
            createForeignRelTable(tableName);
        }
    }
}

bool SqliteCatalog::bindPropertyDefinitions(const std::string& tableName,
    std::vector<binder::PropertyDefinition>& propertyDefinitions) {
    auto columns = connector.getTableInfo(tableName);
    if (columns.empty()) {
        return false;
    }
    for (auto& col : columns) {
        auto logicalType = sqliteDeclaredTypeToLogicalType(col.declaredType);
        auto columnDefinition = binder::ColumnDefinition(col.name, logicalType.copy());
        propertyDefinitions.emplace_back(std::move(columnDefinition));
    }
    return true;
}

void SqliteCatalog::createForeignTable(const std::string& tableName) {
    std::vector<binder::PropertyDefinition> propertyDefinitions;
    if (!bindPropertyDefinitions(tableName, propertyDefinitions)) {
        return;
    }
    std::vector<common::LogicalType> columnTypes;
    std::vector<std::string> columnNames;
    for (auto& definition : propertyDefinitions) {
        columnNames.push_back(definition.getName());
        columnTypes.push_back(definition.getType().copy());
    }
    // Qualify with the default `main` schema so the join push-down optimizer can
    // re-prefix the reference when it splices rel tables into pushed SQL
    // (mirroring the pg_client `"public".table` convention). `main` always
    // exists in SQLite and keeps the reference valid when executed.
    auto query =
        std::format("SELECT {{}} FROM \"main\".{}", SqliteConnector::quoteIdentifier(tableName));
    auto scanInfo = std::make_shared<SqliteTableScanInfo>(query, std::move(columnTypes),
        columnNames, connector);
    auto scanFunction = getScanFunction(scanInfo);
    auto tableEntry =
        std::make_unique<catalog::SqliteTableCatalogEntry>(tableName, scanFunction, scanInfo);
    for (auto& definition : propertyDefinitions) {
        tableEntry->addProperty(definition);
    }
    // Capture the raw pointer before moving: the catalog takes ownership, so
    // the pointer stays valid while the moved-from unique_ptr is null.
    auto* attachedEntry = tableEntry.get();
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(tableEntry));

    // Shadow entry in the main catalog for traversal support.
    auto primaryKeyName = propertyDefinitions[0].getName();
    // Must be the attached-database name: the join-push-down optimizer uses
    // it as a lookup key into DatabaseManager::getAttachedDatabase().
    auto foreignDatabaseName = dbName;
    auto mainTableEntry = std::make_unique<catalog::NodeTableCatalogEntry>(tableName,
        primaryKeyName, foreignDatabaseName, catalog::ShadowTag{});
    for (auto& definition : propertyDefinitions) {
        mainTableEntry->addProperty(definition);
    }
    mainTableEntry->setReferencedEntry(attachedEntry);
    context_->getDatabase()->getCatalog()->addTableEntry(std::move(mainTableEntry));
    auto mainEntry = context_->getDatabase()->getCatalog()->getTableCatalogEntry(
        &transaction::DUMMY_TRANSACTION, tableName);
    lbug::storage::StorageManager::Get(*context_)->createTable(mainEntry);
}

void SqliteCatalog::createForeignRelTable(const std::string& tableName) {
    // Resolve src/dst node tables from foreign-key metadata. The FK columns
    // are matched by the same src/dst/dest-prefix convention the join
    // push-down optimizer uses for endpoint columns.
    std::string srcTableName, dstTableName;
    std::string srcColName, dstColName;
    for (auto& fk : connector.getForeignKeys(tableName)) {
        auto lowerCol = fk.fromColumn;
        common::StringUtils::toLower(lowerCol);
        if (lowerCol.rfind("src", 0) == 0) {
            srcTableName = fk.referencedTable;
            srcColName = fk.fromColumn;
        } else if (lowerCol.rfind("dst", 0) == 0 || lowerCol.rfind("dest", 0) == 0) {
            dstTableName = fk.referencedTable;
            dstColName = fk.fromColumn;
        }
    }
    if (srcTableName.empty() || dstTableName.empty()) {
        createForeignTable(tableName);
        return;
    }

    std::vector<binder::PropertyDefinition> propertyDefinitions;
    if (!bindPropertyDefinitions(tableName, propertyDefinitions)) {
        return;
    }

    auto* catalog = context_->getDatabase()->getCatalog();
    if (!catalog->containsTable(&transaction::DUMMY_TRANSACTION, srcTableName) ||
        !catalog->containsTable(&transaction::DUMMY_TRANSACTION, dstTableName)) {
        createForeignTable(tableName);
        return;
    }
    auto* srcEntry = catalog->getTableCatalogEntry(&transaction::DUMMY_TRANSACTION, srcTableName);
    auto* dstEntry = catalog->getTableCatalogEntry(&transaction::DUMMY_TRANSACTION, dstTableName);
    if (srcEntry == nullptr || dstEntry == nullptr) {
        createForeignTable(tableName);
        return;
    }
    common::table_id_t srcTableID = srcEntry->getTableID();
    common::table_id_t dstTableID = dstEntry->getTableID();

    std::vector<common::LogicalType> columnTypes;
    std::vector<std::string> columnNames;
    for (auto& def : propertyDefinitions) {
        columnNames.push_back(def.getName());
        columnTypes.push_back(def.getType().copy());
    }
    binder::expression_vector columns;
    for (auto i = 0u; i < columnTypes.size(); i++) {
        columns.push_back(std::make_shared<binder::VariableExpression>(columnTypes[i].copy(),
            columnNames[i], columnNames[i]));
    }

    // Qualify with the default `main` schema so the join push-down optimizer can
    // re-prefix the reference when it splices rel tables into pushed SQL
    // (mirroring the pg_client `"public".table` convention). `main` always
    // exists in SQLite and keeps the reference valid when executed.
    auto query =
        std::format("SELECT {{}} FROM \"main\".{}", SqliteConnector::quoteIdentifier(tableName));
    auto scanInfo = std::make_shared<SqliteTableScanInfo>(query, std::move(columnTypes),
        columnNames, connector);
    auto scanFunc = getScanFunction(scanInfo);

    auto tableEntry =
        std::make_unique<catalog::SqliteTableCatalogEntry>(tableName, scanFunc, scanInfo);
    for (auto& def : propertyDefinitions) {
        tableEntry->addProperty(def);
    }
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(tableEntry));

    auto lowerName = tableName;
    common::StringUtils::toLower(lowerName);
    bool internalIDContract = lowerName.rfind("csr_rel_", 0) == 0;
    std::optional<std::pair<common::column_id_t, common::column_id_t>> srcDstColumnPositions;
    if (internalIDContract) {
        auto findPos = [&columnNames](
                           const std::string& name) -> std::optional<common::column_id_t> {
            for (auto i = 0u; i < columnNames.size(); i++) {
                auto lower = columnNames[i];
                common::StringUtils::toLower(lower);
                auto lowerName = name;
                common::StringUtils::toLower(lowerName);
                if (lower == lowerName) {
                    return i;
                }
            }
            return std::nullopt;
        };
        auto srcPos = findPos(srcColName);
        auto dstPos = findPos(dstColName);
        if (srcPos && dstPos) {
            srcDstColumnPositions = std::make_pair(*srcPos, *dstPos);
        }
    }
    auto bindData = std::make_shared<SqliteScanBindData>(query, columnNames, connector,
        std::move(columns), srcDstColumnPositions);
    auto foreignDatabaseName = dbName;

    std::vector<catalog::RelTableCatalogInfo> relTableInfos;
    common::oid_t relOID = tables->getNextOID();
    relTableInfos.emplace_back(catalog::NodeTableIDPair{srcTableID, dstTableID}, relOID,
        common::RelMultiplicity::MANY, common::RelMultiplicity::MANY);

    auto relGroupEntry = std::make_unique<catalog::RelGroupCatalogEntry>(tableName,
        common::RelMultiplicity::MANY, common::RelMultiplicity::MANY, common::ExtendDirection::BOTH,
        std::move(relTableInfos), dbName + "." + tableName, common::StorageFormat::NONE, scanFunc,
        bindData, std::move(foreignDatabaseName));
    for (auto& def : propertyDefinitions) {
        relGroupEntry->addProperty(def);
    }
    context_->getDatabase()->getCatalog()->addTableEntry(std::move(relGroupEntry));
    auto mainEntry = context_->getDatabase()->getCatalog()->getTableCatalogEntry(
        &transaction::DUMMY_TRANSACTION, tableName);
    if (mainEntry) {
        storage::StorageManager::Get(*context_)->createTable(mainEntry);
    }
}

} // namespace sqlite_extension
} // namespace lbug
