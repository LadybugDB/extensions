#include "connector/iceberg_connector.h"

#include "common/exception/runtime.h"
#include "options/iceberg_options.h"

namespace lbug {
namespace iceberg_extension {

void IcebergConnector::connect(const std::string& /*dbPath*/, const std::string& /*catalogName*/,
    const std::string& /*schemaName*/, main::ClientContext* context) {
    auto config = IcebergOptions::getRestCatalogConfig(context);
    if (!config.restCatalogConfigured() && config.hasAnyOption()) {
        throw common::RuntimeException{std::format(
            "Iceberg REST catalog options were set but '{}' is empty. Set '{}' to the warehouse "
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
    // If the Iceberg REST catalog is configured, attach it inside the embedded
    // DuckDB instance, so iceberg tables can be referenced by fully qualified
    // name (e.g. iceberg_catalog.default.events) instead of a filesystem path.
    // This avoids the version-hint.text dependency of filesystem-based catalogs.
    if (!config.restCatalogConfigured()) {
        return;
    }
    if (config.hasAuth()) {
        executeQuery(IcebergSecretManager::getSecret(config));
    }
    executeQuery(IcebergSecretManager::getAttachQuery(config));
}

} // namespace iceberg_extension
} // namespace lbug
