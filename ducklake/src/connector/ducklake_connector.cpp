#include "connector/ducklake_connector.h"

#include "options/ducklake_options.h"
#include <format>

namespace lbug {
namespace ducklake_extension {

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

void DuckLakeConnector::connect(const std::string& dbPath, const std::string& catalogName,
    const std::string& /*schemaName*/, main::ClientContext* context) {
    // Creates an in-memory DuckDB instance, loads the ducklake extension
    // (plus httpfs for remote DATA_PATHs such as s3://), then attaches the
    // DuckLake catalog inside the embedded instance so its tables can be
    // referenced by fully qualified name (e.g. my_lake.main.person).
    instance = std::make_unique<duckdb::DuckDB>(nullptr);
    connection = std::make_unique<duckdb::Connection>(*instance);
    ensureExtensionLoaded(*this, "ducklake");
    ensureExtensionLoaded(*this, "httpfs");
    executeQuery(DuckLakeAttachHelper::getAttachQuery(dbPath, catalogName, context));
}

} // namespace ducklake_extension
} // namespace lbug
