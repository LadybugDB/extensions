#include "connector/unity_catalog_connector.h"

#include "options/unity_catalog_options.h"
#include <format>

namespace lbug {
namespace unity_catalog_extension {

// Ensures a DuckDB extension is loaded. LOAD is attempted first so that an
// already-installed extension is reused as-is (no network, no repository
// origin check). INSTALL runs only as a fallback when the extension is
// missing, e.g. on a fresh machine.
static void ensureExtensionLoaded(const duckdb_extension::DuckDBConnector& connector,
    const std::string& extensionName) {
    try {
        connector.executeQuery(std::format("load {};", extensionName));
        return;
    } catch (const common::Exception&) {
        // Not installed yet: fall through to INSTALL.
    }
    connector.executeQuery(std::format("install {};", extensionName));
    connector.executeQuery(std::format("load {};", extensionName));
}

void UnityCatalogConnector::connect(const std::string& dbPath, const std::string& catalogName,
    const std::string& /*schemaName*/, main::ClientContext* context) {
    // Creates an in-memory duckdb instance, then install httpfs and attach postgres.
    instance = std::make_unique<duckdb::DuckDB>(nullptr);
    connection = std::make_unique<duckdb::Connection>(*instance);
    ensureExtensionLoaded(*this, "uc_catalog");
    ensureExtensionLoaded(*this, "delta");
    executeQuery(DuckDBUnityCatalogSecretManager::getSecret(context));
    executeQuery(
        std::format("attach '{}' as {} (TYPE UC_CATALOG, read_only);", dbPath, catalogName));
}

} // namespace unity_catalog_extension
} // namespace lbug
