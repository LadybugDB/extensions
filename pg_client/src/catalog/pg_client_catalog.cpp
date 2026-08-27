#include "catalog/pg_client_catalog.h"

#include <format>
#include <libpq-fe.h>

#include "binder/bound_attach_info.h"
#include "binder/ddl/property_definition.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/pg_client_table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "connector/pg_client_connector.h"
#include "function/pg_client_scan.h"
#include "main/client_context.h"
#include "main/database.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/pg_client_storage.h"

namespace lbug {
namespace pg_client_extension {

PgClientCatalog::PgClientCatalog(std::string connStr, std::string catalogName,
    std::string defaultSchemaName, std::string attachedDbName,
    main::ClientContext* context, const PgClientConnector& connector)
    : CatalogExtension{}, connStr{std::move(connStr)}, catalogName{std::move(catalogName)},
      defaultSchemaName{std::move(defaultSchemaName)},
      attachedDbName{std::move(attachedDbName)}, connector{connector},
      context_{context} {}

std::string PgClientCatalog::bindSchemaName(const binder::AttachOption& options,
    const std::string& defaultName) {
    auto& opts = options;
    if (opts.options.contains(PgClientStorageExtension::SCHEMA_OPTION)) {
        auto val = opts.options.at(PgClientStorageExtension::SCHEMA_OPTION);
        if (val.getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING) {
            throw common::RuntimeException{
                std::format("Invalid option value for {}", PgClientStorageExtension::SCHEMA_OPTION)};
        }
        return val.getValue<std::string>();
    }
    return defaultName;
}

void PgClientCatalog::init() {
    // Query information_schema.tables to find node_* and rel_* tables
    auto query = std::format(
        "select table_name from information_schema.tables "
        "where table_catalog = '{}' and table_schema = '{}' "
        "order by table_name",
        catalogName, defaultSchemaName);

    auto result = connector.executeQuery(query);
    if (!result.success) {
        throw common::BinderException(
            std::format("Failed to query tables: {}", result.errorMessage));
    }

    for (auto& row : result.rows) {
        std::string tableName = row.cells[0].value;
        auto lowerName = tableName;
        common::StringUtils::toLower(lowerName);
        if (lowerName.rfind("node_", 0) == 0) {
            createForeignNodeTable(tableName);
        }
    }
    // Second pass: register rel tables. Node tables must be registered first so that
    // FK-based rel tables can resolve their src/dst node table IDs.
    for (auto& row : result.rows) {
        std::string tableName = row.cells[0].value;
        auto lowerName = tableName;
        common::StringUtils::toLower(lowerName);
        if (lowerName.rfind("rel_", 0) == 0) {
            // Foreign-key-based rel table: scan-driven, optimizer generates a join.
            // No CSR columns; backed by a ForeignRelTable.
            createForeignRelTable(tableName);
        } else if (lowerName.rfind("csr_rel_", 0) == 0) {
            // CSR-based rel table: materialized into a local on-disk CSR rel table.
            // TODO: COPY data from PostgreSQL into a local RelTable.
            createForeignRelTable(tableName);
        }
    }
}

static std::vector<PgClientColumnInfo> getTableColumnInfoFromConnector(
    const PgClientConnector& connector, const std::string& catalogName,
    const std::string& schemaName, const std::string& tableName) {
    std::vector<PgClientColumnInfo> result;

    auto query = std::format(
        "select column_name, data_type from information_schema.columns "
        "where table_catalog = '{}' and table_schema = '{}' "
        "and table_name = '{}' order by ordinal_position",
        catalogName, schemaName, tableName);

    auto queryResult = connector.executeQuery(query);
    if (!queryResult.success) {
        return result;
    }

    result.reserve(queryResult.rows.size());
    for (auto& row : queryResult.rows) {
        std::string colName = row.cells[0].value;
        std::string pgType = row.cells[1].value;
        result.push_back({colName, pgTypeNameToLogicalType(pgType)});
    }

    return result;
}

common::LogicalType pgTypeNameToLogicalType(const std::string& pgType) {
    auto upper = pgType;
    common::StringUtils::toUpper(upper);

    if (upper == "BOOLEAN" || upper == "BOOL") {
        return common::LogicalType(common::LogicalTypeID::BOOL);
    } else if (upper == "BIGINT" || upper == "INT8" || upper == "INTEGER" ||
               upper == "INT" || upper == "INT4" || upper == "SMALLINT" ||
               upper == "INT2" || upper == "SERIAL" || upper == "BIGSERIAL" ||
               upper == "OID") {
        return common::LogicalType(common::LogicalTypeID::INT64);
    } else if (upper == "REAL" || upper == "FLOAT4") {
        return common::LogicalType(common::LogicalTypeID::FLOAT);
    } else if (upper == "DOUBLE PRECISION" || upper == "FLOAT8" || upper == "NUMERIC" ||
               upper == "DECIMAL") {
        return common::LogicalType(common::LogicalTypeID::DOUBLE);
    } else if (upper == "TEXT" || upper == "VARCHAR" || upper == "CHARACTER VARYING" ||
               upper == "CHAR" || upper == "CHARACTER" || upper == "BPCHAR" ||
               upper == "NAME" || upper == "XML" || upper == "JSON" || upper == "JSONB") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else if (upper == "BYTEA" || upper == "BLOB") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else if (upper == "DATE") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else if (upper == "TIMESTAMP" || upper == "TIMESTAMP WITHOUT TIME ZONE" ||
               upper == "TIMESTAMP WITH TIME ZONE" || upper == "TIMESTAMPTZ") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else if (upper == "INTERVAL") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else if (upper == "UUID") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else if (upper == "BOOLEAN[]" || upper == "INTEGER[]" || upper == "TEXT[]" ||
               upper == "VARCHAR[]") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else {
        // Default to string for unknown types
        return common::LogicalType(common::LogicalTypeID::STRING);
    }
}

void PgClientCatalog::createForeignNodeTable(const std::string& tableName) {
    // Get column info from information_schema.columns
    auto columnInfo = getTableColumnInfoFromConnector(connector, catalogName,
        defaultSchemaName, tableName);

    if (columnInfo.empty()) {
        return;
    }

    // Build property definitions
    std::vector<binder::PropertyDefinition> propertyDefs;
    std::string pkName = columnInfo[0].name;
    for (auto& col : columnInfo) {
        propertyDefs.emplace_back(
            binder::ColumnDefinition{col.name, col.type.copy()});
    }

    // Create the PgClientTableScanInfo for this table
    auto columnNames = getColumnNames(columnInfo);
    auto columnTypes = getColumnTypes(columnInfo);

    auto query = std::format("SELECT {{}} FROM \"{}\".\"{}\"",
        defaultSchemaName, tableName);

    auto scanInfo = std::make_shared<PgClientTableScanInfo>(query,
        std::move(columnTypes), std::move(columnNames), connector);

    // Create the scan function
    auto scanFunction = getScanFunction(scanInfo);

    // Create the foreign table catalog entry in the attached catalog
    // Save a raw pointer before moving it into the catalog set
    auto foreignTableEntry = std::make_unique<catalog::PgClientTableCatalogEntry>(
        tableName, std::move(scanFunction), scanInfo);

    for (auto& def : propertyDefs) {
        foreignTableEntry->addProperty(def);
    }

    auto* attachedEntryPtr = foreignTableEntry.get();
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(foreignTableEntry));

