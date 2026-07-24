#include "function/pg_client_scan.h"

#include <format>
#include <unordered_map>

#include "binder/binder.h"
#include "common/constants.h"
#include "common/exception/runtime.h"
#include "common/system_config.h"
#include "common/string_utils.h"
#include "common/vector/value_vector.h"
#include "connector/pg_client_connector.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "processor/execution_context.h"

using namespace lbug::function;
using namespace lbug::common;

namespace lbug {
namespace pg_client_extension {

std::string PgClientScanBindData::getSQL() const {
    // Build column selection
    std::string columns;
    bool first = true;
    for (auto i = 0u; i < columnNamesInPg.size(); i++) {
        if (!first) {
            columns += ",";
        }
        columns += columnNamesInPg[i];
        first = false;
    }

    std::string predicatesString = "";
    for (auto& predicates : getColumnPredicates()) {
        if (predicates.isEmpty()) {
            continue;
        }
        if (predicatesString.empty()) {
            predicatesString = " WHERE " + predicates.toString();
        } else {
            predicatesString += std::format(" AND {}", predicates.toString());
        }
    }

    std::string q = query;
    size_t pos = q.find("{}");
    if (pos != std::string::npos) {
        q.replace(pos, 2, columns);
    }
    q += predicatesString;
    q += getOrderBy();
    if (getLimitNum() != common::INVALID_ROW_IDX) {
        q += std::format(" LIMIT {}", getLimitNum());
    }
    return q;
}

std::string PgClientScanBindData::getDescription() const {
    return getSQL();
}

struct PgClientScanFunction {
    static constexpr char PG_CLIENT_SCAN_FUNC_NAME[] = "pg_client_scan";

    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output);

    static std::unique_ptr<TableFuncBindData> bindFunc(
        std::shared_ptr<PgClientTableScanInfo> bindData, main::ClientContext* /*context*/,
        const TableFuncBindInput* input);

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput& input);

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput& input);
};

std::unique_ptr<TableFuncSharedState> PgClientScanFunction::initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto* rawBindData = input.bindData;
    if (!rawBindData) {
        throw RuntimeException("PgClientScanFunction::initSharedState: bindData is null");
    }
    auto scanBindData = rawBindData->constPtrCast<PgClientScanBindData>();
    if (!scanBindData) {
        throw RuntimeException("PgClientScanFunction::initSharedState: cast failed");
    }
    auto sql = scanBindData->getSQL();
    if (sql.empty()) {
        throw RuntimeException("PgClientScanFunction::initSharedState: SQL is empty");
    }
    auto result = scanBindData->connector.executeQuery(sql);
    if (!result.success) {
        throw RuntimeException(
            std::format("Failed to execute query due to error: {}", result.errorMessage));
    }
    return std::make_unique<PgClientScanSharedState>(std::move(result));
}

std::unique_ptr<TableFuncLocalState> PgClientScanFunction::initLocalState(
    const TableFuncInitLocalStateInput&) {
    return std::make_unique<TableFuncLocalState>();
}

static void setValueFromCell(ValueVector* vector, uint32_t pos,
    const PgClientCell& cell, const LogicalType& type) {
    if (cell.isNull) {
        vector->setNull(pos, true);
        return;
    }

    switch (type.getLogicalTypeID()) {
    case LogicalTypeID::BOOL: {
        auto upper = cell.value;
        StringUtils::toUpper(upper);
        bool val = (upper == "TRUE" || upper == "T" || upper == "YES" || upper == "1");
        vector->setValue<uint8_t>(pos, val ? 1 : 0);
        break;
    }
    case LogicalTypeID::INT64: {
        int64_t val = std::stoll(cell.value);
        vector->setValue<int64_t>(pos, val);
        break;
    }
    case LogicalTypeID::INT32: {
        int32_t val = std::stoi(cell.value);
        vector->setValue<int32_t>(pos, val);
        break;
    }
    case LogicalTypeID::INT16: {
        int16_t val = static_cast<int16_t>(std::stoi(cell.value));
        vector->setValue<int16_t>(pos, val);
        break;
    }
    case LogicalTypeID::FLOAT: {
        float val = std::stof(cell.value);
        vector->setValue<float>(pos, val);
        break;
    }
    case LogicalTypeID::DOUBLE: {
        double val = std::stod(cell.value);
        vector->setValue<double>(pos, val);
        break;
    }
    case LogicalTypeID::STRING:
    case LogicalTypeID::BLOB:
    case LogicalTypeID::UUID: {
        StringVector::addString(vector, pos, cell.value);
        break;
    }
    case LogicalTypeID::DATE: {
        // Parse date string (YYYY-MM-DD) to days since epoch
        // For now, use string representation which Ladybug can parse
        StringVector::addString(vector, pos, cell.value);
        break;
    }
    case LogicalTypeID::TIMESTAMP:
    case LogicalTypeID::TIMESTAMP_MS:
    case LogicalTypeID::TIMESTAMP_NS:
    case LogicalTypeID::TIMESTAMP_SEC:
    case LogicalTypeID::TIMESTAMP_TZ: {
        StringVector::addString(vector, pos, cell.value);
        break;
    }
    case LogicalTypeID::INTERVAL: {
        StringVector::addString(vector, pos, cell.value);
        break;
    }
    default: {
        StringVector::addString(vector, pos, cell.value);
        break;
    }
    }
}

