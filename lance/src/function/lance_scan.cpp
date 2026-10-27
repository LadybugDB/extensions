#include "function/lance_scan.h"

#include <format>

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
    auto connector = std::make_shared<LanceConnector>();
    connector->connect("" /* inMemDB */, "" /* defaultCatalogName */, "" /* defaultSchemaName */,
        context);
    // DuckDB's lance extension resolves '<path>.lance' through its replacement scan.
    std::string query = std::format("SELECT * FROM '{}'",
        LanceConnector::escapeLiteral(input->getLiteralVal<std::string>(0)));
    auto result = connector->executeQuery(query + " LIMIT 1");
    std::vector<LogicalType> returnTypes;
    std::vector<std::string> returnColumnNames =
        scanInput ? scanInput->expectedColumnNames : std::vector<std::string>{};
    for (auto type : result->GetTypes()) {
        returnTypes.push_back(
            duckdb_extension::DuckDBTypeConverter::convertDuckDBType(type.ToString()));
    }

    if (returnColumnNames.empty()) {
        for (auto name : result->GetNames()) {
            returnColumnNames.push_back(name.GetIdentifierName());
        }
    }
    DASSERT(returnTypes.size() == returnColumnNames.size());
    returnColumnNames =
        TableFunction::extractYieldVariables(returnColumnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);
    return std::make_unique<LanceScanBindData>(std::move(query), connector,
        duckdb_extension::DuckDBResultConverter{returnTypes}, columns, context);
}

std::unique_ptr<TableFuncSharedState> initLanceScanSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto lanceScanBindData = input.bindData->constPtrCast<LanceScanBindData>();
    auto queryResult = lanceScanBindData->connector->executeQuery(lanceScanBindData->query);
    return std::make_unique<duckdb_extension::DuckDBScanSharedState>(std::move(queryResult));
}

offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto sharedState = input.sharedState->ptrCast<duckdb_extension::DuckDBScanSharedState>();
    auto lanceScanBindData = input.bindData->constPtrCast<LanceScanBindData>();
    std::unique_ptr<duckdb::DataChunk> result;
    try {
        // Duckdb queryResult.fetch() is not thread safe, we have to acquire a lock there.
        std::lock_guard lock{sharedState->mtx};
        result = sharedState->queryResult->Fetch();
    } catch (std::exception& e) {
        return 0;
    }
    if (result == nullptr) {
        return 0;
    }
    lanceScanBindData->converter.convertDuckDBResultToVector(*result, output.dataChunk);
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
