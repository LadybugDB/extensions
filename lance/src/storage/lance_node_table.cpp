#include "storage/lance_node_table.h"

#include <algorithm>
#include <iostream>
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
          std::make_unique<LanceNodeTableScanSharedState>()} {
    const auto& storage = nodeTableEntry->getStorage();
    datasetPath = common::VirtualFileSystem::resolvePath(context,
        LanceUtils::constructNodeTablePath(storage, nodeTableEntry->getName()));
    // Object-store datasets are validated lazily at scan time (credentials may only be
    // configured later); local datasets fail fast here, mirroring icebug-disk.
    if (common::LocalFileSystem::isLocalPath(datasetPath)) {
        ensureDataset(context);
        checkCompatibility(context);
    }
}

std::shared_ptr<LanceDataset> LanceNodeTable::ensureDataset(main::ClientContext* context) const {
    (void)context;
    if (dataset) {
        return dataset;
    }
    std::lock_guard lock{datasetMutex};
    if (!dataset) {
        dataset = std::make_shared<LanceDataset>(datasetPath);
        datasetSchema = dataset->schema();
    }
    return dataset;
}

void LanceNodeTable::checkCompatibility(main::ClientContext* context) const {
    // Enforce the stamped schema version. Datasets without the marker predate
    // versioning and keep working with a warning, mirroring icebug-disk.
    const auto version = dataset->versionString();
    if (version.empty()) {
        std::cerr << "Warning: " << datasetPath
                  << ": Lance dataset is missing lance_version schema metadata; continuing "
                     "without version compatibility check"
                  << std::endl;
    } else if (version != LanceConstants::CURRENT_VERSION) {
        throw RuntimeException{datasetPath + ": current ladybug version does not support " +
                               "lance_version: " + version};
    }
    // The primary key column must exist: node offsets are positional, but every filter
    // and lookup addresses nodes through the PK.
    const auto pkName = nodeTableCatalogEntry->getPrimaryKeyName();
    const auto pkIt = std::find_if(datasetSchema.begin(), datasetSchema.end(),
        [&](const LanceDatasetColumn& col) { return col.name == pkName; });
    if (pkIt == datasetSchema.end()) {
        throw RuntimeException{"Lance dataset '" + datasetPath + "' has no primary key column '" +
                               pkName + "' for node table '" + nodeTableCatalogEntry->getName() +
                               "'"};
    }
    // Declared properties present in the dataset must match exactly: values cross
    // the FFI with dataset types, so any coercion would silently mistype them.
    // Missing properties read as NULL, mirroring icebug-disk.
    for (auto i = 0u; i < nodeTableCatalogEntry->getNumProperties(); ++i) {
        const auto& prop = nodeTableCatalogEntry->getProperty(i);
        auto it = std::find_if(datasetSchema.begin(), datasetSchema.end(),
            [&](const LanceDatasetColumn& col) { return col.name == prop.getName(); });
        if (it == datasetSchema.end()) {
            continue;
        }
        if (it->ladybugType != prop.getType()) {
            throw RuntimeException{"Lance dataset '" + datasetPath + "' column '" + it->name +
                                   "' has type " + it->ladybugType.toString() +
                                   " but node table '" + nodeTableCatalogEntry->getName() +
                                   "' declares " + prop.getType().toString()};
        }
    }
    (void)context;
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
    auto lanceDataset = ensureDataset(context);
    if (!scanState.reader) {
        // First init for this scan state: snapshot the requested columns. Columns are
        // matched by name; dataset types were validated against the catalog at CREATE.
        std::vector<LanceReader::ProjectedColumn> projection;
        std::vector<column_id_t> readerColumnIDs;
        for (auto columnID : scanState.columnIDs) {
            if (columnID == INVALID_COLUMN_ID || columnID == ROW_IDX_COLUMN_ID) {
                continue;
            }
            std::string propName;
            for (auto i = 0u; i < nodeTableCatalogEntry->getNumProperties(); ++i) {
                if (nodeTableCatalogEntry->getColumnID(i) == columnID) {
                    propName = nodeTableCatalogEntry->getProperty(i).getName();
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
            projection.push_back(LanceReader::ProjectedColumn{it->name, it->ladybugType.copy()});
            readerColumnIDs.push_back(columnID);
        }
        if (projection.empty()) {
            // No property columns requested: surrogate projection over the first dataset
            // column; values are discarded and only the row count is used.
            if (datasetSchema.empty()) {
                throw RuntimeException{"Lance dataset '" + datasetPath + "' has no columns"};
            }
            projection.push_back(LanceReader::ProjectedColumn{datasetSchema[0].name,
                datasetSchema[0].ladybugType.copy()});
        }
        scanState.dataset = lanceDataset;
        scanState.reader =
            std::make_unique<LanceReader>(std::move(lanceDataset), std::move(projection));
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

    // Dataset column index per requested output position (surrogate projection when no
    // columns were requested: single temp column, values discarded).
    const bool surrogate = lanceScanState.readerColumnIDs.empty();
    const auto numProjCols =
        surrogate ? 1 : static_cast<uint32_t>(lanceScanState.readerColumnIDs.size());

    while (true) {
        scanState.resetOutVectors();
        DataChunk lanceDataChunk(numProjCols, scanState.outState);
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
        const auto count = static_cast<row_idx_t>(ensureDataset(context)->countRows());
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
    // Lookup joins resolve INTERNAL_ID keys directly to offsets (same fast path as
    // NodeTable::lookupPK). The base implementation checks in-memory node groups,
    // which are empty for disk-resident tables, so handle it here via total row count.
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
