#include "storage/lance_node_table.h"

#include <algorithm>
#include <mutex>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/cast.h"
#include "common/data_chunk/data_chunk.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/file_system/local_file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/lance_constants.h"
#include "storage/lance_utils.h"
#include "storage/storage_manager.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace lance_extension {

LanceNodeTable::LanceNodeTable(const StorageManager* storageManager,
    const NodeTableCatalogEntry* nodeTableEntry, MemoryManager* memoryManager,
    main::ClientContext* context)
    : ColumnarNodeTableBase{storageManager, nodeTableEntry, memoryManager,
          std::make_unique<LanceNodeTableScanSharedState>()},
      connector{std::make_shared<LanceConnector>()} {
    connector->connect("" /* inMemDB */, "" /* defaultCatalogName */, "" /* defaultSchemaName */,
        context);
    const auto& storage = nodeTableEntry->getStorage();
    datasetPath = common::VirtualFileSystem::resolvePath(context,
        LanceUtils::constructNodeTablePath(storage, nodeTableEntry->getName()));
    // Object-store datasets are validated lazily at scan time (credentials may only be
    // configured later); local datasets fail fast here, mirroring icebug-disk.
    if (common::LocalFileSystem::isLocalPath(datasetPath)) {
        ensureSchemaLoaded(context);
        checkCompatibility(context);
    }
}

void LanceNodeTable::ensureSchemaLoaded(main::ClientContext* context) const {
    if (!datasetSchema.empty()) {
        return;
    }
    std::lock_guard lock{schemaMutex};
    if (!datasetSchema.empty()) {
        return;
    }
    datasetSchema = LanceReader::fetchSchema(*connector, context, datasetPath);
}

void LanceNodeTable::checkCompatibility(main::ClientContext* context) const {
    // The primary key column must exist: node offsets are positional, but every filter
    // and lookup addresses nodes through the PK.
    const auto pkName = nodeTableCatalogEntry->getPrimaryKeyName();
    const bool hasPK = std::any_of(datasetSchema.begin(), datasetSchema.end(),
        [&](const LanceDatasetColumn& col) { return col.name == pkName; });
    if (!hasPK) {
        throw RuntimeException{"Lance dataset '" + datasetPath + "' has no primary key column '" +
                               pkName + "' for node table '" + nodeTableCatalogEntry->getName() +
                               "'"};
    }
    // Every catalog property present in the dataset must have a mappable type, and the
    // full projection must bind (trial query over zero rows validates the CASTs).
    std::string selectList;
    for (auto i = 0u; i < nodeTableCatalogEntry->getNumProperties(); ++i) {
        const auto& prop = nodeTableCatalogEntry->getProperty(i);
        auto it = std::find_if(datasetSchema.begin(), datasetSchema.end(),
            [&](const LanceDatasetColumn& col) { return col.name == prop.getName(); });
        if (it == datasetSchema.end()) {
            continue; // Missing properties read as NULL, mirroring icebug-disk.
        }
        const auto spelling = LanceReader::toDuckDBCastSpelling(prop.getType());
        if (!selectList.empty()) {
            selectList += ", ";
        }
        selectList += "CAST(" + LanceConnector::quoteIdent(it->name) + " AS " + spelling + ")";
    }
    if (!selectList.empty()) {
        auto conn = connector->newConnection(context);
        connector->executeOn(*conn, "SELECT " + selectList + " FROM '" +
                                        LanceConnector::escapeLiteral(datasetPath) + "' LIMIT 0");
    }
}

void LanceNodeTable::initializeScanCoordination(const transaction::Transaction* transaction) {
    auto sharedState = static_cast<LanceNodeTableScanSharedState*>(tableScanSharedState.get());
    sharedState->reset(getNumBatches(transaction));
}

node_group_idx_t LanceNodeTable::getNumScanMorsels(const Transaction* transaction) const {
    return getNumBatches(transaction);
}

node_group_idx_t LanceNodeTable::getNumBatches(const Transaction* transaction) const {
    const auto totalRows = getTotalRowCount(transaction);
    if (totalRows == 0) {
        return 1;
    }
    auto morsels =
        (totalRows + LanceConstants::MORSEL_TARGET_ROWS - 1) / LanceConstants::MORSEL_TARGET_ROWS;
    return static_cast<node_group_idx_t>(std::min<uint64_t>(morsels, LanceConstants::MAX_MORSELS));
}

