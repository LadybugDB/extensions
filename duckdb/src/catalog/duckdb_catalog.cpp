#include "catalog/duckdb_catalog.h"

#include <optional>
#include <regex>
#include <utility>

#include "binder/bound_attach_info.h"
#include "binder/expression/variable_expression.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/duckdb_table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "connector/duckdb_type_converter.h"
#include "function/duckdb_scan.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/duckdb_storage.h"
#include "storage/storage_manager.h"
#include <format>

namespace lbug {
namespace duckdb_extension {

DuckDBCatalog::DuckDBCatalog(std::string dbPath, std::string catalogName,
    std::string defaultSchemaName, main::ClientContext* context, const DuckDBConnector& connector,
    const binder::AttachOption& attachOption, std::string attachedDbName)
    : CatalogExtension{}, dbPath{std::move(dbPath)}, catalogName{std::move(catalogName)},
      defaultSchemaName{std::move(defaultSchemaName)},
      dbName{std::move(attachedDbName)},
      tableNamesVector{common::LogicalType::STRING(), storage::MemoryManager::Get(*context)},
      connector{connector}, context_{context} {
    skipUnsupportedTable = DuckDBStorageExtension::SKIP_UNSUPPORTED_TABLE_DEFAULT_VAL;
    auto& options = attachOption.options;
    if (options.contains(DuckDBStorageExtension::SKIP_UNSUPPORTED_TABLE_KEY)) {
        auto val = options.at(DuckDBStorageExtension::SKIP_UNSUPPORTED_TABLE_KEY);
        if (val.getDataType().getLogicalTypeID() != common::LogicalTypeID::BOOL) {
            throw common::RuntimeException{std::format("Invalid option value for {}",
                DuckDBStorageExtension::SKIP_UNSUPPORTED_TABLE_KEY)};
        }
        skipUnsupportedTable = val.getValue<bool>();
    }
}

void DuckDBCatalog::init() {
    auto query = std::format(
        "select table_name from information_schema.tables where table_catalog = '{}' and "
        "table_schema = '{}' order by table_name;",
        catalogName, defaultSchemaName);
    auto result = connector.executeQuery(query);
    std::unique_ptr<duckdb::DataChunk> resultChunk;
    try {
        resultChunk = result->Fetch();
    } catch (std::exception& e) {
        throw common::BinderException(e.what());
    }
    if (resultChunk == nullptr || resultChunk->size() == 0) {
        return;
    }
    duckdb_conversion_func_t conversionFunc;
    DuckDBResultConverter::getDuckDBVectorConversionFunc(common::PhysicalTypeID::STRING,
        conversionFunc);
    conversionFunc(resultChunk->data[0], tableNamesVector, resultChunk->size());
    // Two-pass initialization: node tables must be registered before rel tables
    // so that rel tables can resolve their src/dst node table IDs. The table
    // enumeration order is alphabetical, which can put rel_* tables before the
    // node tables they reference.
    // First pass: register node tables (everything that is not a rel table).
    for (auto i = 0u; i < resultChunk->size(); i++) {
        auto tableName = tableNamesVector.getValue<common::string_t>(i).getAsString();
        auto lowerName = tableName;
        common::StringUtils::toLower(lowerName);
        if (lowerName.rfind("rel_", 0) == 0 || lowerName.rfind("csr_rel_", 0) == 0) {
            continue;
        }
        createForeignTable(tableName);
    }
    // Second pass: register rel tables.
    for (auto i = 0u; i < resultChunk->size(); i++) {
        auto tableName = tableNamesVector.getValue<common::string_t>(i).getAsString();
        auto lowerName = tableName;
        common::StringUtils::toLower(lowerName);
        if (lowerName.rfind("rel_", 0) == 0) {
            // Foreign-key-based rel table: scan-driven, optimizer generates a join.
            // No CSR columns; backed by a ForeignRelTable. Foreign keys reference
            // primary keys, which are not guaranteed to equal node offsets.
            createForeignRelTable(tableName, false /* internalIDContract */);
        } else if (lowerName.rfind("csr_rel_", 0) == 0) {
            // CSR-based rel table: materialized into a local on-disk CSR rel table.
            // TODO: COPY data from DuckDB into a local RelTable.
            // The csr_rel_ prefix promises that the foreign keys are usable as
            // node offsets directly (dense, gapless, aligned with the node
            // tables' internal ID scheme), enabling MATCH traversal.
            createForeignRelTable(tableName, true /* internalIDContract */);
        }
    }
}

std::string DuckDBCatalog::bindSchemaName(const binder::AttachOption& options,
    const std::string& defaultName) {
    if (options.options.contains(DuckDBStorageExtension::SCHEMA_OPTION)) {
        auto val = options.options.at(DuckDBStorageExtension::SCHEMA_OPTION);
        if (val.getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING) {
            throw common::RuntimeException{
                std::format("Invalid option value for {}", DuckDBStorageExtension::SCHEMA_OPTION)};
        }
        return val.getValue<std::string>();
    }
    return defaultName;
}

static std::string getQuery(const binder::BoundCreateTableInfo& info) {
    auto extraInfo = info.extraInfo->constPtrCast<BoundExtraCreateDuckDBTableInfo>();
    return "SELECT {} " + std::format("FROM \"{}\".{}.{}", extraInfo->catalogName,
                              extraInfo->schemaName, info.tableName);
}

void DuckDBCatalog::createForeignTable(const std::string& tableName) {
    auto info = bindCreateTableInfo(tableName);
    if (info == nullptr) {
        return;
    }
    auto extraInfo =
        common::dynamic_cast_checked<BoundExtraCreateDuckDBTableInfo*>(info->extraInfo.get());
    std::vector<common::LogicalType> columnTypes;
    std::vector<std::string> columnNames;
    for (auto& definition : extraInfo->propertyDefinitions) {
        columnNames.push_back(definition.getName());
        columnTypes.push_back(definition.getType().copy());
    }
    auto duckdbTableInfo =
        connector.getTableScanInfo(getQuery(*info), std::move(columnTypes), columnNames);
    auto tableEntry = std::make_unique<catalog::DuckDBTableCatalogEntry>(info->tableName,
        getScanFunction(duckdbTableInfo), duckdbTableInfo);
    for (auto& definition : extraInfo->propertyDefinitions) {
        tableEntry->addProperty(definition);
    }
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(tableEntry));
    auto attachedEntry = tableEntry.get();
    // Create another for main catalog for traversal support
    auto primaryKeyName = extraInfo->propertyDefinitions[0].getName();

