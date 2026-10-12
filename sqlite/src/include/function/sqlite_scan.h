#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/types/types.h"
#include "connector/sqlite_connector.h"
#include "function/table/bind_data.h"
#include "function/table/table_function.h"

namespace lbug {
namespace sqlite_extension {

class SqliteTableScanInfo {
public:
    SqliteTableScanInfo(std::string templateQuery, std::vector<common::LogicalType> columnTypes,
        std::vector<std::string> columnNames, const SqliteConnector& connector)
        : templateQuery{std::move(templateQuery)}, columnTypes{std::move(columnTypes)},
          columnNames{std::move(columnNames)}, connector{connector} {}

    SqliteTableScanInfo(const SqliteTableScanInfo& other) = default;

    virtual ~SqliteTableScanInfo() = default;

    virtual std::string getTemplateQuery(const main::ClientContext& /*context*/) const {
        return templateQuery;
    }

    virtual std::vector<common::LogicalType> getColumnTypes(
        const main::ClientContext& context) const;

    std::vector<std::string> getColumnNames() const { return columnNames; }

    const SqliteConnector& getConnector() const { return connector; }

private:
    std::string templateQuery;
    std::vector<common::LogicalType> columnTypes;
    std::vector<std::string> columnNames;
    const SqliteConnector& connector;
};

struct SqliteScanBindData : function::TableFuncBindData {
    std::string query;
    std::vector<std::string> columnNamesInSqlite;
    const SqliteConnector& connector;
    // Positions of the src/dst columns in the scan output when the scan
    // output carries node offsets directly (csr_rel_* contract); nullopt
    // otherwise. See TableFuncBindData::getSrcDstColumnPositions.
    std::optional<std::pair<common::column_id_t, common::column_id_t>> srcDstColumnPositions;

    SqliteScanBindData(std::string query, std::vector<std::string> columnNamesInSqlite,
        const SqliteConnector& connector, binder::expression_vector columns,
        std::optional<std::pair<common::column_id_t, common::column_id_t>> srcDstColumnPositions =
            std::nullopt)
        : function::TableFuncBindData{std::move(columns), 0 /* numRows */}, query{std::move(query)},
          columnNamesInSqlite{std::move(columnNamesInSqlite)}, connector{connector},
          srcDstColumnPositions{srcDstColumnPositions} {}

    SqliteScanBindData(const SqliteScanBindData& other)
        : function::TableFuncBindData{other}, query{other.query},
          columnNamesInSqlite{other.columnNamesInSqlite}, connector{other.connector},
          srcDstColumnPositions{other.srcDstColumnPositions} {}

    std::optional<std::pair<common::column_id_t, common::column_id_t>>
    getSrcDstColumnPositions() const override {
        return srcDstColumnPositions;
    }

    // Whether the bound output carries a synthesized internal-ID column at
    // position 0 (present when the scan runs under a MATCH node pattern).
    // The SQLite query itself never returns it.
    bool hasInternalIDColumn() const;
    // Bound column indices to read, honouring planner column skips. Index 0
    // denotes the synthesized internal ID when hasInternalIDColumn().
    std::vector<uint32_t> getColumnIndicesToSelect() const;
    // Maps a bound column index to its position in columnNamesInSqlite.
    uint32_t boundIdxToSqliteIdx(uint32_t boundIdx) const;

    std::string getSQL() const;
    std::string getDescription() const override;

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<SqliteScanBindData>(*this);
    }

    std::unique_ptr<TableFuncBindData> copyWithQuery(const std::string& newQuery,
        binder::expression_vector columns,
        const std::vector<std::string>& columnNamesInResult) const override {
        return std::make_unique<SqliteScanBindData>(newQuery, columnNamesInResult, connector,
            std::move(columns), srcDstColumnPositions);
    }
};

struct SqliteScanSharedState final : function::TableFuncSharedState {
    SqliteQueryResult queryResult;
    uint64_t currentOffset;

    explicit SqliteScanSharedState(SqliteQueryResult queryResult)
        : TableFuncSharedState{queryResult.numRows}, queryResult{std::move(queryResult)},
          currentOffset{0} {}
};

function::TableFunction getScanFunction(std::shared_ptr<SqliteTableScanInfo> scanInfo);

} // namespace sqlite_extension
} // namespace lbug