static void buildColumnNameToIndexMap(const std::vector<std::string>& columnNames,
    std::unordered_map<std::string, int>& nameToIdx) {
    nameToIdx.clear();
    for (int i = 0; i < (int)columnNames.size(); i++) {
        nameToIdx[columnNames[i]] = i;
    }
}

offset_t PgClientScanFunction::tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    if (!input.sharedState) {
        throw RuntimeException("tableFunc: sharedState is null");
    }
    if (!input.bindData) {
        throw RuntimeException("tableFunc: bindData is null");
    }
    auto pgClientScanSharedState = input.sharedState->ptrCast<PgClientScanSharedState>();
    auto pgClientScanBindData = input.bindData->constPtrCast<PgClientScanBindData>();
    auto& queryResult = pgClientScanSharedState->queryResult;

    // Atomically claim a morsel (slice of [startOffset, endOffset)) from the
    // shared query result. The shared state is owned by the ForeignRelTable
    // and shared across worker threads, so we must serialize offset updates
    // — otherwise parallel scans (e.g. the parallel hash join that drives
    // MATCH ... -[...]-> ... queries) would hand out overlapping rows and
    // miss others. TableFuncSharedState provides the mutex for exactly this
    // purpose.
    uint64_t startOffset = 0;
    uint64_t batchSize = 0;
    {
        std::lock_guard<std::mutex> lk{pgClientScanSharedState->mtx};
        if (pgClientScanSharedState->currentOffset >= queryResult.rows.size()) {
            return 0;
        }
        uint64_t remainingRows = queryResult.rows.size() - pgClientScanSharedState->currentOffset;
        batchSize = std::min<uint64_t>(remainingRows, DEFAULT_VECTOR_CAPACITY);
        startOffset = pgClientScanSharedState->currentOffset;
        pgClientScanSharedState->currentOffset += batchSize;
    }

    auto& dataChunk = output.dataChunk;
    auto numColumns = dataChunk.getNumValueVectors();
    auto& selVector = dataChunk.state->getSelVectorUnsafe();
    selVector.setSelSize(batchSize);

    // The output may have more columns than PG result columns:
    // Column 0 = internal ID (synthesized), columns 1+ = actual data
    // This happens when a scan involves a nodeUniqueName (e.g. MATCH queries).
    // `columnNamesInPg` only contains actual PG column names (no rowid).
    bool hasInternalId = numColumns > pgClientScanBindData->columnNamesInPg.size();

    // Build a name-to-index map for the result columns
    std::unordered_map<std::string, int> resultColMap;
    buildColumnNameToIndexMap(queryResult.columnNames, resultColMap);

    for (auto colIdx = 0u; colIdx < numColumns; colIdx++) {
        auto& vector = dataChunk.getValueVectorMutable(colIdx);

        if (hasInternalId && colIdx == 0) {
            // Internal ID column - synthesize from row index
            for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
                vector.setValue<int64_t>(rowIdx, startOffset + rowIdx);
            }
            continue;
        }

        // Determine which PG result column this output column maps to
        int pgColIdx = hasInternalId ? (int)colIdx - 1 : (int)colIdx;
        if (pgColIdx < 0 || pgColIdx >= (int)pgClientScanBindData->columnNamesInPg.size()) {
            for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
                vector.setNull(rowIdx, true);
            }
            continue;
        }

        std::string targetColName = pgClientScanBindData->columnNamesInPg[pgColIdx];

        // Find this column in the result
        auto it = resultColMap.find(targetColName);
        if (it == resultColMap.end() ||
            it->second >= (int)queryResult.columnNames.size()) {
            for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
                vector.setNull(rowIdx, true);
            }
            continue;
        }

        int resultColIdx = it->second;

        // Copy data from result to vector, respecting the current offset
        for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
            uint64_t srcRowIdx = startOffset + rowIdx;
            setValueFromCell(&vector, rowIdx,
                queryResult.rows[srcRowIdx].cells[resultColIdx], vector.dataType);
        }
    }

    return batchSize;
}

std::unique_ptr<TableFuncBindData> PgClientScanFunction::bindFunc(
    std::shared_ptr<PgClientTableScanInfo> scanInfo, main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto columnNames =
        TableFunction::extractYieldVariables(scanInfo->getColumnNames(), input->yieldVariables);
    auto columns = input->binder->createVariables(columnNames, scanInfo->getColumnTypes(*context));
    return std::make_unique<PgClientScanBindData>(scanInfo->getTemplateQuery(*context),
        scanInfo->getColumnNames(), scanInfo->getConnector(), std::move(columns));
}

TableFunction getScanFunction(std::shared_ptr<PgClientTableScanInfo> scanInfo) {
    auto function =
        TableFunction(PgClientScanFunction::PG_CLIENT_SCAN_FUNC_NAME, std::vector<LogicalTypeID>{});
    function.tableFunc = PgClientScanFunction::tableFunc;
    function.bindFunc = std::bind(PgClientScanFunction::bindFunc, scanInfo,
        std::placeholders::_1, std::placeholders::_2);
    function.initSharedStateFunc = PgClientScanFunction::initSharedState;
    function.initLocalStateFunc = PgClientScanFunction::initLocalState;
    function.supportsPushDownFunc = [] { return true; };
    return function;
}

} // namespace pg_client_extension
} // namespace lbug