    // Create a main catalog entry for node table support (in-place queries)
    //
    // The shadow entry's foreignDatabaseName must be the lbug attached-database
    // name -- NOT the schema-qualified PG name ("public.node_person") -- because
    // the join-push-down optimizer uses it as a lookup key into
    // DatabaseManager::getAttachedDatabase().
    auto foreignDatabaseName = attachedDbName;
    auto mainTableEntry = std::make_unique<catalog::NodeTableCatalogEntry>(
        tableName, pkName, foreignDatabaseName, catalog::ShadowTag{});

    for (auto& def : propertyDefs) {
        mainTableEntry->addProperty(def);
    }

    // Link the shadow entry to the foreign entry so planners can find the scan function
    mainTableEntry->setReferencedEntry(attachedEntryPtr);

    context_->getDatabase()->getCatalog()->addTableEntry(std::move(mainTableEntry));

    // Register in the storage manager so the query planner can find this table
    auto* mainEntry = context_->getDatabase()->getCatalog()->getTableCatalogEntry(
        &transaction::DUMMY_TRANSACTION, tableName);
    if (mainEntry) {
        storage::StorageManager::Get(*context_)->createTable(mainEntry);
    }
}

void PgClientCatalog::createForeignRelTable(const std::string& tableName) {
    // Query foreign key info to find src/dst node tables
    auto fkQuery = std::format(
        "SELECT kcu.column_name, ccu.table_name "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "  ON tc.constraint_name = kcu.constraint_name "
        "  AND tc.table_schema = kcu.table_schema "
        "JOIN information_schema.constraint_column_usage ccu "
        "  ON ccu.constraint_name = tc.constraint_name "
        "  AND ccu.table_schema = tc.table_schema "
        "WHERE tc.constraint_type = 'FOREIGN KEY' "
        "  AND tc.table_schema = '{}' "
        "  AND tc.table_name = '{}'",
        defaultSchemaName, tableName);
    auto fkResult = connector.executeQuery(fkQuery);

    std::string srcTableName, dstTableName;
    for (auto& row : fkResult.rows) {
        auto colName = row.cells[0].value;
        auto refTable = row.cells[1].value;
        auto lowerCol = colName;
        common::StringUtils::toLower(lowerCol);
        if (lowerCol.rfind("src", 0) == 0 || lowerCol.rfind("from", 0) == 0) {
            srcTableName = refTable;
        } else if (lowerCol.rfind("dst", 0) == 0 || lowerCol.rfind("dest", 0) == 0 ||
                   lowerCol.rfind("to", 0) == 0) {
            dstTableName = refTable;
        }
    }

    if (srcTableName.empty() || dstTableName.empty()) {
        // No FK info found — register as a plain foreign table instead
        createForeignNodeTable(tableName);
        return;
    }

    // Get column info
    auto columnInfo = getTableColumnInfoFromConnector(connector, catalogName,
        defaultSchemaName, tableName);

    if (columnInfo.empty()) {
        return;
    }

    // Build property definitions
    std::vector<binder::PropertyDefinition> propertyDefs;
    for (auto& col : columnInfo) {
        propertyDefs.emplace_back(
            binder::ColumnDefinition{col.name, col.type.copy()});
    }

    auto columnNames = getColumnNames(columnInfo);
    auto columnTypes = getColumnTypes(columnInfo);

    // Look up src/dst node tables in the attached catalog (the foreign PgClientTableCatalogEntry
    // entries), not the main catalog shadows. rel_ tables join against the foreign node
    // entries directly, so the rel's src/dst table IDs must match the entries that
    // `testdb.node_person` (and bare `node_person` via shadow) resolve to.
    auto* srcEntry = tables->getEntry(&transaction::DUMMY_TRANSACTION, srcTableName);
    auto* dstEntry = tables->getEntry(&transaction::DUMMY_TRANSACTION, dstTableName);
    if (srcEntry == nullptr || dstEntry == nullptr) {
        createForeignNodeTable(tableName);
        return;
    }

    common::table_id_t srcTableID = srcEntry->cast<catalog::TableCatalogEntry>().getTableID();
    common::table_id_t dstTableID = dstEntry->cast<catalog::TableCatalogEntry>().getTableID();

    // Build scan function
    auto query = std::format("SELECT {{}} FROM \"{}\".\"{}\"",
        defaultSchemaName, tableName);

    auto scanInfo = std::make_shared<PgClientTableScanInfo>(query,
        std::move(columnTypes), std::move(columnNames), connector);
    auto scanFunc = getScanFunction(scanInfo);

    // Create foreign table entry in attached catalog
    auto foreignTableEntry = std::make_unique<catalog::PgClientTableCatalogEntry>(
        tableName, scanFunc, scanInfo);
    for (auto& def : propertyDefs) {
        foreignTableEntry->addProperty(def);
    }
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(foreignTableEntry));