void LanceNodeTable::initReaderForMorsel(Transaction* transaction,
    LanceNodeTableScanState& scanState) const {
    auto context = transaction->getClientContext();
    if (!context) {
        throw RuntimeException("Invalid client context for Lance node scan initialization");
    }
    ensureSchemaLoaded(context);
    if (!scanState.reader) {
        // First init for this scan state: snapshot the requested columns and build the
        // reader projection (dataset column + CAST spelling + catalog type per column).
        std::vector<LanceReader::ProjectedColumn> projection;
        std::vector<column_id_t> readerColumnIDs;
        for (auto columnID : scanState.columnIDs) {
            if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID) {
                continue;
            }
            // Find the catalog property behind this column ID.
            std::string propName;
            const LogicalType* propType = nullptr;
            for (auto i = 0u; i < nodeTableCatalogEntry->getNumProperties(); ++i) {
                if (nodeTableCatalogEntry->getColumnID(i) == columnID) {
                    propName = nodeTableCatalogEntry->getProperty(i).getName();
                    propType = &nodeTableCatalogEntry->getProperty(i).getType();
                    break;
                }
            }
            if (propName.empty()) {
                continue;
            }
            auto it = std::find_if(datasetSchema.begin(), datasetSchema.end(),
                [&](const LanceDatasetColumn& col) { return col.name == propName; });
            if (it == datasetSchema.end()) {
                continue; // Filled with NULL below, mirroring icebug-disk.
            }
            projection.push_back(LanceReader::ProjectedColumn{it->name,
                LanceReader::toDuckDBCastSpelling(*propType), propType->copy()});
            readerColumnIDs.push_back(columnID);
        }
        if (projection.empty()) {
            // No property columns requested: surrogate projection over the first dataset
            // column; values are discarded and only the row count is used.
            if (datasetSchema.empty()) {
                throw RuntimeException{"Lance dataset '" + datasetPath + "' has no columns"};
            }
            projection.push_back(LanceReader::ProjectedColumn{datasetSchema[0].name,
                LanceReader::toDuckDBCastSpelling(datasetSchema[0].ladybugType),
                datasetSchema[0].ladybugType.copy()});
        }
        scanState.reader =
            std::make_unique<LanceReader>(connector, datasetPath, std::move(projection), context);
        scanState.readerColumnIDs = std::move(readerColumnIDs);
    }
    // Pull the next morsel from the shared state (mirrors IceDiskNodeTable: the
    // morsel-driven assignment from nextMorsel is discarded in favor of the table's
    // own counter).
    auto* sharedState = static_cast<LanceNodeTableScanSharedState*>(tableScanSharedState.get());
    node_group_idx_t assignedMorsel = INVALID_NODE_GROUP_IDX;
    if (!sharedState->getNextMorsel(assignedMorsel)) {
        scanState.scanCompleted = true;
        return;
    }
    const auto totalRows = static_cast<uint64_t>(getTotalRowCount(transaction));
    scanState.morselStart =
        static_cast<uint64_t>(assignedMorsel) * LanceConstants::MORSEL_TARGET_ROWS;
    scanState.morselLimit = totalRows > scanState.morselStart ?
                                std::min<uint64_t>(LanceConstants::MORSEL_TARGET_ROWS,
                                    totalRows - scanState.morselStart) :
                                0;
    scanState.morselConsumed = 0;
    scanState.reader->openRange(scanState.morselStart, scanState.morselLimit);
}

void LanceNodeTable::initScanState(Transaction* transaction, TableScanState& scanState,
    [[maybe_unused]] bool resetCachedBoundNodeSelVec) const {
    auto& nodeScanState = scanState.cast<NodeTableScanState>();
    nodeScanState.source = TableScanSource::COMMITTED;
    auto& lanceScanState = common::dynamic_cast_checked<LanceNodeTableScanState&>(nodeScanState);
    lanceScanState.scanCompleted = false;
    lanceScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;
    initReaderForMorsel(transaction, lanceScanState);
}

