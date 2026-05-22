#include "connector/adbc_connector.h"

#include <sstream>

#include "common/arrow/arrow_converter.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include <format>

namespace lbug {
namespace adbc_extension {

static constexpr const char* DRIVER_OPTION = "DRIVER";
static constexpr const char* TABLES_OPTION = "TABLES";
static constexpr const char* SCHEMA_OPTION = "SCHEMA";

static std::vector<std::string> splitCommaSeparated(const std::string& input) {
    std::vector<std::string> result;
    std::stringstream ss{input};
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = common::StringUtils::ltrim(common::StringUtils::rtrim(item));
        if (!item.empty()) {
            result.push_back(std::move(item));
        }
    }
    return result;
}

ADBCQueryResult::~ADBCQueryResult() {
    if (stream.release) {
        stream.release(&stream);
    }
    AdbcError error{};
    if (statement.private_data) {
        AdbcStatementRelease(&statement, &error);
    }
    if (connectionInitialized) {
        AdbcConnectionRelease(&connection, &error);
    }
    if (error.release) {
        error.release(&error);
    }
}

ADBCConnector::ADBCConnector(const binder::AttachOption& attachOption)
    : attachOption{attachOption} {}

ADBCConnector::~ADBCConnector() {
    if (connectionInitialized) {
        AdbcConnectionRelease(&connection, &error);
    }
    if (databaseInitialized) {
        AdbcDatabaseRelease(&database, &error);
    }
    if (error.release) {
        error.release(&error);
    }
}

bool ADBCConnector::hasOption(const std::string& key) const {
    return attachOption.options.contains(key);
}

std::string ADBCConnector::getStringOption(const std::string& key,
    const std::string& defaultValue) const {
    if (!hasOption(key)) {
        return defaultValue;
    }
    const auto& value = attachOption.options.at(key);
    if (value.getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING) {
        throw common::RuntimeException{std::format("Invalid option value for {}", key)};
    }
    return value.getValue<std::string>();
}

void ADBCConnector::checkStatus(AdbcStatusCode status, const std::string& operation) const {
    if (status == ADBC_STATUS_OK) {
        return;
    }
    std::string message = error.message == nullptr ? "unknown ADBC error" : error.message;
    throw common::RuntimeException{std::format("{} failed: {}", operation, message)};
}

void ADBCConnector::connect(const std::string& uri) {
    if (!hasOption(DRIVER_OPTION)) {
        throw common::RuntimeException{"ADBC attach requires a DRIVER option."};
    }
    checkStatus(AdbcDatabaseNew(&database, &error), "AdbcDatabaseNew");
    databaseInitialized = true;
    auto driverName = getStringOption(DRIVER_OPTION);
    checkStatus(AdbcDatabaseSetOption(&database, "driver", driverName.c_str(), &error),
        "AdbcDatabaseSetOption(driver)");
    checkStatus(AdbcDriverManagerDatabaseSetLoadFlags(&database, ADBC_LOAD_FLAG_DEFAULT, &error),
        "AdbcDriverManagerDatabaseSetLoadFlags");
    if (!uri.empty()) {
        auto uriOption =
            uri.find("://") != std::string::npos || uri.starts_with("file:") ? "uri" : "path";
        checkStatus(AdbcDatabaseSetOption(&database, uriOption, uri.c_str(), &error),
            std::format("AdbcDatabaseSetOption({})", uriOption));
    }
    for (auto& [key, value] : attachOption.options) {
        auto upperKey = common::StringUtils::getUpper(key);
        if (upperKey == DRIVER_OPTION || upperKey == TABLES_OPTION || upperKey == SCHEMA_OPTION) {
            continue;
        }
        if (value.getDataType().getLogicalTypeID() != common::LogicalTypeID::STRING) {
            throw common::RuntimeException{std::format("Invalid option value for {}", key)};
        }
        auto stringValue = value.getValue<std::string>();
        checkStatus(AdbcDatabaseSetOption(&database, key.c_str(), stringValue.c_str(), &error),
            std::format("AdbcDatabaseSetOption({})", key));
    }
    checkStatus(AdbcDatabaseInit(&database, &error), "AdbcDatabaseInit");
    checkStatus(AdbcConnectionNew(&connection, &error), "AdbcConnectionNew");
    connectionInitialized = true;
    checkStatus(AdbcConnectionInit(&connection, &database, &error), "AdbcConnectionInit");
}

std::vector<std::string> ADBCConnector::getTableNames() const {
    auto tables = getStringOption(TABLES_OPTION);
    if (tables.empty()) {
        throw common::RuntimeException{
            "ADBC attach currently requires TABLES='table1,table2,...' for table discovery."};
    }
    return splitCommaSeparated(tables);
}

std::vector<std::pair<std::string, common::LogicalType>> ADBCConnector::getTableSchema(
    const std::string& schemaName, const std::string& tableName) const {
    std::lock_guard<std::mutex> lock{mtx};
    if (schemaCache.contains(tableName)) {
        std::vector<std::pair<std::string, common::LogicalType>> cachedResult;
        cachedResult.reserve(schemaCache.at(tableName).size());
        for (auto& [name, type] : schemaCache.at(tableName)) {
            cachedResult.emplace_back(name, type.copy());
        }
        return cachedResult;
    }
    ArrowSchemaWrapper schema;
    checkStatus(AdbcConnectionGetTableSchema(&connection, nullptr,
                    schemaName.empty() ? nullptr : schemaName.c_str(), tableName.c_str(), &schema,
                    &error),
        std::format("AdbcConnectionGetTableSchema({})", tableName));
    std::vector<std::pair<std::string, common::LogicalType>> result;
    result.reserve(schema.n_children);
    for (auto i = 0; i < schema.n_children; i++) {
        auto child = schema.children[i];
        result.emplace_back(child->name == nullptr ? std::format("column{}", i) : child->name,
            common::ArrowConverter::fromArrowSchema(child));
    }
    std::vector<std::pair<std::string, common::LogicalType>> cachedResult;
    cachedResult.reserve(result.size());
    for (auto& [name, type] : result) {
        cachedResult.emplace_back(name, type.copy());
    }
    schemaCache.emplace(tableName, std::move(cachedResult));
    return result;
}

std::unique_ptr<ADBCQueryResult> ADBCConnector::executeQuery(const std::string& query,
    const std::vector<std::string>& /*columnNames*/,
    const std::vector<common::LogicalType>& /*columnTypes*/) const {
    auto result = std::make_unique<ADBCQueryResult>();
    AdbcError queryError{};
    auto checkQueryStatus = [&queryError](AdbcStatusCode status, const std::string& operation) {
        if (status == ADBC_STATUS_OK) {
            return;
        }
        std::string message =
            queryError.message == nullptr ? "unknown ADBC error" : queryError.message;
        if (queryError.release) {
            queryError.release(&queryError);
        }
        throw common::RuntimeException{std::format("{} failed: {}", operation, message)};
    };
    {
        std::lock_guard<std::mutex> lock{mtx};
        checkQueryStatus(AdbcConnectionNew(&result->connection, &queryError), "AdbcConnectionNew");
        result->connectionInitialized = true;
        checkQueryStatus(AdbcConnectionInit(&result->connection,
                             const_cast<AdbcDatabase*>(&database), &queryError),
            "AdbcConnectionInit");
    }
    checkQueryStatus(AdbcStatementNew(&result->connection, &result->statement, &queryError),
        "AdbcStatementNew");
    checkQueryStatus(AdbcStatementSetSqlQuery(&result->statement, query.c_str(), &queryError),
        "AdbcStatementSetSqlQuery");
    int64_t rowsAffected = 0;
    checkQueryStatus(
        AdbcStatementExecuteQuery(&result->statement, &result->stream, &rowsAffected, &queryError),
        "AdbcStatementExecuteQuery");
    if (result->stream.get_schema(&result->stream, &result->schema) != 0) {
        auto streamError = result->stream.get_last_error == nullptr ?
                               "unknown Arrow stream error" :
                               result->stream.get_last_error(&result->stream);
        if (queryError.release) {
            queryError.release(&queryError);
        }
        throw common::RuntimeException{
            std::format("ArrowArrayStream.get_schema failed: {}", streamError)};
    }
    // Fully consume the ADBC stream before returning control to the execution engine. Some drivers
    // keep query state pending while the stream is open, and later catalog calls can close it.
    while (true) {
        ArrowArrayWrapper array;
        if (result->stream.get_next(&result->stream, &array) != 0) {
            auto streamError = result->stream.get_last_error == nullptr ?
                                   "unknown Arrow stream error" :
                                   result->stream.get_last_error(&result->stream);
            if (queryError.release) {
                queryError.release(&queryError);
            }
            throw common::RuntimeException{
                std::format("ArrowArrayStream.get_next failed after {} batches: {}",
                    result->arrays.size(), streamError)};
        }
        if (array.release == nullptr) {
            break;
        }
        if (array.length > 0) {
            result->arrays.push_back(std::move(array));
        }
    }
    if (result->stream.release) {
        result->stream.release(&result->stream);
    }
    if (queryError.release) {
        queryError.release(&queryError);
    }
    return result;
}

} // namespace adbc_extension
} // namespace lbug
