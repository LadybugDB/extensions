#include "storage/adbc_storage.h"

#include "catalog/adbc_catalog.h"
#include "catalog/adbc_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "connector/adbc_connector.h"
#include "storage/attached_adbc_database.h"

namespace lbug {
namespace adbc_extension {

std::unique_ptr<main::AttachedDatabase> attachADBC(std::string dbName, std::string dbPath,
    main::ClientContext* clientContext, const binder::AttachOption& attachOption) {
    if (dbName.empty()) {
        dbName = "adbc";
    }
    std::string schemaName = ADBCStorageExtension::DEFAULT_SCHEMA_NAME;
    if (attachOption.options.contains("SCHEMA")) {
        auto val = attachOption.options.at("SCHEMA");
        if (val.getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING) {
            throw common::RuntimeException{"Invalid option value for SCHEMA"};
        }
        schemaName = val.getValue<std::string>();
    }
    // Optional catalog for 3-level namespaces (e.g. Databricks UC). Enables
    // the statement-probe schema discovery and qualifies scan SQL. Empty =
    // legacy behavior (bare table, driver-resolved namespace).
    std::string catalogName;
    if (attachOption.options.contains("CATALOG")) {
        auto val = attachOption.options.at("CATALOG");
        if (val.getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING) {
            throw common::RuntimeException{"Invalid option value for CATALOG"};
        }
        catalogName = val.getValue<std::string>();
    }
    auto connector = std::make_unique<ADBCConnector>(attachOption);
    connector->connect(dbPath);
    auto catalog =
        std::make_unique<ADBCCatalog>(catalogName, schemaName, clientContext, *connector);
    catalog->init();
    return std::make_unique<AttachedADBCDatabase>(dbName, ADBCStorageExtension::DB_TYPE,
        std::move(catalog), std::move(connector));
}

ADBCStorageExtension::ADBCStorageExtension(main::Database& /*database*/)
    : StorageExtension{attachADBC} {}

bool ADBCStorageExtension::canHandleDB(std::string dbType_) const {
    common::StringUtils::toUpper(dbType_);
    return dbType_ == DB_TYPE;
}

} // namespace adbc_extension
} // namespace lbug
