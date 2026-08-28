#include <cctype>

#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "connector/iceberg_connector.h"
#include "function/iceberg_functions.h"
#include "options/iceberg_options.h"
#include <format>

namespace lbug {
namespace iceberg_extension {

using namespace function;
using namespace common;

static std::string escapeSingleQuotes(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (auto c : value) {
        if (c == '\'') {
            result += "''";
        } else {
            result += c;
        }
    }
    return result;
}

static const optional_params_t& getScanOptions(const TableFuncBindInput* input,
    const std::string& functionName) {
    if (functionName == "ICEBERG_SCAN") {
        auto scanInput = input->extraInput->constPtrCast<ExtraScanTableFuncBindInput>();
        return scanInput->fileScanInfo.options;
    }
    return input->optionalParams;
}

static std::string generateQueryOptions(const TableFuncBindInput* input,
    const std::string& functionName) {
    std::string query_options = "";
    auto appendOptions = [&](const auto& options) {
        for (auto& [name, value] : options) {
            auto lowerCaseName = StringUtils::getLower(name);
            auto valueStr = value.toString();
            if (lowerCaseName == "allow_moved_paths") {
                // check data type of allow_moved_paths
                if (value.getDataType().getLogicalTypeID() != LogicalTypeID::BOOL) {
                    throw RuntimeException{
                        std::format("Invalid allow_moved_paths value for {}", valueStr)};
                }
                query_options += std::format(", {} = {}", lowerCaseName, valueStr);
            } else {
                query_options += std::format(", {} = '{}'", lowerCaseName, valueStr);
            }
        }
    };
    appendOptions(getScanOptions(input, functionName));

    return query_options;
}

// Returns true if the given argument is a catalog-qualified iceberg table name
// (e.g. 'iceberg_catalog.default.events' or 'default.events') rather than a
// filesystem path.
static bool isCatalogQualifiedTableName(const std::string& name) {
    auto parts = StringUtils::split(name, ".");
    if (parts.size() < 2 || parts.size() > 3) {
        return false;
    }
    for (auto& part : parts) {
        if (part.empty()) {
            return false;
        }
        auto firstChar = static_cast<unsigned char>(part[0]);
        if (!std::isalpha(firstChar) && part[0] != '_') {
            return false;
        }
        for (auto c : part) {
            auto ch = static_cast<unsigned char>(c);
            if (!std::isalnum(ch) && c != '_' && c != '$') {
                return false;
            }
        }
    }
    return true;
}

// The embedded DuckDB instance attaches the Iceberg REST catalog with a fixed
// alias (IcebergSecretManager::CATALOG_ALIAS). Two-part names are interpreted
// as namespace.table on that catalog.
static std::string resolveQualifiedTableName(const std::string& name) {
    auto parts = StringUtils::split(name, ".");
    if (parts.size() == 2) {
        return std::format("{}.{}", IcebergSecretManager::CATALOG_ALIAS, name);
    }
    if (parts[0] != IcebergSecretManager::CATALOG_ALIAS) {
        throw RuntimeException{std::format(
            "Unknown iceberg catalog '{}'. The attached Iceberg REST catalog is named '{}'. "
            "Use '{}.namespace.table' or 'namespace.table' to reference a REST catalog table.",
            parts[0], IcebergSecretManager::CATALOG_ALIAS, IcebergSecretManager::CATALOG_ALIAS)};
    }
    return name;
}

// Catalog tables resolve their current metadata through the REST catalog, so
// time travel is expressed with DuckDB's AT clause instead of scan options.
static std::string generateTimeTravelClause(const optional_params_t& options) {
    std::string clause;
    for (auto& [name, value] : options) {
        auto lowerCaseName = StringUtils::getLower(name);
        if (lowerCaseName == "snapshot_from_id") {
            if (!clause.empty()) {
                throw RuntimeException(
                    "snapshot_from_id and snapshot_from_timestamp are mutually exclusive.");
            }
            clause = std::format(" AT (VERSION => {})", value.toString());
        } else if (lowerCaseName == "snapshot_from_timestamp") {
            if (!clause.empty()) {
                throw RuntimeException(
                    "snapshot_from_id and snapshot_from_timestamp are mutually exclusive.");
            }
            clause = std::format(" AT (TIMESTAMP => TIMESTAMP '{}')",
                escapeSingleQuotes(value.toString()));
        } else {
            throw RuntimeException{std::format(
                "Option '{}' is not supported when scanning an Iceberg REST catalog table.", name)};
        }
    }
    return clause;
}

static std::string generateCatalogTableQuery(const TableFuncBindInput* input,
    const std::string& functionName, const std::string& source) {
    auto qualified = resolveQualifiedTableName(source);
    if (functionName == "ICEBERG_SCAN") {
        return std::format("SELECT * FROM {}{}", qualified,
            generateTimeTravelClause(getScanOptions(input, functionName)));
    }
    if (!getScanOptions(input, functionName).empty()) {
        throw RuntimeException{std::format(
            "Options are not supported for {} on an Iceberg REST catalog table.", functionName)};
    }
    if (functionName == "ICEBERG_METADATA") {
        return std::format("SELECT * FROM iceberg_metadata({})", qualified);
    }
    return std::format("SELECT * FROM iceberg_snapshots({})", qualified);
}

std::unique_ptr<TableFuncBindData> bindFuncHelper(main::ClientContext* context,
    const TableFuncBindInput* input, const std::string& functionName) {
    auto connector = std::make_shared<IcebergConnector>();
    connector->connect("" /* inMemDB */, "" /* defaultCatalogName */, "" /* defaultSchemaName */,
        context);

    std::string query;
    auto source = input->getLiteralVal<std::string>(0);
    if (IcebergOptions::getRestCatalogConfig(context).restCatalogConfigured() &&
        isCatalogQualifiedTableName(source)) {
        query = generateCatalogTableQuery(input, functionName, source);
    } else {
        std::string query_options = generateQueryOptions(input, functionName);
        query = std::format("SELECT * FROM {}('{}'{})", functionName, source, query_options);
    }
    auto result = connector->executeQuery(query + " LIMIT 1");

    std::vector<LogicalType> returnTypes;
    std::vector<std::string> returnColumnNames;
    // only ICEBERG_SCAN uses scanInput
    if (functionName == "ICEBERG_SCAN") {
        auto scanInput =
            dynamic_cast_checked<ExtraScanTableFuncBindInput*>(input->extraInput.get());
        returnColumnNames = scanInput->expectedColumnNames;
        if (scanInput->expectedColumnNames.empty()) {
            for (auto name : result->names) {
                returnColumnNames.push_back(name);
            }
        }
    }
    for (auto type : result->types) {
        returnTypes.push_back(
            duckdb_extension::DuckDBTypeConverter::convertDuckDBType(type.ToString()));
    }
    if (functionName != "ICEBERG_SCAN") {
        for (auto name : result->names) {
            returnColumnNames.push_back(name);
        }
    }
    DASSERT(returnTypes.size() == returnColumnNames.size());
    returnColumnNames =
        TableFunction::extractYieldVariables(returnColumnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);
    return std::make_unique<delta_extension::DeltaScanBindData>(std::move(query), connector,
        duckdb_extension::DuckDBResultConverter{returnTypes}, columns, context);
}

} // namespace iceberg_extension
} // namespace lbug
