#pragma once

#include "common/types/value/value.h"

namespace lbug {
namespace main {
class Database;
class ClientContext;
} // namespace main

namespace ducklake_extension {

// Optional override for the DuckLake `DATA_PATH` ATTACH parameter. When set,
// it is appended to the `ATTACH 'ducklake:...'` statement executed on the
// embedded DuckDB instance as `(DATA_PATH '...', OVERRIDE_DATA_PATH true)`,
// so the given directory is used instead of the data path stored in the
// DuckLake metadata for the current connection (the stored value is left
// untouched). Empty (the default) leaves the stored path untouched, which is
// the common case for file-backed catalogs. The override form is required
// because DuckDB rejects a plain `DATA_PATH` that differs from the stored
// path, and it is what makes relocated catalogs (e.g. checked-in test data)
// queryable.
struct DuckLakeDataPath {
    static constexpr const char* NAME = "ducklake_data_path";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::STRING;
    static common::Value getDefaultValue() { return common::Value{std::string()}; }
};

// Optional read-only flag, forwarded as `READ_ONLY` to the embedded `ATTACH`.
struct DuckLakeReadOnly {
    static constexpr const char* NAME = "ducklake_read_only";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::BOOL;
    static common::Value getDefaultValue() { return common::Value{false}; }
};

struct DuckLakeOptions {
    static void registerExtensionOptions(main::Database* db);
    static void setEnvValue(main::ClientContext* context);
    static std::string getDataPath(main::ClientContext* context);
    static bool getReadOnly(main::ClientContext* context);
};

// Translates the Ladybug ATTACH path and extension options into the
// `ATTACH 'ducklake:...' AS <alias> (...)` statement executed on the embedded
// DuckDB instance.
struct DuckLakeAttachHelper {
    static std::string getAttachQuery(const std::string& dbPath, const std::string& catalogAlias,
        main::ClientContext* context);
};

} // namespace ducklake_extension
} // namespace lbug