    // Create bind data for the scan function (empty columns — populated at query time)
    binder::expression_vector emptyColumns;
    auto bindData = std::make_shared<PgClientScanBindData>(query, columnNames,
        connector, emptyColumns);

    // Create RelGroupCatalogEntry in the main catalog so MATCH ... -[...]-> ...
    // queries can find the rel table. ForeignRelTable (created via
    // StorageManager::createTable) is responsible for executing the scan using
    // the scan function / bind data; the connector is mutex-guarded so the
    // underlying libpq connection is safe across parallel hash-join workers,
    // and ForeignRelTable now owns the shared state and serializes offset
    // advancement with its own mutex, matching the morsel-driven model.
    // The rel group's foreignDatabaseName must be the lbug attached-database
    // name -- NOT the schema-qualified PG name ("public.rel_knows") -- because
    // the join-push-down optimizer uses it as a lookup key into
    // DatabaseManager::getAttachedDatabase().
    auto foreignDatabaseName = attachedDbName;

    std::vector<catalog::RelTableCatalogInfo> relTableInfos;
    common::oid_t relOID = tables->getNextOID();
    relTableInfos.emplace_back(
        catalog::NodeTableIDPair{srcTableID, dstTableID}, relOID,
        common::RelMultiplicity::MANY, common::RelMultiplicity::MANY);

