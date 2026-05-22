#include "function/adbc_scan.h"

#include <algorithm>

#include "binder/binder.h"
#include "common/arrow/arrow_converter.h"
#include "common/constants.h"
#include "common/exception/runtime.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include <format>

namespace lbug {
namespace adbc_extension {

static std::string quoteIdentifier(const std::string& value) {
    std::string result = "\"";
    for (auto ch : value) {
        if (ch == '"') {
            result += "\"\"";
        } else {
            result += ch;
        }
    }
    result += "\"";
    return result;
}

static std::string joinColumns(const std::vector<std::string>& columnNames) {
    std::string result;
    bool first = true;
    for (auto& columnName : columnNames) {
        if (!first) {
            result += ", ";
        }
        result += quoteIdentifier(columnName);
        first = false;
    }
    return result.empty() ? "*" : result;
}

std::string ADBCScanBindData::getSQL() const {
    auto sql = std::format("SELECT {} FROM {}", joinColumns(scanInfo->columnNames),
        quoteIdentifier(scanInfo->tableName));
    if (getLimitNum() != common::INVALID_ROW_IDX) {
        sql += std::format(" LIMIT {}", getLimitNum());
    }
    return sql;
}

struct ADBCScanFunction {
    static constexpr char NAME[] = "adbc_scan";

    static common::offset_t tableFunc(const function::TableFuncInput& input,
        function::TableFuncOutput& output);
    static std::unique_ptr<function::TableFuncBindData> bindFunc(
        std::shared_ptr<ADBCTableScanInfo> scanInfo, main::ClientContext* context,
        const function::TableFuncBindInput* input);
    static std::unique_ptr<function::TableFuncSharedState> initSharedState(
        const function::TableFuncInitSharedStateInput& input);
    static std::unique_ptr<function::TableFuncLocalState> initLocalState(
        const function::TableFuncInitLocalStateInput& input);
};

struct ADBCScanLocalState final : function::TableFuncLocalState {
    ArrowArrayWrapper array;
    uint64_t offset = 0;
};

std::unique_ptr<function::TableFuncSharedState> ADBCScanFunction::initSharedState(
    const function::TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<ADBCScanBindData>();
    return std::make_unique<ADBCScanSharedState>(bindData->scanInfo->connector.executeQuery(
        bindData->getSQL(), bindData->scanInfo->columnNames, bindData->scanInfo->columnTypes));
}

common::offset_t ADBCScanFunction::tableFunc(const function::TableFuncInput& input,
    function::TableFuncOutput& output) {
    auto sharedState = input.sharedState->ptrCast<ADBCScanSharedState>();
    auto localState = input.localState->ptrCast<ADBCScanLocalState>();
    auto bindData = input.bindData->constPtrCast<ADBCScanBindData>();
    while (localState->array.release == nullptr ||
           localState->offset >= static_cast<uint64_t>(localState->array.length)) {
        localState->array = ArrowArrayWrapper{};
        localState->offset = 0;
        {
            std::lock_guard<std::mutex> lock{sharedState->mtx};
            if (sharedState->queryResult->nextArrayIdx >= sharedState->queryResult->arrays.size()) {
                return 0;
            }
            localState->array = std::move(
                sharedState->queryResult->arrays[sharedState->queryResult->nextArrayIdx++]);
        }
        if (localState->array.release == nullptr || localState->array.length == 0) {
            return 0;
        }
    }
    auto count = std::min<uint64_t>(common::DEFAULT_VECTOR_CAPACITY,
        static_cast<uint64_t>(localState->array.length) - localState->offset);
    auto& schema = sharedState->queryResult->schema;
    for (auto i = 0u; i < bindData->scanInfo->columnNames.size(); i++) {
        auto srcOffset = localState->array.children[i]->offset + localState->offset;
        common::ArrowNullMaskTree mask(schema.children[i], localState->array.children[i], srcOffset,
            count);
        common::ArrowConverter::fromArrowArray(schema.children[i], localState->array.children[i],
            output.dataChunk.getValueVectorMutable(i), &mask, srcOffset, 0, count);
    }
    localState->offset += count;
    return count;
}

std::unique_ptr<function::TableFuncBindData> ADBCScanFunction::bindFunc(
    std::shared_ptr<ADBCTableScanInfo> scanInfo, main::ClientContext* /*context*/,
    const function::TableFuncBindInput* input) {
    auto columnNames = function::TableFunction::extractYieldVariables(scanInfo->columnNames,
        input->yieldVariables);
    std::vector<common::LogicalType> columnTypes;
    for (auto& columnName : columnNames) {
        auto columnIt =
            std::find(scanInfo->columnNames.begin(), scanInfo->columnNames.end(), columnName);
        columnTypes.push_back(
            scanInfo->columnTypes[columnIt - scanInfo->columnNames.begin()].copy());
    }
    auto columns = input->binder->createVariables(columnNames, columnTypes);
    auto selectedScanInfo = std::make_shared<ADBCTableScanInfo>(scanInfo->tableName,
        std::move(columnNames), std::move(columnTypes), scanInfo->connector);
    return std::make_unique<ADBCScanBindData>(std::move(selectedScanInfo), std::move(columns));
}

std::unique_ptr<function::TableFuncLocalState> ADBCScanFunction::initLocalState(
    const function::TableFuncInitLocalStateInput&) {
    return std::make_unique<ADBCScanLocalState>();
}

function::TableFunction getADBCScanFunction(std::shared_ptr<ADBCTableScanInfo> scanInfo) {
    auto function =
        function::TableFunction(ADBCScanFunction::NAME, std::vector<common::LogicalTypeID>{});
    function.tableFunc = ADBCScanFunction::tableFunc;
    function.bindFunc = std::bind(ADBCScanFunction::bindFunc, scanInfo, std::placeholders::_1,
        std::placeholders::_2);
    function.initSharedStateFunc = ADBCScanFunction::initSharedState;
    function.initLocalStateFunc = ADBCScanFunction::initLocalState;
    function.supportsPushDownFunc = [] { return false; };
    return function;
}

} // namespace adbc_extension
} // namespace lbug
