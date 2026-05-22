#pragma once

#include "catalog/catalog_entry/table_catalog_entry.h"
#include "function/adbc_scan.h"

namespace lbug {
namespace catalog {

class ADBCTableCatalogEntry final : public TableCatalogEntry {
public:
    ADBCTableCatalogEntry(std::string name, std::optional<function::TableFunction> scanFunction,
        std::shared_ptr<adbc_extension::ADBCTableScanInfo> scanInfo);

    common::TableType getTableType() const override;
    std::optional<function::TableFunction> getScanFunction() const override { return scanFunction; }
    std::unique_ptr<binder::BoundTableScanInfo> getBoundScanInfo(main::ClientContext* context,
        const std::string& nodeUniqueName = "") override;
    std::unique_ptr<TableCatalogEntry> copy() const override;

private:
    std::unique_ptr<binder::BoundExtraCreateCatalogEntryInfo> getBoundExtraCreateInfo(
        transaction::Transaction* transaction) const override;

private:
    std::optional<function::TableFunction> scanFunction;
    std::shared_ptr<adbc_extension::ADBCTableScanInfo> scanInfo;
};

} // namespace catalog
} // namespace lbug
