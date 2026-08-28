#pragma once

#include <string>

#include "common/types/value/value.h"

namespace lbug {
namespace main {
class Database;
class ClientContext;
} // namespace main

namespace iceberg_extension {

// Connection parameters for DuckDB's Iceberg REST catalog support. When
// `iceberg_warehouse` is set, the embedded DuckDB instance used by the iceberg
// table functions attaches the Iceberg REST catalog, so tables can be
// referenced by fully qualified name (e.g. `iceberg_catalog.default.events`)
// instead of a filesystem path. This removes the dependency on
// `metadata/version-hint.text`, which is only produced by filesystem-based
// catalogs (HadoopCatalog/HadoopTables).
struct IcebergWarehouse {
    static constexpr const char* NAME = "iceberg_warehouse";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergEndpoint {
    static constexpr const char* NAME = "iceberg_endpoint";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergEndpointType {
    static constexpr const char* NAME = "iceberg_endpoint_type";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergAuthorizationType {
    static constexpr const char* NAME = "iceberg_authorization_type";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergOauth2ServerUri {
    static constexpr const char* NAME = "iceberg_oauth2_server_uri";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergClientId {
    static constexpr const char* NAME = "iceberg_client_id";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergToken {
    static constexpr const char* NAME = "iceberg_token";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergClientSecret {
    static constexpr const char* NAME = "iceberg_client_secret";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

struct IcebergRestCatalogConfig {
    std::string warehouse;
    std::string endpoint;
    std::string endpointType;
    std::string authorizationType;
    std::string oauth2ServerUri;
    std::string clientId;
    std::string token;
    std::string clientSecret;

    // The warehouse identifier is the trigger for REST catalog mode: it is the
    // path argument of DuckDB's `ATTACH ... (TYPE ICEBERG)` statement.
    bool restCatalogConfigured() const { return !warehouse.empty(); }
    bool hasAnyOption() const {
        return !warehouse.empty() || !endpoint.empty() || !endpointType.empty() ||
               !authorizationType.empty() || !oauth2ServerUri.empty() || !clientId.empty() ||
               !token.empty() || !clientSecret.empty();
    }
    // Whether the attached catalog needs an Iceberg secret for authentication.
    bool hasAuth() const {
        return !token.empty() || !oauth2ServerUri.empty() || !clientId.empty() ||
               !clientSecret.empty();
    }
};

struct IcebergOptions {
    static void registerExtensionOptions(main::Database* db);
    static void setEnvValue(main::ClientContext* context);
    static IcebergRestCatalogConfig getRestCatalogConfig(main::ClientContext* context);
};

// Translates the Ladybug extension options into `CREATE SECRET (TYPE ICEBERG, ...)`
// and `ATTACH ... (TYPE ICEBERG, ...)` statements executed on the embedded
// DuckDB instance.
struct IcebergSecretManager {
    static constexpr const char* CATALOG_ALIAS = "iceberg_catalog";
    static constexpr const char* SECRET_NAME = "iceberg_rest_secret";

    static std::string getSecret(const IcebergRestCatalogConfig& config);
    static std::string getAttachQuery(const IcebergRestCatalogConfig& config);
};

} // namespace iceberg_extension
} // namespace lbug
