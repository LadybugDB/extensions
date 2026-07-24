#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/copy_constructors.h"
#include "common/types/types.h"
#include "connector/pg_client_connector.h"
#include "function/table/bind_data.h"
#include "function/table/table_function.h"

namespace lbug {
namespace pg_client_extension {

class PgClientConnector;

class PgClientTableScanInfo {
public:
    PgClientTableScanInfo(std::string templateQuery,
        std::vector<common::LogicalType> columnTypes,
        std::vector<std::string> columnNames, const PgClientConnector& connector)
        : templateQuery{std::move(templateQuery)}, columnTypes{std::move(columnTypes)},
          columnNames{std::move(columnNames)}, connector{connector} {}

    PgClientTableScanInfo(const PgClientTableScanInfo& other) = default;

    virtual ~PgClientTableScanInfo() = default;

    virtual std::string getTemplateQuery(const main::ClientContext& /*context*/) const {
        return templateQuery;
    }

    virtual std::vector<common::LogicalType> getColumnTypes(
        const main::ClientContext& /*context*/) const {
        std::vector<common::LogicalType> result;
        result.reserve(columnTypes.size());
        for (auto& t : columnTypes) {
            result.push_back(t.copy());
        }
        return result;
    }

    std::vector<std::string> getColumnNames() const { return columnNames; }

    const PgClientConnector& getConnector() const { return connector; }

private:
    std::string templateQuery;
    std::vector<common::LogicalType> columnTypes;
    std::vector<std::string> columnNames;
    const PgClientConnector& connector;
};

struct PgClientScanBindData : function::TableFuncBindData {
    std::string query;
    std::vector<std::string> columnNamesInPg;
    const PgClientConnector& connector;

    PgClientScanBindData(std::string query, std::vector<std::string> columnNamesInPg,
        const PgClientConnector& connector, binder::expression_vector columns)
        : function::TableFuncBindData{std::move(columns), 0 /* numRows */},
          query{std::move(query)}, columnNamesInPg{std::move(columnNamesInPg)},
          connector{connector} {}

    PgClientScanBindData(const PgClientScanBindData& other)
        : function::TableFuncBindData{other}, query{other.query},
          columnNamesInPg{other.columnNamesInPg}, connector{other.connector} {}

    std::string getSQL() const;
    std::string getDescription() const override;

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<PgClientScanBindData>(*this);
    }

    std::unique_ptr<TableFuncBindData> copyWithQuery(const std::string& newQuery,
        binder::expression_vector columns,
        const std::vector<std::string>& columnNamesInResult) const override {
        return std::make_unique<PgClientScanBindData>(newQuery, columnNamesInResult, connector,
            std::move(columns));
    }
};

struct PgClientScanSharedState final : function::TableFuncSharedState {
    PgClientQueryResult queryResult;
    uint64_t currentOffset;

    explicit PgClientScanSharedState(PgClientQueryResult queryResult)
        : TableFuncSharedState{queryResult.numRows},
          queryResult{std::move(queryResult)}, currentOffset{0} {}
};

function::TableFunction getScanFunction(std::shared_ptr<PgClientTableScanInfo> scanInfo);

} // namespace pg_client_extension
} // namespace lbug
