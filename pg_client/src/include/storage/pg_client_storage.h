#pragma once

#include "storage/storage_extension.h"

namespace lbug {
namespace main {
class Database;
} // namespace main

namespace pg_client_extension {

class PgClientStorageExtension final : public storage::StorageExtension {
public:
    static constexpr const char* DB_TYPE = "PG_CLIENT";

    static constexpr const char* DEFAULT_SCHEMA_NAME = "public";

    static constexpr const char* SCHEMA_OPTION = "SCHEMA";

    explicit PgClientStorageExtension(main::Database& database);

    bool canHandleDB(std::string dbType_) const override;
};

} // namespace pg_client_extension
} // namespace lbug
