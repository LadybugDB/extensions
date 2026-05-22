#pragma once

#include <mutex>
#include <unordered_map>

#include "binder/bound_attach_info.h"
#include "common/arrow/arrow.h"
#include "common/types/types.h"

#if __has_include(<arrow-adbc/adbc.h>) && __has_include(<arrow-adbc/adbc_driver_manager.h>)
#include <arrow-adbc/adbc.h>
#include <arrow-adbc/adbc_driver_manager.h>
#else
#error "ADBC C API and driver manager headers not found"
#endif

#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE
struct ArrowArrayStream {
    int (*get_schema)(struct ArrowArrayStream*, struct ArrowSchema* out);
    int (*get_next)(struct ArrowArrayStream*, struct ArrowArray* out);
    const char* (*get_last_error)(struct ArrowArrayStream*);
    void (*release)(struct ArrowArrayStream*);
    void* private_data;
};
#endif

namespace lbug {
namespace adbc_extension {

struct ADBCQueryResult {
    AdbcConnection connection{};
    AdbcStatement statement{};
    ArrowArrayStream stream{};
    ArrowSchemaWrapper schema{};
    std::vector<ArrowArrayWrapper> arrays;
    uint64_t nextArrayIdx = 0;
    bool connectionInitialized = false;

    ADBCQueryResult() = default;
    ADBCQueryResult(const ADBCQueryResult&) = delete;
    ADBCQueryResult& operator=(const ADBCQueryResult&) = delete;
    ~ADBCQueryResult();
};

class ADBCConnector final {
public:
    explicit ADBCConnector(const binder::AttachOption& attachOption);
    ~ADBCConnector();

    void connect(const std::string& uri);

    std::vector<std::string> getTableNames() const;
    std::vector<std::pair<std::string, common::LogicalType>> getTableSchema(
        const std::string& schemaName, const std::string& tableName) const;
    std::unique_ptr<ADBCQueryResult> executeQuery(const std::string& query,
        const std::vector<std::string>& columnNames,
        const std::vector<common::LogicalType>& columnTypes) const;

private:
    std::string getStringOption(const std::string& key, const std::string& defaultValue = "") const;
    bool hasOption(const std::string& key) const;
    void checkStatus(AdbcStatusCode status, const std::string& operation) const;

private:
    const binder::AttachOption& attachOption;
    mutable std::mutex mtx;
    mutable AdbcError error{};
    AdbcDatabase database{};
    mutable AdbcConnection connection{};
    mutable std::unordered_map<std::string,
        std::vector<std::pair<std::string, common::LogicalType>>>
        schemaCache;
    bool databaseInitialized = false;
    bool connectionInitialized = false;
};

} // namespace adbc_extension
} // namespace lbug
