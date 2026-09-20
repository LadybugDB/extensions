#pragma once

#include "storage/storage_extension.h"

namespace lbug {
namespace main {
class Database;
} // namespace main

namespace ducklake_extension {

class DuckLakeStorageExtension final : public storage::StorageExtension {
public:
    static constexpr const char* DB_TYPE = "DUCKLAKE";

    // DuckLake catalogs live in DuckDB, whose default schema is `main`.
    static constexpr const char* DEFAULT_SCHEMA_NAME = "main";

    explicit DuckLakeStorageExtension(main::Database& database);

    bool canHandleDB(std::string dbType) const override;
};

} // namespace ducklake_extension
} // namespace lbug
