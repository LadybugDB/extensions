#include "connector/iceberg_connector.h"

#include "common/exception/runtime.h"
#include "options/iceberg_options.h"

namespace lbug {
namespace iceberg_extension {

void IcebergConnector::connect(const std::string& dbPath, const std::string& catalogName,
    const std::string& /*schemaName*/, main::ClientContext* context) {
    auto config = IcebergOptions::getRestCatalogConfig(context);
    // ATTACH '<warehouse>' AS <alias> (DBTYPE ICEBERG) passes the warehouse and
    // the attached alias through; the LOAD FROM table-function path calls
    // connect() with empty arguments and relies on the global options instead.
    const bool attaching = !dbPath.empty() || !catalogName.empty();
    if (!dbPath.empty()) {
        config.warehouse = dbPath;
    }
    const auto catalogAlias =
        catalogName.empty() ? IcebergSecretManager::CATALOG_ALIAS : catalogName;
    if (config.warehouse.empty()) {
        if (attaching) {
            throw common::RuntimeException{
                "Cannot attach an Iceberg catalog without a warehouse. Give the warehouse "
                "identifier as the ATTACH path (ATTACH '<warehouse>' AS <alias> (DBTYPE "
                "ICEBERG)) or set the 'iceberg_warehouse' option."};
        }
        if (config.hasAnyOption()) {
            throw common::RuntimeException{std::format(
                "Iceberg REST catalog options were set but '{}' is empty. Set '{}' to the "
                "warehouse "
                "identifier to enable Iceberg REST catalog access.",
                IcebergWarehouse::NAME, IcebergWarehouse::NAME)};
        }
        // Creates an in-memory duckdb instance, then install iceberg and httpfs.
        instance = std::make_unique<duckdb::DuckDB>(nullptr);
        connection = std::make_unique<duckdb::Connection>(*instance);
        // Install the Desired Extension on DuckDB
        executeQuery("install iceberg;");
        executeQuery("load iceberg;");
        executeQuery("install httpfs;");
        executeQuery("load httpfs;");
        initRemoteFSSecrets(context);
        return;
    }
    if (attaching && config.endpoint.empty()) {
        throw common::RuntimeException{
            "Cannot attach an Iceberg REST catalog without an endpoint. Set the "
            "'iceberg_endpoint' option to the REST catalog URL before attaching."};
    }
    // Creates an in-memory duckdb instance, then install iceberg and httpfs.
    instance = std::make_unique<duckdb::DuckDB>(nullptr);
    connection = std::make_unique<duckdb::Connection>(*instance);
    // Install the Desired Extension on DuckDB
    executeQuery("install iceberg;");
    executeQuery("load iceberg;");
    executeQuery("install httpfs;");
    executeQuery("load httpfs;");
    initRemoteFSSecrets(context);
    // If the Iceberg REST catalog is configured, attach it inside the embedded
    // DuckDB instance, so iceberg tables can be referenced by fully qualified
    // name (e.g. iceberg_catalog.default.events) instead of a filesystem path.
    // This avoids the version-hint.text dependency of filesystem-based catalogs.
    if (config.hasAuth()) {
        executeQuery(IcebergSecretManager::getSecret(config));
    }
    executeQuery(IcebergSecretManager::getAttachQuery(config, catalogAlias));
}

} // namespace iceberg_extension
} // namespace lbug
