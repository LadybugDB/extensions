#include "storage/pg_client_storage.h"

#include <regex>

#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "connector/pg_client_connector.h"
#include "extension/extension.h"
#include "catalog/pg_client_catalog.h"
#include "function/pg_client_scan.h"
#include "storage/attached_pg_client_database.h"

namespace lbug {
namespace pg_client_extension {

std::string extractDBName(const std::string& connectionInfo) {
    std::regex pattern("dbname=([^ ]+)");
    std::smatch match;
    if (std::regex_search(connectionInfo, match, pattern)) {
        return match.str(1);
    }
    // Try PostgreSQL URI format
    std::regex uriPattern("postgres(?:ql)?://[^/]+/([^?]+)");
    if (std::regex_search(connectionInfo, match, uriPattern)) {
        return match.str(1);
    }
    throw common::RuntimeException{"Invalid PostgreSQL connection string."};
}

std::unique_ptr<main::AttachedDatabase> attachPgClient(std::string dbName, std::string dbPath,
    main::ClientContext* clientContext, const binder::AttachOption& attachOption) {
    auto catalogName = extractDBName(dbPath);
    if (dbName == "") {
        dbName = catalogName;
    }

    // Connect to PostgreSQL via libpq
    auto connector = std::make_unique<PgClientConnector>();
    connector->connect(dbPath);

    // Create catalog that discovers tables from PG information_schema
    auto schemaName = PgClientCatalog::bindSchemaName(attachOption,
        PgClientStorageExtension::DEFAULT_SCHEMA_NAME);
    auto catalog = std::make_unique<PgClientCatalog>(dbPath, catalogName,
        schemaName, dbName, clientContext, *connector);
    catalog->init();

    return std::make_unique<AttachedPgClientDatabase>(dbName,
        PgClientStorageExtension::DB_TYPE, std::move(catalog), std::move(connector),
        schemaName);
}

PgClientStorageExtension::PgClientStorageExtension(main::Database& /*database*/)
    : StorageExtension{attachPgClient} {}

bool PgClientStorageExtension::canHandleDB(std::string dbType_) const {
    common::StringUtils::toUpper(dbType_);
    return dbType_ == DB_TYPE;
}

} // namespace pg_client_extension
} // namespace lbug