    // Set relStorage to "dbname.tablename" so the foreign join push-down optimizer
    // can parse it and construct the proper SQL table reference.
    auto relStorage = attachedDbName + "." + tableName;

    auto relGroupEntry =
        std::make_unique<catalog::RelGroupCatalogEntry>(tableName,
            common::RelMultiplicity::MANY, common::RelMultiplicity::MANY,
            common::ExtendDirection::BOTH, std::move(relTableInfos),
            relStorage,
            common::StorageFormat::NONE, scanFunc, bindData,
            std::move(foreignDatabaseName));

    for (auto& def : propertyDefs) {
        relGroupEntry->addProperty(def);
    }

    context_->getDatabase()->getCatalog()->addTableEntry(std::move(relGroupEntry));

    // Set up storage for the rel table so the planner can find it. This
    // instantiates a ForeignRelTable, which lazily creates the scan function's
    // shared state on the first initScanState and shares it across all
    // worker threads (morsel-driven parallelism).
    auto mainEntry = context_->getDatabase()->getCatalog()->getTableCatalogEntry(
        &transaction::DUMMY_TRANSACTION, tableName);
    if (mainEntry) {
        storage::StorageManager::Get(*context_)->createTable(mainEntry);
    }
}

std::vector<PgClientColumnInfo> PgClientCatalog::getTableColumnInfo(
    const std::string& tableName) const {
    return getTableColumnInfoFromConnector(connector, catalogName,
        defaultSchemaName, tableName);
}

std::vector<std::string> PgClientCatalog::getColumnNames(
    const std::vector<PgClientColumnInfo>& columns) const {
    std::vector<std::string> names;
    names.reserve(columns.size());
    for (auto& col : columns) {
        names.push_back(col.name);
    }
    return names;
}

std::vector<common::LogicalType> PgClientCatalog::getColumnTypes(
    const std::vector<PgClientColumnInfo>& columns) const {
    std::vector<common::LogicalType> types;
    types.reserve(columns.size());
    for (auto& col : columns) {
        types.push_back(col.type.copy());
    }
    return types;
}

} // namespace pg_client_extension
} // namespace lbug