bool LanceNodeTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& lanceScanState = common::dynamic_cast_checked<LanceNodeTableScanState&>(scanState);
    if (lanceScanState.scanCompleted || !lanceScanState.reader) {
        return false;
    }
    auto context = transaction->getClientContext();
    auto memoryManager = MemoryManager::Get(*context);

    // Dataset column index per requested output position (INVALID_COLUMN_ID = NULL fill).
    // Surrogate projection (no columns requested): single temp column, values discarded.
    const bool surrogate = lanceScanState.readerColumnIDs.empty();
    const auto numProjCols =
        surrogate ? 1 : static_cast<uint32_t>(lanceScanState.readerColumnIDs.size());

    while (true) {
        scanState.resetOutVectors();
        DataChunk lanceDataChunk(numProjCols, scanState.outState);
        // NOTE: projection column types are snapshotted from the reader below.
        auto projTypes = lanceScanState.reader->getProjectionTypes();
        for (auto i = 0u; i < numProjCols; ++i) {
            lanceDataChunk.insert(i,
                std::make_shared<ValueVector>(projTypes[i].copy(), memoryManager));
        }
        lanceDataChunk.state->getSelVectorUnsafe().setToFiltered(0);
        const auto rowsRead = lanceScanState.reader->next(lanceDataChunk);
        if (rowsRead == 0) {
            lanceScanState.scanCompleted = true;
            return false;
        }
        lanceDataChunk.state->getSelVectorUnsafe().setToUnfiltered(rowsRead);

        const auto startOffset =
            static_cast<offset_t>(lanceScanState.morselStart + lanceScanState.morselConsumed);
        lanceScanState.morselConsumed += rowsRead;

        NodeTable::applySemiMaskFilter(scanState, startOffset, rowsRead,
            lanceDataChunk.state->getSelVectorUnsafe());
        const auto selSize = lanceDataChunk.state->getSelVector().getSelSize();
        if (selSize == 0) {
            continue;
        }

        const auto selectedPositions = lanceDataChunk.state->getSelVector().getSelectedPositions();
        if (!surrogate) {
            // Map requested output columns to projection positions by catalog column ID.
            std::vector<size_t> outputToProj(scanState.outputVectors.size(), INVALID_COLUMN_ID);
            for (size_t outCol = 0; outCol < scanState.outputVectors.size(); ++outCol) {
                for (size_t projCol = 0; projCol < lanceScanState.readerColumnIDs.size();
                     ++projCol) {
                    if (scanState.columnIDs[outCol] == lanceScanState.readerColumnIDs[projCol]) {
                        outputToProj[outCol] = projCol;
                        break;
                    }
                }
            }
            for (size_t outCol = 0; outCol < scanState.outputVectors.size(); ++outCol) {
                auto* dstVector = scanState.outputVectors[outCol];
                if (!dstVector) {
                    continue;
                }
                auto projCol = outputToProj[outCol];
                if (projCol == INVALID_COLUMN_ID ||
                    projCol >= lanceDataChunk.getNumValueVectors()) {
                    for (size_t row = 0; row < selSize; ++row) {
                        dstVector->setNull(row, true);
                    }
                    continue;
                }
                auto& srcVector = lanceDataChunk.getValueVector(projCol);
                size_t row = 0;
                for (auto sourceRow : selectedPositions) {
                    dstVector->copyFromVectorData(row++, &srcVector, sourceRow);
                }
            }
        }

        auto tableID = this->getTableID();
        size_t row = 0;
        for (auto sourceRow : selectedPositions) {
            auto& nodeID = scanState.nodeIDVector->getValue<nodeID_t>(row++);
            nodeID.tableID = tableID;
            nodeID.offset = startOffset + sourceRow;
        }
        scanState.outState->getSelVectorUnsafe().setToUnfiltered(selSize);
        return true;
    }
}

row_idx_t LanceNodeTable::getTotalRowCount(const Transaction* transaction) const {
    const auto cached = cachedRowCount.load(std::memory_order_relaxed);
    if (cached != INVALID_ROW_IDX) {
        return cached;
    }
    auto context = transaction->getClientContext();
    if (!context) {
        return 0;
    }
    try {
        const auto count =
            static_cast<row_idx_t>(LanceReader::countRows(*connector, context, datasetPath));
        cachedRowCount.store(count, std::memory_order_relaxed);
        return count;
    } catch (const std::exception&) {
        return 0;
    }
}

bool LanceNodeTable::isVisible(const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < getTotalRowCount(transaction);
}

bool LanceNodeTable::isVisibleNoLock(const transaction::Transaction* transaction,
    common::offset_t offset) const {
    return offset < getTotalRowCount(transaction);
}

bool LanceNodeTable::lookupPK(const transaction::Transaction* transaction,
    common::ValueVector* keyVector, uint64_t vectorPos, common::offset_t& result) const {
    // Same fast path as IceDiskNodeTable: INTERNAL_ID keys resolve directly to offsets.
    if (keyVector->dataType.getLogicalTypeID() == common::LogicalTypeID::INTERNAL_ID) {
        if (keyVector->isNull(vectorPos)) {
            return false;
        }
        const auto nodeID = keyVector->getValue<common::nodeID_t>(vectorPos);
        if (nodeID.tableID != tableID || nodeID.offset == common::INVALID_OFFSET) {
            return false;
        }
        if (!isVisibleNoLock(transaction, nodeID.offset)) {
            return false;
        }
        result = nodeID.offset;
        return true;
    }
    return NodeTable::lookupPK(transaction, keyVector, vectorPos, result);
}

} // namespace lance_extension
} // namespace lbug
