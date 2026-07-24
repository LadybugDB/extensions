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
    std::string defaultSchemaName, main::ClientContext* context,
    const PgClientConnector& connector)
    : CatalogExtension{}, connStr{std::move(connStr)}, catalogName{std::move(catalogName)},
      defaultSchemaName{std::move(defaultSchemaName)}, connector{connector},
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
        if (tableName.starts_with("node_")) {
            createForeignNodeTable(tableName, {}, tableName.substr(5));
        } else if (tableName.starts_with("rel_")) {
            createForeignRelTable(tableName, {}, "", "");
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
        return common::LogicalType(common::LogicalTypeID::BLOB);
    } else if (upper == "DATE") {
        return common::LogicalType(common::LogicalTypeID::DATE);
    } else if (upper == "TIMESTAMP" || upper == "TIMESTAMP WITHOUT TIME ZONE" ||
               upper == "TIMESTAMP WITH TIME ZONE" || upper == "TIMESTAMPTZ") {
        return common::LogicalType(common::LogicalTypeID::TIMESTAMP);
    } else if (upper == "INTERVAL") {
        return common::LogicalType(common::LogicalTypeID::INTERVAL);
    } else if (upper == "UUID") {
        return common::LogicalType(common::LogicalTypeID::UUID);
    } else if (upper == "BOOLEAN[]" || upper == "INTEGER[]" || upper == "TEXT[]" ||
               upper == "VARCHAR[]") {
        return common::LogicalType(common::LogicalTypeID::STRING);
    } else {
        // Default to string for unknown types
        return common::LogicalType(common::LogicalTypeID::STRING);
    }
}

void PgClientCatalog::createForeignNodeTable(const std::string& tableName,
    const std::vector<binder::PropertyDefinition>&,
    const std::string& primaryKey) {
    // Get column info from information_schema.columns
    auto columnInfo = getTableColumnInfoFromConnector(connector, catalogName,
        defaultSchemaName, tableName);

    if (columnInfo.empty()) {
        return;
    }

    // Build property definitions
    std::vector<binder::PropertyDefinition> propertyDefs;
    std::string pkName = primaryKey.empty() ? columnInfo[0].name : primaryKey;
    for (auto& col : columnInfo) {
        propertyDefs.emplace_back(
            binder::ColumnDefinition{col.name, col.type.copy()});
    }

    // Create the PgClientTableScanInfo for this table
    auto columnNames = getColumnNames(columnInfo);
    auto columnTypes = getColumnTypes(columnInfo);

    auto query = std::format("SELECT {{}} FROM \"{}\".\"{}\"",
        catalogName, tableName);

    auto scanInfo = std::make_shared<PgClientTableScanInfo>(query,
        std::move(columnTypes), std::move(columnNames), connector);

    // Create the scan function
    auto scanFunction = getScanFunction(scanInfo);

    // Create the foreign table catalog entry
    auto foreignTableEntry = std::make_unique<catalog::PgClientTableCatalogEntry>(
        tableName, std::move(scanFunction), scanInfo);

    for (auto& def : propertyDefs) {
        foreignTableEntry->addProperty(def);
    }

    // Register in our catalog
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(foreignTableEntry));

    // Create a main catalog entry for node table support
    auto foreignDatabaseName = std::format("{}.{}", catalogName, tableName);
    auto mainTableEntry = std::make_unique<catalog::NodeTableCatalogEntry>(
        tableName, pkName, foreignDatabaseName, catalog::ShadowTag{});

    for (auto& def : propertyDefs) {
        mainTableEntry->addProperty(def);
    }

    context_->getDatabase()->getCatalog()->addTableEntry(std::move(mainTableEntry));
    auto mainEntry = context_->getDatabase()->getCatalog()->getTableCatalogEntry(
        &transaction::DUMMY_TRANSACTION, tableName);
    (void)mainEntry;
}

void PgClientCatalog::createForeignRelTable(const std::string& tableName,
    const std::vector<binder::PropertyDefinition>&,
    const std::string&, const std::string&) {
    // Get column info from information_schema.columns
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

    // Create the PgClientTableScanInfo for this table
    auto columnNames = getColumnNames(columnInfo);
    auto columnTypes = getColumnTypes(columnInfo);

    auto query = std::format("SELECT {{}} FROM \"{}\".\"{}\"",
        catalogName, tableName);

    auto scanInfo = std::make_shared<PgClientTableScanInfo>(query,
        std::move(columnTypes), std::move(columnNames), connector);

    auto scanFunction = getScanFunction(scanInfo);

    // Create the foreign table catalog entry
    auto foreignTableEntry = std::make_unique<catalog::PgClientTableCatalogEntry>(
        tableName, std::move(scanFunction), scanInfo);

    for (auto& def : propertyDefs) {
        foreignTableEntry->addProperty(def);
    }

    // Register in our catalog
    tables->createEntry(&transaction::DUMMY_TRANSACTION, std::move(foreignTableEntry));
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
