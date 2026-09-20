#include "options/ducklake_options.h"

#include "common/string_utils.h"
#include "extension/extension.h"
#include "main/client_context.h"
#include "main/database.h"
#include <format>

namespace lbug {
namespace ducklake_extension {

using namespace common;

namespace {

std::string escapeSingleQuotes(std::string value) {
    StringUtils::replaceAll(value, "'", "''");
    return value;
}

// Reads an option value from the environment. Both the option name as-is and
// its upper-case form are accepted (e.g. `ducklake_data_path` or
// `DUCKLAKE_DATA_PATH`).
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

} // namespace

void DuckLakeOptions::registerExtensionOptions(main::Database* db) {
    ADD_EXTENSION_OPTION(DuckLakeDataPath);
    ADD_EXTENSION_OPTION(DuckLakeReadOnly);
}

void DuckLakeOptions::setEnvValue(main::ClientContext* context) {
    setEnvOption(context, DuckLakeDataPath::NAME);
    auto readOnly = getEnvOption(DuckLakeReadOnly::NAME);
    if (!readOnly.empty()) {
        StringUtils::toLower(readOnly);
        context->setExtensionOption(DuckLakeReadOnly::NAME,
            Value::createValue(readOnly == "true" || readOnly == "1"));
    }
}

std::string DuckLakeOptions::getDataPath(main::ClientContext* context) {
    return context->getCurrentSetting(DuckLakeDataPath::NAME).toString();
}

bool DuckLakeOptions::getReadOnly(main::ClientContext* context) {
    auto value = context->getCurrentSetting(DuckLakeReadOnly::NAME);
    return value.getDataType().getLogicalTypeID() == LogicalTypeID::BOOL && value.getValue<bool>();
}

std::string DuckLakeAttachHelper::getAttachQuery(const std::string& dbPath,
    const std::string& catalogAlias, main::ClientContext* context) {
    // Normalize the ATTACH target. An empty path means "read the
    // configuration from the default (unnamed) TYPE ducklake secret"
    // (`ATTACH 'ducklake:'`). A path that already carries the `ducklake:`
    // scheme (e.g. `ducklake:postgres:dbname=...`) is used as-is; anything
    // else is treated as the metadata catalog path and prefixed.
    std::string target;
    if (dbPath.empty()) {
        target = "ducklake:";
    } else if (dbPath.rfind("ducklake:", 0) == 0) {
        target = dbPath;
    } else {
        target = "ducklake:" + dbPath;
    }
    std::string options;
    auto dataPath = DuckLakeOptions::getDataPath(context);
    if (!dataPath.empty()) {
        // OVERRIDE_DATA_PATH is required: DuckDB rejects a DATA_PATH that
        // differs from the catalog's stored path without it.
        options +=
            std::format(", DATA_PATH '{}', OVERRIDE_DATA_PATH true", escapeSingleQuotes(dataPath));
    }
    if (DuckLakeOptions::getReadOnly(context)) {
        options += ", READ_ONLY";
    }
    // Strip the leading ", " when options are present.
    auto params = options.empty() ? std::string{} : " (" + options.substr(2) + ")";
    return std::format("ATTACH '{}' AS {}{};", escapeSingleQuotes(target), catalogAlias, params);
}

} // namespace ducklake_extension
} // namespace lbug