    // Create DuckDB scan function for SQL pushdown
    auto scanFunction = getScanFunction(duckdbTableInfo);
    // Must be the attached-database name: the join-push-down optimizer uses it
    // as a lookup key into DatabaseManager::getAttachedDatabase().
    auto foreignDatabaseName = dbName;
    auto mainTableEntry = std::make_unique<catalog::NodeTableCatalogEntry>(info->tableName,
        primaryKeyName, foreignDatabaseName, catalog::ShadowTag{});
    for (auto& definition : extraInfo->propertyDefinitions) {
        mainTableEntry->addProperty(definition);
    }
    mainTableEntry->setReferencedEntry(attachedEntry);
    context_->getDatabase()->getCatalog()->addTableEntry(std::move(mainTableEntry));
    auto mainEntry = context_->getDatabase()->getCatalog()->getTableCatalogEntry(
        &transaction::DUMMY_TRANSACTION, info->tableName);
    lbug::storage::StorageManager::Get(*context_)->createTable(mainEntry);
}

void DuckDBCatalog::createForeignRelTable(const std::string& tableName, bool internalIDContract) {
    // Query foreign key info to find src/dst node tables.
    //
    // information_schema.constraint_column_usage is unusable for this in
    // DuckDB: for FK constraints it reports the constraint's own table (the
    // referencing side), not the referenced one. duckdb_constraints() exposes
    // the referenced table directly; unnest() flattens the column list so each
    // row yields (fk_column, referenced_table).
    auto fkQuery = std::format("SELECT unnest(constraint_column_names) AS column_name, "
                               "referenced_table FROM duckdb_constraints() "
                               "WHERE constraint_type = 'FOREIGN KEY' "
                               "AND referenced_table IS NOT NULL AND table_name = '{}'",
        tableName);
    auto fkResult = connector.executeQuery(fkQuery);

    std::string srcTableName, dstTableName;
    std::string srcColName, dstColName;
    for (auto i = 0u; i < fkResult->RowCount(); i++) {
        auto colName = fkResult->GetValue(0, i).GetValue<std::string>();
        auto refTable = fkResult->GetValue(1, i).GetValue<std::string>();
        auto lowerCol = colName;
        common::StringUtils::toLower(lowerCol);
        if (lowerCol == "src_id" || lowerCol.find("src") == 0) {
            srcTableName = refTable;
            srcColName = colName;
        } else if (lowerCol == "dst_id" || lowerCol.find("dst") == 0 ||
                   lowerCol.find("dest") == 0) {
            dstTableName = refTable;
            dstColName = colName;
        }
    }

    if (srcTableName.empty() || dstTableName.empty()) {
        createForeignTable(tableName);
        return;
    }

    // Build property definitions
    std::vector<binder::PropertyDefinition> propertyDefinitions;
    bindPropertyDefinitions(tableName, propertyDefinitions);

    // Determine the node table IDs from the main catalog. containsTable() must
    // be checked first: getTableCatalogEntry() throws when the table is
    // missing, and a rel table may reference tables that were not registered.
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

    // Build columns for the scan bind data (must happen before columnTypes is
    // moved into the scan info below).
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

    // Build query and scan info
    auto queryStr =
        std::format("SELECT * FROM \"{}\".{}.{}", catalogName, defaultSchemaName, tableName);
    auto duckdbTableInfo = std::make_shared<DuckDBTableScanInfo>(queryStr, std::move(columnTypes),
        columnNames, connector);
    auto scanFunc = getScanFunction(duckdbTableInfo);

    // Create DuckDB table catalog entry
    auto tableEntry =
        std::make_unique<catalog::DuckDBTableCatalogEntry>(tableName, scanFunc, duckdbTableInfo);
    for (auto& def : propertyDefinitions) {
        tableEntry->addProperty(def);
    }
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(tableEntry));

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
    auto bindData = std::make_shared<DuckDBScanBindData>(queryStr, columnNames, connector,
        std::move(columns), srcDstColumnPositions);
    // Create RelGroupCatalogEntry. foreignDatabaseName must be the
    // attached-database name (see the node shadow comment above).
    auto foreignDatabaseName = dbName;

