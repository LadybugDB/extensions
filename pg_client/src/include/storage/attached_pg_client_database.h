#pragma once

#include "connector/pg_client_connector.h"
#include "main/attached_database.h"

namespace lbug {
namespace pg_client_extension {

class AttachedPgClientDatabase final : public main::AttachedDatabase {
public:
    AttachedPgClientDatabase(std::string dbName, std::string dbType,
        std::unique_ptr<extension::CatalogExtension> catalog,
        std::unique_ptr<PgClientConnector> connector)
        : main::AttachedDatabase{std::move(dbName), std::move(dbType), std::move(catalog)},
          connector_{std::move(connector)} {}

    const PgClientConnector& getConnector() const { return *connector_; }

private:
    std::unique_ptr<PgClientConnector> connector_;
};

} // namespace pg_client_extension
} // namespace lbug
