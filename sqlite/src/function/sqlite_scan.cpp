#include "function/sqlite_scan.h"

#include <algorithm>
#include <unordered_set>

#include "binder/binder.h"
#include "common/constants.h"
#include "common/exception/exception.h"
#include "common/exception/runtime.h"
#include "common/string_utils.h"
#include "common/system_config.h"
#include "common/types/date_t.h"
#include "common/types/timestamp_t.h"
#include "common/vector/value_vector.h"
#include "connector/sqlite_connector.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "main/client_context.h"
#include "processor/execution_context.h"
#include <format>

using namespace lbug::function;
using namespace lbug::common;

namespace lbug {
namespace sqlite_extension {

std::vector<common::LogicalType> SqliteTableScanInfo::getColumnTypes(
    const main::ClientContext& context) const {
    if (context.getCurrentSetting("sqlite_all_varchar").getValue<bool>()) {
        std::vector<common::LogicalType> allVarchar;
        allVarchar.reserve(columnTypes.size());
        for (auto i = 0u; i < columnTypes.size(); i++) {
            allVarchar.push_back(common::LogicalType::STRING());
        }
        return allVarchar;
    }
    std::vector<common::LogicalType> result;
    result.reserve(columnTypes.size());
    for (auto& type : columnTypes) {
        result.push_back(type.copy());
    }
    return result;
}

bool SqliteScanBindData::hasInternalIDColumn() const {
    if (columnNamesInSqlite.size() >= columns.size()) {
        return false;
    }
    return !columns.empty() &&
           columns[0]->getUniqueName().ends_with(std::format(".{}", InternalKeyword::ID));
}

std::vector<uint32_t> SqliteScanBindData::getColumnIndicesToSelect() const {
    auto columnSkips = getColumnSkips();
    auto numSkippedColumns =
        std::count_if(columnSkips.begin(), columnSkips.end(), [](auto item) { return item; });
    if (getNumColumns() == numSkippedColumns) {
        return {0};
    }
    std::vector<uint32_t> result;
    for (auto i = 0u; i < getNumColumns(); i++) {
        // Always include the synthesized internal ID column even if marked
        // as skipped, so output positions stay aligned with bound columns.
        if (columnSkips[i] && !(i == 0 && hasInternalIDColumn())) {
            continue;
        }
        result.push_back(i);
    }
    return result;
}

uint32_t SqliteScanBindData::boundIdxToSqliteIdx(uint32_t boundIdx) const {
    return hasInternalIDColumn() ? boundIdx - 1 : boundIdx;
}

std::string SqliteScanBindData::getSQL() const {
    std::string columns;
    bool first = true;
    for (auto boundIdx : getColumnIndicesToSelect()) {
        if (hasInternalIDColumn() && boundIdx == 0) {
            continue;
        }
        if (!first) {
            columns += ",";
        }
        // Spliced raw by design: for pushed-down join queries these entries
        // are full `expr AS alias` select items, not plain identifiers
        // (same contract as the DuckDB/pg_client scans).
        columns += columnNamesInSqlite[boundIdxToSqliteIdx(boundIdx)];
        first = false;
    }
    if (first) {
        // Every column was skipped: still select one so the query is valid.
        // Its values are discarded by the scan.
        columns = SqliteConnector::quoteIdentifier(columnNamesInSqlite[0]);
    }

    std::string predicatesString;
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

std::string SqliteScanBindData::getDescription() const {
    return getSQL();
}

struct SqliteScanFunction {
    static constexpr char SQLITE_SCAN_FUNC_NAME[] = "sqlite_scan";

    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output);

    static std::unique_ptr<TableFuncBindData> bindFunc(
        std::shared_ptr<SqliteTableScanInfo> bindData, main::ClientContext* context,
        const TableFuncBindInput* input);

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput& input);

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput& input);
};

