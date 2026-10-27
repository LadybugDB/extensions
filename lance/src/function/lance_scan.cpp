#include "function/lance_scan.h"

#include <algorithm>

#include "common/data_chunk/data_chunk.h"
#include "common/exception/binder.h"
#include "common/vector/value_vector.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"

namespace lbug {
namespace lance_extension {

using namespace function;
using namespace common;

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    // LOAD FROM / COPY callers attach scan metadata; direct CALLs do not.
    const auto* scanInput = input->extraInput ?
                                input->extraInput->constPtrCast<ExtraScanTableFuncBindInput>() :
                                nullptr;
    const auto datasetPath = input->getLiteralVal<std::string>(0);
    LanceDataset dataset{datasetPath};
    const auto schema = dataset.schema();
    std::vector<LogicalType> returnTypes;
    std::vector<std::string> returnColumnNames =
        scanInput ? scanInput->expectedColumnNames : std::vector<std::string>{};
    if (returnColumnNames.empty()) {
        for (auto& col : schema) {
            returnColumnNames.push_back(col.name);
            returnTypes.push_back(col.ladybugType.copy());
        }
    } else {
        for (auto& name : returnColumnNames) {
            auto it = std::find_if(schema.begin(), schema.end(),
                [&](const LanceDatasetColumn& col) { return col.name == name; });
            if (it == schema.end()) {
                throw BinderException{
                    "Lance dataset '" + datasetPath + "' has no column '" + name + "'"};
            }
            returnTypes.push_back(it->ladybugType.copy());
        }
    }
    DASSERT(returnTypes.size() == returnColumnNames.size());
    returnColumnNames =
        TableFunction::extractYieldVariables(returnColumnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);
    return std::make_unique<LanceScanBindData>(datasetPath, std::move(returnColumnNames),
        std::move(returnTypes), std::move(columns), context);
}

std::unique_ptr<TableFuncSharedState> initLanceScanSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<LanceScanBindData>();
    auto dataset = std::make_shared<LanceDataset>(bindData->datasetPath);
    std::vector<LanceReader::ProjectedColumn> projection;
    for (auto i = 0u; i < bindData->columnNames.size(); ++i) {
        projection.push_back(LanceReader::ProjectedColumn{bindData->columnNames[i],
            bindData->columnTypes[i].copy()});
    }
    return std::make_unique<LanceScanSharedState>(std::move(dataset), std::move(projection));
}

offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto sharedState = input.sharedState->ptrCast<LanceScanSharedState>();
    std::lock_guard lock{sharedState->mtx};
    auto projTypes = sharedState->reader.getProjectionTypes();
    if (projTypes.empty()) {
        return 0;
    }
    auto* context = input.context->clientContext;
    auto memoryManager = storage::MemoryManager::Get(*context);
    DataChunk slice(projTypes.size());
    for (auto i = 0u; i < projTypes.size(); ++i) {
        slice.insert(i, std::make_shared<ValueVector>(projTypes[i].copy(), memoryManager));
    }
    slice.state->getSelVectorUnsafe().setToFiltered(0);
    const auto rowsRead = sharedState->reader.next(slice);
    if (rowsRead == 0) {
        return 0;
    }
    // Copy the slice into the output chunk (same types by construction).
    for (auto i = 0u; i < projTypes.size(); ++i) {
        auto& srcVector = slice.getValueVector(i);
        auto& dstVector = output.dataChunk.getValueVectorMutable(i);
        for (auto row = 0u; row < rowsRead; ++row) {
            dstVector.copyFromVectorData(row, &srcVector, row);
        }
    }
    output.dataChunk.state->getSelVectorUnsafe().setToUnfiltered(rowsRead);
    return output.dataChunk.state->getSelVector().getSelSize();
}

function_set LanceScanFunction::getFunctionSet() {
    function_set functionSet;
    auto function = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::STRING});
    function->tableFunc = tableFunc;
    function->bindFunc = bindFunc;
    function->initSharedStateFunc = initLanceScanSharedState;
    function->initLocalStateFunc = TableFunction::initEmptyLocalState;
    functionSet.push_back(std::move(function));
    return functionSet;
}

} // namespace lance_extension
} // namespace lbug
