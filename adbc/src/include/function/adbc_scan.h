#pragma once

#include "common/arrow/arrow.h"
#include "connector/adbc_connector.h"
#include "function/table/bind_data.h"
#include "function/table/table_function.h"

namespace lbug {
namespace adbc_extension {

struct ADBCTableScanInfo {
    std::string tableName;
    // Prebuilt FROM clause (catalog/schema-qualified when the ATTACH
    // specified CATALOG, else the bare quoted table). Built once so the
    // quoting is not applied twice.
    std::string fromClause;
    // Lakehouse-quoting for identifiers: with ANSI mode off (Spark/
    // Databricks), "x" is a string literal, so columns must use backticks.
    // Legacy drivers keep double quotes.
    bool backtickIds = false;
    std::vector<std::string> columnNames;
    std::vector<common::LogicalType> columnTypes;
    const ADBCConnector& connector;

    ADBCTableScanInfo(std::string tableName, std::string fromClause, bool backtickIds,
        std::vector<std::string> columnNames, std::vector<common::LogicalType> columnTypes,
        const ADBCConnector& connector)
        : tableName{std::move(tableName)}, fromClause{std::move(fromClause)},
          backtickIds{backtickIds}, columnNames{std::move(columnNames)},
          columnTypes{std::move(columnTypes)}, connector{connector} {}
};

struct ADBCScanBindData final : function::TableFuncBindData {
    std::shared_ptr<ADBCTableScanInfo> scanInfo;

    ADBCScanBindData(std::shared_ptr<ADBCTableScanInfo> scanInfo, binder::expression_vector columns)
        : function::TableFuncBindData{std::move(columns), 0}, scanInfo{std::move(scanInfo)} {}
    ADBCScanBindData(const ADBCScanBindData& other)
        : function::TableFuncBindData{other}, scanInfo{other.scanInfo} {}

    std::string getSQL() const;
    std::string getDescription() const override { return getSQL(); }
    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ADBCScanBindData>(*this);
    }
};

struct ADBCScanSharedState final : function::TableFuncSharedState {
    explicit ADBCScanSharedState(std::unique_ptr<ADBCQueryResult> queryResult)
        : queryResult{std::move(queryResult)} {}

    std::unique_ptr<ADBCQueryResult> queryResult;
};

function::TableFunction getADBCScanFunction(std::shared_ptr<ADBCTableScanInfo> scanInfo);

} // namespace adbc_extension
} // namespace lbug