    std::vector<catalog::RelTableCatalogInfo> relTableInfos;
    auto info = bindCreateTableInfo(tableName);
    common::oid_t relOID = tables->getNextOID();
    relTableInfos.emplace_back(catalog::NodeTableIDPair{srcTableID, dstTableID}, relOID,
        common::RelMultiplicity::MANY, common::RelMultiplicity::MANY);

    auto relGroupEntry =
        std::make_unique<catalog::RelGroupCatalogEntry>(tableName, common::RelMultiplicity::MANY,
            common::RelMultiplicity::MANY, common::ExtendDirection::BOTH, std::move(relTableInfos),
            // "<attachedDb>.<table>" matches the pg_client convention; the
            // join-push-down optimizer extracts the table name after the dot.
            dbName + "." + tableName, common::StorageFormat::NONE, scanFunc, bindData,
            std::move(foreignDatabaseName));

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

static bool getTableInfo(const DuckDBConnector& connector, const std::string& tableName,
    const std::string& schemaName, const std::string& catalogName,
    std::vector<common::LogicalType>& columnTypes, std::vector<std::string>& columnNames,
    bool skipUnsupportedTable) {
    auto query = std::format("select data_type,column_name from information_schema.columns where "
                             "table_name = '{}' and table_schema = '{}' and table_catalog = '{}' "
                             "order by ordinal_position;",
        tableName, schemaName, catalogName);
    auto result = connector.executeQuery(query);
    if (result->RowCount() == 0) {
        return false;
    }
    columnTypes.reserve(result->RowCount());
    columnNames.reserve(result->RowCount());
    for (auto i = 0u; i < result->RowCount(); i++) {
        try {
            columnTypes.push_back(DuckDBTypeConverter::convertDuckDBType(
                result->GetValue(0, i).GetValue<std::string>()));
        } catch (common::BinderException& e) {
            if (skipUnsupportedTable) {
                return false;
            }
            throw;
        }
        columnNames.push_back(result->GetValue(1, i).GetValue<std::string>());
    }
    return true;
}

bool DuckDBCatalog::bindPropertyDefinitions(const std::string& tableName,
    std::vector<binder::PropertyDefinition>& propertyDefinitions) {
    std::vector<common::LogicalType> columnTypes;
    std::vector<std::string> columnNames;
    if (!getTableInfo(connector, tableName, defaultSchemaName, catalogName, columnTypes,
            columnNames, skipUnsupportedTable)) {
        return false;
    }
    for (auto i = 0u; i < columnNames.size(); i++) {
        auto columnDefinition = binder::ColumnDefinition(columnNames[i], columnTypes[i].copy());
        auto propertyDefinition = binder::PropertyDefinition(std::move(columnDefinition));
        propertyDefinitions.push_back(std::move(propertyDefinition));
    }
    return true;
}

std::unique_ptr<binder::BoundCreateTableInfo> DuckDBCatalog::bindCreateTableInfo(
    const std::string& tableName) {
    std::vector<binder::PropertyDefinition> propertyDefinitions;
    if (!bindPropertyDefinitions(tableName, propertyDefinitions)) {
        return nullptr;
    }
    return std::make_unique<binder::BoundCreateTableInfo>(
        catalog::CatalogEntryType::FOREIGN_TABLE_ENTRY, tableName,
        common::ConflictAction::ON_CONFLICT_THROW,
        std::make_unique<duckdb_extension::BoundExtraCreateDuckDBTableInfo>(catalogName,
            defaultSchemaName, std::move(propertyDefinitions)),
        false /* isInternal */);
}

} // namespace duckdb_extension
} // namespace lbug
