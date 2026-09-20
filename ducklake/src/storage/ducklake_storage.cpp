#include "storage/ducklake_storage.h"

#include "catalog/duckdb_catalog.h"
#include "common/string_utils.h"
#include "connector/ducklake_connector.h"
#include "extension/extension.h"
#include "function/clear_cache.h"
#include "storage/attached_duckdb_database.h"

namespace lbug {
namespace ducklake_extension {

std::unique_ptr<main::AttachedDatabase> attachDuckLake(std::string dbName, std::string dbPath,
    main::ClientContext* clientContext, const binder::AttachOption& attachOption) {
    if (dbName == "") {
        dbName = dbPath;
    }
    auto connector = std::make_unique<DuckLakeConnector>();
    // The DuckDB-side catalog is attached AS dbName, so DuckDBCatalog must use
    // dbName (not dbPath) as its catalog name: information_schema lookups and
    // generated SQL reference the attached alias. The catalog is constructed
    // before connecting so that ATTACH option validation happens before any
    // filesystem or network I/O.
    auto catalog = std::make_unique<duckdb_extension::DuckDBCatalog>(dbPath, dbName,
        DuckLakeStorageExtension::DEFAULT_SCHEMA_NAME, clientContext, *connector, attachOption,
        dbName);
    connector->connect(dbPath, dbName, DuckLakeStorageExtension::DEFAULT_SCHEMA_NAME,
        clientContext);
    catalog->init();
    return std::make_unique<duckdb_extension::AttachedDuckDBDatabase>(dbName,
        DuckLakeStorageExtension::DB_TYPE, std::move(catalog), std::move(connector));
}

DuckLakeStorageExtension::DuckLakeStorageExtension(main::Database& database)
    : StorageExtension{attachDuckLake} {
    extension::ExtensionUtils::addStandaloneTableFunc<duckdb_extension::ClearCacheFunction>(
        database);
}

bool DuckLakeStorageExtension::canHandleDB(std::string dbType_) const {
    common::StringUtils::toUpper(dbType_);
    return dbType_ == DB_TYPE;
}

} // namespace ducklake_extension
} // namespace lbug
