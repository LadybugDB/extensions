#include "options/iceberg_options.h"

#include "common/string_utils.h"
#include "extension/extension.h"
#include "main/client_context.h"
#include "main/database.h"
#include <format>

namespace lbug {
namespace iceberg_extension {

using namespace common;

namespace {

std::string escapeSingleQuotes(std::string value) {
    StringUtils::replaceAll(value, "'", "''");
    return value;
}

// Reads an option value from the environment. Both the option name as-is and
// its upper-case form are accepted (e.g. `iceberg_token` or `ICEBERG_TOKEN`).
std::string getEnvOption(const char* name) {
    auto value = main::ClientContext::getEnvVariable(name);
    if (value.empty()) {
        value = main::ClientContext::getEnvVariable(StringUtils::getUpper(std::string(name)));
    }
    return value;
}

void setEnvOption(main::ClientContext* context, const char* name) {
    auto value = getEnvOption(name);
    if (!value.empty()) {
        context->setExtensionOption(name, Value::createValue(value));
    }
}

std::string getSetting(main::ClientContext* context, const char* name) {
    return context->getCurrentSetting(name).toString();
}

} // namespace

void IcebergOptions::registerExtensionOptions(main::Database* db) {
    ADD_EXTENSION_OPTION(IcebergWarehouse);
    ADD_EXTENSION_OPTION(IcebergEndpoint);
    ADD_EXTENSION_OPTION(IcebergEndpointType);
    ADD_EXTENSION_OPTION(IcebergAuthorizationType);
    ADD_EXTENSION_OPTION(IcebergOauth2ServerUri);
    ADD_EXTENSION_OPTION(IcebergClientId);
    ADD_CONFIDENTIAL_EXTENSION_OPTION(IcebergToken);
    ADD_CONFIDENTIAL_EXTENSION_OPTION(IcebergClientSecret);
}

void IcebergOptions::setEnvValue(main::ClientContext* context) {
    setEnvOption(context, IcebergWarehouse::NAME);
    setEnvOption(context, IcebergEndpoint::NAME);
    setEnvOption(context, IcebergEndpointType::NAME);
    setEnvOption(context, IcebergAuthorizationType::NAME);
    setEnvOption(context, IcebergOauth2ServerUri::NAME);
    setEnvOption(context, IcebergClientId::NAME);
    setEnvOption(context, IcebergToken::NAME);
    setEnvOption(context, IcebergClientSecret::NAME);
}

IcebergRestCatalogConfig IcebergOptions::getRestCatalogConfig(main::ClientContext* context) {
    IcebergRestCatalogConfig config;
    config.warehouse = getSetting(context, IcebergWarehouse::NAME);
    config.endpoint = getSetting(context, IcebergEndpoint::NAME);
    config.endpointType = getSetting(context, IcebergEndpointType::NAME);
    config.authorizationType = getSetting(context, IcebergAuthorizationType::NAME);
    config.oauth2ServerUri = getSetting(context, IcebergOauth2ServerUri::NAME);
    config.clientId = getSetting(context, IcebergClientId::NAME);
    config.token = getSetting(context, IcebergToken::NAME);
    config.clientSecret = getSetting(context, IcebergClientSecret::NAME);
    return config;
}

std::string IcebergSecretManager::getSecret(const IcebergRestCatalogConfig& config) {
    std::string options;
    auto appendOption = [&options](const std::string& name, const std::string& value) {
        options += std::format("{} '{}',", name, escapeSingleQuotes(value));
    };
    if (!config.token.empty()) {
        appendOption("TOKEN", config.token);
    }
    if (!config.oauth2ServerUri.empty()) {
        appendOption("OAUTH2_SERVER_URI", config.oauth2ServerUri);
    }
    if (!config.clientId.empty()) {
        appendOption("CLIENT_ID", config.clientId);
    }
    if (!config.clientSecret.empty()) {
        appendOption("CLIENT_SECRET", config.clientSecret);
    }
    return std::format("CREATE SECRET {} ({} TYPE ICEBERG);", SECRET_NAME, options);
}

std::string IcebergSecretManager::getAttachQuery(const IcebergRestCatalogConfig& config) {
    std::string options = "TYPE ICEBERG";
    if (config.hasAuth()) {
        options += std::format(", SECRET {}", SECRET_NAME);
    }
    if (!config.endpoint.empty()) {
        options += std::format(", ENDPOINT '{}'", escapeSingleQuotes(config.endpoint));
    }
    if (!config.endpointType.empty()) {
        options += std::format(", ENDPOINT_TYPE '{}'", escapeSingleQuotes(config.endpointType));
    }
    if (!config.authorizationType.empty()) {
        options +=
            std::format(", AUTHORIZATION_TYPE '{}'", escapeSingleQuotes(config.authorizationType));
    }
    return std::format("ATTACH '{}' AS {} ({});", escapeSingleQuotes(config.warehouse),
        CATALOG_ALIAS, options);
}

} // namespace iceberg_extension
} // namespace lbug
