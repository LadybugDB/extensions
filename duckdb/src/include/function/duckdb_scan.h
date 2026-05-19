#pragma once

#include "binder/expression/expression_util.h"
#include "common/types/types.h"
#include "connector/duckdb_result_converter.h"
#include "function/table/bind_data.h"
#include "function/table/scan_file_function.h"

namespace lbug {
namespace duckdb_extension {

class DuckDBConnector;

class DuckDBTableScanInfo {
public:
    DuckDBTableScanInfo(std::string templateQuery, std::vector<common::LogicalType> columnTypes,
        std::vector<std::string> columnNames, const DuckDBConnector& connector)
        : templateQuery{std::move(templateQuery)}, columnTypes{std::move(columnTypes)},
          columnNames{std::move(columnNames)}, connector{connector} {}
    DuckDBTableScanInfo(std::string templateQuery, std::vector<common::LogicalType> columnTypes,
        std::vector<std::string> columnNames, const DuckDBConnector& connector,
        std::string internalIDColumnName)
        : templateQuery{std::move(templateQuery)}, columnTypes{std::move(columnTypes)},
          columnNames{std::move(columnNames)}, connector{connector},
          internalIDColumnName{std::move(internalIDColumnName)} {}

    DuckDBTableScanInfo(const DuckDBTableScanInfo& other) = default;

    virtual ~DuckDBTableScanInfo() = default;

    virtual std::string getTemplateQuery(const main::ClientContext& /*context*/) const {
        return templateQuery;
    }

    virtual std::vector<common::LogicalType> getColumnTypes(
        const main::ClientContext& /*context*/) const {
        return copyVector(columnTypes);
    }

    std::vector<std::string> getColumnNames() const { return columnNames; }

    const std::string& getInternalIDColumnName() const { return internalIDColumnName; }

    const DuckDBConnector& getConnector() const { return connector; }

private:
    std::string templateQuery;
    std::vector<common::LogicalType> columnTypes;
    std::vector<std::string> columnNames;
    const DuckDBConnector& connector;
    std::string internalIDColumnName = "rowid";
};

struct DuckDBScanBindData : function::TableFuncBindData {
    std::string query;
    std::vector<std::string> columnNamesInDuckDB;
    const DuckDBConnector& connector;
    DuckDBResultConverter converter;

    DuckDBScanBindData(std::string query, std::vector<std::string> columnNamesInDuckDB,
        const DuckDBConnector& connector, binder::expression_vector columns)
        : function::TableFuncBindData{std::move(columns), 0 /* numRows */}, query{std::move(query)},
          columnNamesInDuckDB{std::move(columnNamesInDuckDB)}, connector{connector},
          converter{binder::ExpressionUtil::getDataTypes(this->columns)} {}
    DuckDBScanBindData(const DuckDBScanBindData& other)
        : TableFuncBindData{other}, query{other.query},
          columnNamesInDuckDB{other.columnNamesInDuckDB}, connector{other.connector},
          converter{binder::ExpressionUtil::getDataTypes(this->columns)} {}

    std::string getColumnsToSelect() const;
    std::vector<uint32_t> getColumnIndicesToSelect() const;
    std::string getSQL() const;

    std::string getDescription() const override;

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DuckDBScanBindData>(*this);
    }

    std::unique_ptr<TableFuncBindData> copyWithQuery(const std::string& newQuery,
        binder::expression_vector columns,
        const std::vector<std::string>& columnNamesInResult) const override {
        return std::make_unique<DuckDBScanBindData>(newQuery, columnNamesInResult, connector,
            std::move(columns));
    }
};

struct DuckDBScanSharedState final : function::TableFuncSharedState {
    explicit DuckDBScanSharedState(std::shared_ptr<duckdb::MaterializedQueryResult> queryResult);

    std::shared_ptr<duckdb::MaterializedQueryResult> queryResult;
};

function::TableFunction getScanFunction(std::shared_ptr<DuckDBTableScanInfo> scanInfo);

} // namespace duckdb_extension
} // namespace lbug