std::unique_ptr<TableFuncSharedState> SqliteScanFunction::initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto scanBindData = input.bindData->constPtrCast<SqliteScanBindData>();
    auto result = scanBindData->connector.executeQuery(scanBindData->getSQL());
    if (!result.success) {
        throw RuntimeException(
            std::format("Failed to execute SQLite query due to error: {}", result.errorMessage));
    }
    return std::make_unique<SqliteScanSharedState>(std::move(result));
}

std::unique_ptr<TableFuncLocalState> SqliteScanFunction::initLocalState(
    const TableFuncInitLocalStateInput&) {
    return std::make_unique<TableFuncLocalState>();
}

// SQLite is dynamically typed: any storage class can appear in any column.
// Reads enforce the column's declared type strictly so that, e.g., TEXT in an
// INTEGER column surfaces as a Mismatch Type Error instead of silently
// producing garbage -- matching the previous DuckDB-backed behaviour.
static void throwMismatch(const std::string& column, const std::string& declared,
    const SqliteCell& cell) {
    std::string found = cell.textVal;
    if (cell.storage == SqliteStorageClass::BLOB) {
        found = std::format("<{} bytes>", cell.blobVal.size());
    } else if (cell.storage == SqliteStorageClass::NULL_VALUE) {
        found = "NULL";
    }
    throw Exception(std::format("Mismatch Type Error: Invalid type in column \"{}\": column was "
                                "declared as {}, found \"{}\" of type \"{}\" instead.",
        column, declared, found, sqliteStorageClassToString(cell.storage)));
}

