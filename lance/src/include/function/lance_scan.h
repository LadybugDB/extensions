#pragma once

#include <mutex>

#include "binder/binder.h"
#include "function/table/table_function.h"
#include "storage/lance_reader.h"

namespace lbug {
namespace lance_extension {

// Reads a Lance dataset through the native Lance reader (the same engine as
// lance-duckdb, without the DuckDB layer). Registering LANCE_SCAN also enables
// `LOAD FROM ... (file_format='lance')` and `COPY ... FROM ...
// (file_format='lance')` via the `<FORMAT>_SCAN` convention.
struct LanceScanFunction {
    static constexpr const char* name = "LANCE_SCAN";

    static function::function_set getFunctionSet();
};

struct LanceScanBindData final : function::ScanFileBindData {
    std::string datasetPath;
    std::vector<std::string> columnNames;
    std::vector<common::LogicalType> columnTypes;

    LanceScanBindData(std::string datasetPath, std::vector<std::string> columnNames,
        std::vector<common::LogicalType> columnTypes, binder::expression_vector columns,
        main::ClientContext* context)
        : function::ScanFileBindData{std::move(columns), 0 /* numRows */, common::FileScanInfo{},
              context},
          datasetPath{std::move(datasetPath)}, columnNames{std::move(columnNames)},
          columnTypes{std::move(columnTypes)} {}

    // LogicalType is not copyable; rebuild the type vector element-wise.
    LanceScanBindData(const LanceScanBindData& other)
        : function::ScanFileBindData{other}, datasetPath{other.datasetPath},
          columnNames{other.columnNames} {
        for (auto& type : other.columnTypes) {
            columnTypes.push_back(type.copy());
        }
    }

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<LanceScanBindData>(*this);
    }
};

struct LanceScanSharedState final : function::TableFuncSharedState {
    explicit LanceScanSharedState(std::shared_ptr<LanceDataset> dataset,
        std::vector<LanceReader::ProjectedColumn> projection)
        : dataset{std::move(dataset)}, reader{this->dataset, std::move(projection)} {
        reader.openRange(0, UINT64_MAX);
    }

    std::mutex mtx;
    std::shared_ptr<LanceDataset> dataset;
    LanceReader reader;
};

std::unique_ptr<function::TableFuncSharedState> initLanceScanSharedState(
    const function::TableFuncInitSharedStateInput& input);

common::offset_t tableFunc(const function::TableFuncInput& input,
    function::TableFuncOutput& output);

} // namespace lance_extension
} // namespace lbug
