#pragma once

#include "main/database.h"
#include "storage/storage_extension.h"

namespace lbug {
namespace adbc_extension {

class ADBCStorageExtension final : public storage::StorageExtension {
public:
    static constexpr const char* DB_TYPE = "ADBC";

    static constexpr const char* DEFAULT_SCHEMA_NAME = "main";

    explicit ADBCStorageExtension(main::Database& database);

    bool canHandleDB(std::string dbType_) const override;
};

} // namespace adbc_extension
} // namespace lbug