static void setValueFromCell(ValueVector* vector, uint32_t pos, const std::string& columnName,
    const SqliteCell& cell) {
    if (cell.storage == SqliteStorageClass::NULL_VALUE) {
        vector->setNull(pos, true);
        return;
    }
    switch (vector->dataType.getLogicalTypeID()) {
    case LogicalTypeID::BOOL: {
        if (cell.storage != SqliteStorageClass::INTEGER) {
            throwMismatch(columnName, "boolean", cell);
        }
        vector->setValue<uint8_t>(pos, cell.intVal ? 1 : 0);
    } break;
    case LogicalTypeID::INT64: {
        if (cell.storage != SqliteStorageClass::INTEGER) {
            throwMismatch(columnName, "integer", cell);
        }
        vector->setValue<int64_t>(pos, cell.intVal);
    } break;
    case LogicalTypeID::INT32: {
        if (cell.storage != SqliteStorageClass::INTEGER) {
            throwMismatch(columnName, "integer", cell);
        }
        vector->setValue<int32_t>(pos, static_cast<int32_t>(cell.intVal));
    } break;
    case LogicalTypeID::INT16: {
        if (cell.storage != SqliteStorageClass::INTEGER) {
            throwMismatch(columnName, "integer", cell);
        }
        vector->setValue<int16_t>(pos, static_cast<int16_t>(cell.intVal));
    } break;
    case LogicalTypeID::INT8: {
        if (cell.storage != SqliteStorageClass::INTEGER) {
            throwMismatch(columnName, "integer", cell);
        }
        vector->setValue<int8_t>(pos, static_cast<int8_t>(cell.intVal));
    } break;
    case LogicalTypeID::UINT64:
    case LogicalTypeID::UINT32:
    case LogicalTypeID::UINT16:
    case LogicalTypeID::UINT8: {
        if (cell.storage != SqliteStorageClass::INTEGER) {
            throwMismatch(columnName, "integer", cell);
        }
        vector->setValue<int64_t>(pos, cell.intVal);
    } break;
    case LogicalTypeID::FLOAT: {
        if (cell.storage == SqliteStorageClass::INTEGER) {
            vector->setValue<float>(pos, static_cast<float>(cell.intVal));
        } else if (cell.storage == SqliteStorageClass::REAL) {
            vector->setValue<float>(pos, static_cast<float>(cell.realVal));
        } else {
            throwMismatch(columnName, "double", cell);
        }
    } break;
    case LogicalTypeID::DOUBLE: {
        if (cell.storage == SqliteStorageClass::INTEGER) {
            vector->setValue<double>(pos, static_cast<double>(cell.intVal));
        } else if (cell.storage == SqliteStorageClass::REAL) {
            vector->setValue<double>(pos, cell.realVal);
        } else {
            throwMismatch(columnName, "double", cell);
        }
    } break;
    case LogicalTypeID::STRING: {
        if (cell.storage == SqliteStorageClass::BLOB) {
            StringVector::addString(vector, pos, reinterpret_cast<const char*>(cell.blobVal.data()),
                cell.blobVal.size());
        } else {
            StringVector::addString(vector, pos, cell.textVal);
        }
    } break;
    case LogicalTypeID::BLOB: {
        if (cell.storage == SqliteStorageClass::BLOB) {
            BlobVector::addBlob(vector, pos, cell.blobVal.data(), cell.blobVal.size());
        } else if (cell.storage == SqliteStorageClass::TEXT) {
            BlobVector::addBlob(vector, pos, cell.textVal.data(), cell.textVal.size());
        } else {
            throwMismatch(columnName, "blob", cell);
        }
    } break;
    case LogicalTypeID::DATE: {
        if (cell.storage != SqliteStorageClass::TEXT) {
            throwMismatch(columnName, "date", cell);
        }
        date_t date{};
        uint64_t convertPos = 0;
        if (!Date::tryConvertDate(cell.textVal.data(), cell.textVal.size(), convertPos, date) ||
            convertPos != cell.textVal.size()) {
            throw Exception(std::format("Conversion Error: Could not convert string \"{}\" to DATE",
                cell.textVal));
        }
        vector->setValue<date_t>(pos, date);
    } break;
    case LogicalTypeID::TIMESTAMP:
    case LogicalTypeID::TIMESTAMP_MS:
    case LogicalTypeID::TIMESTAMP_NS:
    case LogicalTypeID::TIMESTAMP_SEC:
    case LogicalTypeID::TIMESTAMP_TZ: {
        if (cell.storage != SqliteStorageClass::TEXT) {
            throwMismatch(columnName, "timestamp", cell);
        }
        timestamp_t timestamp{};
        if (!Timestamp::tryConvertTimestamp(cell.textVal.data(), cell.textVal.size(), timestamp)) {
            throw Exception(std::format("Conversion Error: Could not convert string \"{}\" to "
                                        "TIMESTAMP",
                cell.textVal));
        }
        vector->setValue<timestamp_t>(pos, timestamp);
    } break;
    case LogicalTypeID::UUID: {
        StringVector::addString(vector, pos, cell.textVal);
    } break;
    default: {
        StringVector::addString(vector, pos, cell.textVal);
    } break;
    }
}

