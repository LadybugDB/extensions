#pragma once

#include "storage/storage_extension.h"

namespace lbug {
namespace main {
class Database;
} // namespace main

namespace iceberg_extension {

class IcebergStorageExtension final : public storage::StorageExtension {
public:
    static constexpr const char* DB_TYPE = "ICEBERG";

    static constexpr const char* DEFAULT_SCHEMA_NAME = "default";

    explicit IcebergStorageExtension(main::Database& database);

    bool canHandleDB(std::string dbType) const override;
};

} // namespace iceberg_extension
} // namespace lbug
