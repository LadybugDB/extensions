#pragma once

#include "binder/binder.h"
#include "connector/connector_factory.h"
#include "connector/duckdb_result_converter.h"
#include "connector/duckdb_type_converter.h"
#include "connector/lance_connector.h"
#include "function/table/table_function.h"

namespace lbug {
namespace lance_extension {

// Reads a Lance dataset through DuckDB's lance extension
// (lance-duckdb replacement scan: `SELECT * FROM '<path>.lance'`).
// Registering LANCE_SCAN also enables `LOAD FROM ... (file_format='lance')`
// and `COPY ... FROM ... (file_format='lance')` via the `<FORMAT>_SCAN`
// convention.
struct LanceScanFunction {
    static constexpr const char* name = "LANCE_SCAN";

    static function::function_set getFunctionSet();
};

struct LanceScanBindData final : function::ScanFileBindData {
    std::string query;
    std::shared_ptr<duckdb_extension::DuckDBConnector> connector;
    duckdb_extension::DuckDBResultConverter converter;

    LanceScanBindData(std::string query,
        std::shared_ptr<duckdb_extension::DuckDBConnector> connector,
        duckdb_extension::DuckDBResultConverter converter, binder::expression_vector columns,
        main::ClientContext* context)
        : function::ScanFileBindData{std::move(columns), 0 /* numRows */, common::FileScanInfo{},
              context},
          query{std::move(query)}, connector{std::move(connector)},
          converter{std::move(converter)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<LanceScanBindData>(*this);
    }
};

std::unique_ptr<function::TableFuncSharedState> initLanceScanSharedState(
    const function::TableFuncInitSharedStateInput& input);

common::offset_t tableFunc(const function::TableFuncInput& input,
    function::TableFuncOutput& output);

} // namespace lance_extension
} // namespace lbug