offset_t SqliteScanFunction::tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
    auto sqliteScanSharedState = input.sharedState->ptrCast<SqliteScanSharedState>();
    auto sqliteScanBindData = input.bindData->constPtrCast<SqliteScanBindData>();
    auto& queryResult = sqliteScanSharedState->queryResult;

    // Atomically claim a morsel of the materialized result. The shared state
    // is shared across worker threads, so offset updates are serialized with
    // the shared-state mutex (same contract as the pg_client scan).
    uint64_t startOffset = 0;
    uint64_t batchSize = 0;
    {
        std::lock_guard<std::mutex> lk{sqliteScanSharedState->mtx};
        if (sqliteScanSharedState->currentOffset >= queryResult.rows.size()) {
            return 0;
        }
        uint64_t remainingRows = queryResult.rows.size() - sqliteScanSharedState->currentOffset;
        batchSize = std::min<uint64_t>(remainingRows, DEFAULT_VECTOR_CAPACITY);
        startOffset = sqliteScanSharedState->currentOffset;
        sqliteScanSharedState->currentOffset += batchSize;
    }

    auto& dataChunk = output.dataChunk;
    auto numColumns = dataChunk.getNumValueVectors();
    auto& selVector = dataChunk.state->getSelVectorUnsafe();
    selVector.setSelSize(batchSize);

    bool hasInternalId = sqliteScanBindData->hasInternalIDColumn();
    // The planner may compact the output to the non-skipped columns only;
    // detect that by comparing vector counts, mirroring the DuckDB scan.
    bool compactOutput = numColumns < sqliteScanBindData->getNumColumns();
    auto selected = sqliteScanBindData->getColumnIndicesToSelect();

    // SQLite result columns arrive in SELECT-list order, which matches the
    // selected bound indices (minus the synthesized internal ID).
    size_t resultPos = 0;
    size_t compactPos = 0;
    std::unordered_set<uint32_t> selectedSet(selected.begin(), selected.end());
    for (auto boundIdx : selected) {
        size_t outIdx = compactOutput ? compactPos++ : boundIdx;
        if (outIdx >= numColumns) {
            if (!(hasInternalId && boundIdx == 0)) {
                resultPos++;
            }
            continue;
        }
        auto& vector = dataChunk.getValueVectorMutable(outIdx);
        if (hasInternalId && boundIdx == 0) {
            for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
                vector.setValue<int64_t>(rowIdx, startOffset + rowIdx);
            }
            continue;
        }
        uint32_t sqliteIdx = sqliteScanBindData->boundIdxToSqliteIdx(boundIdx);
        const std::string& targetColName = sqliteScanBindData->columnNamesInSqlite[sqliteIdx];
        size_t resultColIdx = resultPos++;
        if (resultColIdx >= queryResult.columnNames.size()) {
            for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
                vector.setNull(rowIdx, true);
            }
            continue;
        }
        for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
            uint64_t srcRowIdx = startOffset + rowIdx;
            setValueFromCell(&vector, rowIdx, targetColName,
                queryResult.rows[srcRowIdx].cells[resultColIdx]);
        }
    }
    if (!compactOutput) {
        // Null-fill bound columns the planner skipped; they still occupy
        // output positions in the non-compact layout.
        for (auto i = 0u; i < sqliteScanBindData->getNumColumns() && i < numColumns; i++) {
            if (selectedSet.contains(i)) {
                continue;
            }
            auto& vector = dataChunk.getValueVectorMutable(i);
            for (auto rowIdx = 0u; rowIdx < batchSize; rowIdx++) {
                vector.setNull(rowIdx, true);
            }
        }
    }

    return batchSize;
}

std::unique_ptr<TableFuncBindData> SqliteScanFunction::bindFunc(
    std::shared_ptr<SqliteTableScanInfo> scanInfo, main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto columnNames =
        TableFunction::extractYieldVariables(scanInfo->getColumnNames(), input->yieldVariables);
    auto columns = input->binder->createVariables(columnNames, scanInfo->getColumnTypes(*context));
    return std::make_unique<SqliteScanBindData>(scanInfo->getTemplateQuery(*context),
        scanInfo->getColumnNames(), scanInfo->getConnector(), std::move(columns));
}

TableFunction getScanFunction(std::shared_ptr<SqliteTableScanInfo> scanInfo) {
    auto function =
        TableFunction(SqliteScanFunction::SQLITE_SCAN_FUNC_NAME, std::vector<LogicalTypeID>{});
    function.tableFunc = SqliteScanFunction::tableFunc;
    function.bindFunc = std::bind(SqliteScanFunction::bindFunc, scanInfo, std::placeholders::_1,
        std::placeholders::_2);
    function.initSharedStateFunc = SqliteScanFunction::initSharedState;
    function.initLocalStateFunc = SqliteScanFunction::initLocalState;
    function.supportsPushDownFunc = [] { return true; };
    return function;
}

} // namespace sqlite_extension
} // namespace lbug
