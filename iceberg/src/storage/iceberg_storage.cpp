#include "storage/iceberg_storage.h"

#include "catalog/duckdb_catalog.h"
#include "common/string_utils.h"
#include "connector/iceberg_connector.h"
#include "extension/extension.h"
#include "function/clear_cache.h"
#include "storage/attached_duckdb_database.h"

namespace lbug {
namespace iceberg_extension {

std::unique_ptr<main::AttachedDatabase> attachIceberg(std::string dbName, std::string dbPath,
    main::ClientContext* clientContext, const binder::AttachOption& attachOption) {
    if (dbName == "") {
        dbName = dbPath;
    }
    auto schemaName = duckdb_extension::DuckDBCatalog::bindSchemaName(attachOption,
        IcebergStorageExtension::DEFAULT_SCHEMA_NAME);
    auto connector = std::make_unique<IcebergConnector>();
    // The DuckDB-side catalog is attached AS dbName, so DuckDBCatalog must use
    // dbName (not dbPath) as its catalog name: information_schema lookups and
    // generated SQL reference the attached alias. The catalog is constructed
    // before connecting so that ATTACH option validation happens before any
    // network I/O.
    auto catalog = std::make_unique<duckdb_extension::DuckDBCatalog>(dbPath, dbName, schemaName,
        clientContext, *connector, attachOption, dbName);
    connector->connect(dbPath, dbName, schemaName, clientContext);
    catalog->init();
    return std::make_unique<duckdb_extension::AttachedDuckDBDatabase>(dbName,
        IcebergStorageExtension::DB_TYPE, std::move(catalog), std::move(connector));
}

IcebergStorageExtension::IcebergStorageExtension(main::Database& database)
    : StorageExtension{attachIceberg} {
    extension::ExtensionUtils::addStandaloneTableFunc<duckdb_extension::ClearCacheFunction>(
        database);
}

bool IcebergStorageExtension::canHandleDB(std::string dbType_) const {
    common::StringUtils::toUpper(dbType_);
    return dbType_ == DB_TYPE;
}

} // namespace iceberg_extension
} // namespace lbug
