#pragma once

#include <optional>
#include <memory>

#include "catalog/catalog_entry/table_catalog_entry.h"
#include "function/pg_client_scan.h"

namespace lbug {
namespace catalog {

class PgClientTableCatalogEntry final : public TableCatalogEntry {
public:
    // constructors
    PgClientTableCatalogEntry(std::string name,
        std::optional<function::TableFunction> scanFunction,
        std::shared_ptr<pg_client_extension::PgClientTableScanInfo> scanInfo);

    // getter & setter
    common::TableType getTableType() const override;
    std::optional<function::TableFunction> getScanFunction() const override { return scanFunction; }
    std::unique_ptr<binder::BoundTableScanInfo> getBoundScanInfo(main::ClientContext* context,
        const std::string& nodeUniqueName = "") override;

    // serialization & deserialization
    std::unique_ptr<TableCatalogEntry> copy() const override;

private:
    std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo> getBoundExtraCreateInfo(
        transaction::Transaction* transaction) const override;

private:
    std::optional<function::TableFunction> scanFunction;
    std::shared_ptr<pg_client_extension::PgClientTableScanInfo> scanInfo;
};

} // namespace catalog
} // namespace lbug
