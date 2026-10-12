#include "storage/sqlite_storage.h"

#include <filesystem>

#include "catalog/sqlite_catalog.h"
#include "common/string_utils.h"
#include "connector/sqlite_connector.h"
#include "extension/extension.h"
#include "storage/attached_sqlite_database.h"

namespace lbug {
namespace sqlite_extension {

static std::string getCatalogNameFromPath(const std::string& dbPath) {
    std::filesystem::path path(dbPath);
    return path.stem().string();
}

std::unique_ptr<main::AttachedDatabase> attachSqlite(std::string dbName, std::string dbPath,
    main::ClientContext* clientContext, const binder::AttachOption& attachOption) {
    (void)attachOption;
    auto catalogName = getCatalogNameFromPath(dbPath);
    if (dbName == "") {
        dbName = catalogName;
    }
    // Connect directly against the SQLite file with the native C API. The
    // file is opened read-only: ladybug never writes to it.
    auto connector = std::make_unique<SqliteConnector>();
    connector->connect(dbPath);
    auto catalog = std::make_unique<SqliteCatalog>(dbPath, dbName, clientContext, *connector);
    catalog->init();
    return std::make_unique<AttachedSqliteDatabase>(dbName, SqliteStorageExtension::DB_TYPE,
        std::move(catalog), std::move(connector));
}

SqliteStorageExtension::SqliteStorageExtension(main::Database& database)
    : StorageExtension{attachSqlite} {
    (void)database;
}

bool SqliteStorageExtension::canHandleDB(std::string dbType_) const {
    common::StringUtils::toUpper(dbType_);
    return dbType_ == DB_TYPE;
}

} // namespace sqlite_extension
} // namespace lbug
