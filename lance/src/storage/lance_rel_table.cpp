#include "storage/lance_rel_table.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/assert.h"
#include "common/cast.h"
#include "common/data_chunk/data_chunk.h"
#include "common/data_chunk/sel_vector.h"
#include "common/exception/runtime.h"
#include "common/file_system/local_file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "common/string_utils.h"
#include "common/vector/value_vector.h"
#include "main/client_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/lance_utils.h"
#include "storage/storage_manager.h"
#include "storage/table/csr_node_group.h"
#include "transaction/transaction.h"

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace lance_extension {

LanceRelTable::LanceRelTable(RelGroupCatalogEntry* relGroupEntry, table_id_t fromTableID,
    table_id_t toTableID, const StorageManager* storageManager, MemoryManager* memoryManager,
    main::ClientContext* context)
    : ColumnarRelTableBase{relGroupEntry, fromTableID, toTableID, storageManager, memoryManager},
      layout{LanceRelTableLayout::CSR} {
    const auto& storage = relGroupEntry->getStorage();
    if (LanceUtils::isSingleDatasetPath(storage)) {
        layout = LanceRelTableLayout::FLAT;
        indicesDatasetPath = VirtualFileSystem::resolvePath(context, storage);
    } else {
        auto paths = LanceUtils::constructCSRPaths(storage, relGroupEntry->getName());
        indicesDatasetPath = VirtualFileSystem::resolvePath(context, paths.indices);
        indptrDatasetPath = VirtualFileSystem::resolvePath(context, paths.indptr);
    }
    // Object-store datasets are validated lazily at scan time; local datasets fail fast
    // here, mirroring icebug-disk.
    if (common::LocalFileSystem::isLocalPath(indicesDatasetPath)) {
        ensureDataset(context);
        checkCompatibility(context);
    }
}

std::shared_ptr<LanceDataset> LanceRelTable::ensureDataset(main::ClientContext* context) const {
    (void)context;
    if (dataset) {
        return dataset;
    }
    std::lock_guard lock{datasetMutex};
    if (!dataset) {
        dataset = std::make_shared<LanceDataset>(indicesDatasetPath);
        indicesSchema = dataset->schema();
    }
    return dataset;
}

static void requireExactType(const std::string& datasetPath, const std::string& tableName,
    const std::string& columnName, const LogicalType& datasetType, const LogicalType& catalogType) {
    if (datasetType != catalogType) {
        throw RuntimeException{"Lance dataset '" + datasetPath + "' column '" + columnName +
                               "' has type " + datasetType.toString() + " but table '" + tableName +
                               "' declares " + catalogType.toString()};
    }
}

void LanceRelTable::checkCompatibility(main::ClientContext* context) const {
    const auto version = dataset->versionString();
    if (version.empty()) {
        std::cerr << "Warning: " << indicesDatasetPath
                  << ": Lance dataset is missing lance_version schema metadata; continuing "
                     "without version compatibility check"
                  << std::endl;
    } else if (version != LanceConstants::CURRENT_VERSION) {
        throw RuntimeException{
            indicesDatasetPath +
            ": current ladybug version does not support lance_version: " + version};
    }
    if (indicesSchema.empty()) {
        throw RuntimeException{"Lance dataset '" + indicesDatasetPath + "' has no columns"};
    }
    const auto& tableName = relGroupEntry->getName();
    if (layout == LanceRelTableLayout::FLAT) {
        if (indicesSchema.size() < 2) {
            throw RuntimeException{"Flat Lance rel dataset '" + indicesDatasetPath +
                                   "' requires source and target offset columns"};
        }
        requireExactType(indicesDatasetPath, tableName, indicesSchema[0].name,
            indicesSchema[0].ladybugType, LogicalType::INT64());
        requireExactType(indicesDatasetPath, tableName, indicesSchema[1].name,
            indicesSchema[1].ladybugType, LogicalType::INT64());
    } else {
        // Column 0 is the destination node offset; the indptr dataset holds one
        // integer column with one entry per source node plus a sentinel.
        requireExactType(indicesDatasetPath, tableName, indicesSchema[0].name,
            indicesSchema[0].ladybugType, LogicalType::INT64());
        auto indptrDataset = std::make_shared<LanceDataset>(indptrDatasetPath);
        const auto indptrSchema = indptrDataset->schema();
        if (indptrSchema.empty()) {
            throw RuntimeException{
                "Lance indptr dataset '" + indptrDatasetPath + "' has no columns"};
        }
        requireExactType(indptrDatasetPath, tableName, indptrSchema[0].name,
            indptrSchema[0].ladybugType, LogicalType::INT64());
    }
    // Edge properties match by name and must match exactly; missing properties
    // read as NULL.
    for (auto i = 0u; i < relGroupEntry->getNumProperties(); ++i) {
        const auto& prop = relGroupEntry->getProperty(i);
        if (prop.getType().getLogicalTypeID() == LogicalTypeID::INTERNAL_ID) {
            continue; // Rel ID is synthesized from the row index, not stored.
        }
        auto it = std::find_if(indicesSchema.begin(), indicesSchema.end(),
            [&](const LanceDatasetColumn& col) { return col.name == prop.getName(); });
        if (it == indicesSchema.end()) {
            continue;
        }
        requireExactType(indicesDatasetPath, tableName, it->name, it->ladybugType, prop.getType());
    }
    (void)context;
}

void LanceRelTable::ensureReader(Transaction* transaction,
    LanceRelTableScanState& scanState) const {
    if (scanState.reader) {
        return;
    }
    auto context = transaction->getClientContext();
    if (!context) {
        throw RuntimeException("Invalid client context for Lance rel scan initialization");
    }
    auto lanceDataset = ensureDataset(context);
    // Projection: endpoint offset column(s) first, then edge properties present in the
    // dataset (by name), in catalog column order. outputToCached maps each requested
    // output position to its cached-batch position (-1 = NULL fill).
    std::vector<LanceReader::ProjectedColumn> projection;
    std::vector<column_id_t> projColumnIDs;
    if (layout == LanceRelTableLayout::FLAT) {
        if (indicesSchema.size() < 2) {
            throw RuntimeException{"Flat Lance rel dataset '" + indicesDatasetPath +
                                   "' requires source and target offset columns"};
        }
        projection.push_back(
            LanceReader::ProjectedColumn{indicesSchema[0].name, LogicalType::INT64()});
        projColumnIDs.push_back(INVALID_COLUMN_ID);
        projection.push_back(
            LanceReader::ProjectedColumn{indicesSchema[1].name, LogicalType::INT64()});
        projColumnIDs.push_back(INVALID_COLUMN_ID);
    } else {
        projection.push_back(
            LanceReader::ProjectedColumn{indicesSchema[0].name, LogicalType::INT64()});
        projColumnIDs.push_back(INVALID_COLUMN_ID);
    }
    for (auto i = 0u; i < relGroupEntry->getNumProperties(); ++i) {
        const auto& prop = relGroupEntry->getProperty(i);
        if (prop.getType().getLogicalTypeID() == LogicalTypeID::INTERNAL_ID) {
            continue;
        }
        auto it = std::find_if(indicesSchema.begin(), indicesSchema.end(),
            [&](const LanceDatasetColumn& col) { return col.name == prop.getName(); });
        if (it == indicesSchema.end()) {
            continue;
        }
        projection.push_back(LanceReader::ProjectedColumn{it->name, prop.getType().copy()});
        projColumnIDs.push_back(relGroupEntry->getColumnID(i));
    }
    scanState.dataset = lanceDataset;
    scanState.reader =
        std::make_unique<LanceReader>(std::move(lanceDataset), std::move(projection));
    scanState.projColumnIDs = std::move(projColumnIDs);
    scanState.outputToCached.assign(scanState.columnIDs.size(), -1);
    for (size_t outCol = 0; outCol < scanState.columnIDs.size(); ++outCol) {
        const auto colID = scanState.columnIDs[outCol];
        if (colID == NBR_ID_COLUMN_ID) {
            scanState.outputToCached[outCol] = 0;
            continue;
        }
        if (colID == INVALID_COLUMN_ID || colID == ROW_IDX_COLUMN_ID || colID == REL_ID_COLUMN_ID) {
            continue;
        }
        for (size_t projCol = 0; projCol < scanState.projColumnIDs.size(); ++projCol) {
            if (scanState.projColumnIDs[projCol] == colID) {
                scanState.outputToCached[outCol] = static_cast<int64_t>(projCol);
                break;
            }
        }
    }
}

void LanceRelTable::fillCachedBatch(Transaction* transaction,
    LanceRelTableScanState& scanState) const {
    auto projTypes = scanState.reader->getProjectionTypes();
    scanState.cachedBatchData = std::make_unique<DataChunk>(projTypes.size());
    auto context = transaction->getClientContext();
    auto memoryManager = MemoryManager::Get(*context);
    for (auto i = 0u; i < projTypes.size(); ++i) {
        scanState.cachedBatchData->insert(i,
            std::make_shared<ValueVector>(projTypes[i].copy(), memoryManager));
    }
    scanState.cachedBatchData->state->getSelVectorUnsafe().setToFiltered(0);
    scanState.reader->next(*scanState.cachedBatchData);
}

void LanceRelTable::initScanState(Transaction* transaction, TableScanState& scanState,
    bool resetCachedBoundNodeSelVec) const {
    auto& relScanState = scanState.cast<RelTableScanState>();
    relScanState.source = TableScanSource::COMMITTED;
    relScanState.nodeGroup = nullptr;
    relScanState.nodeGroupIdx = INVALID_NODE_GROUP_IDX;

    if (resetCachedBoundNodeSelVec) {
        if (relScanState.nodeIDVector->state->getSelVector().isUnfiltered()) {
            relScanState.cachedBoundNodeSelVector.setToUnfiltered();
        } else {
            relScanState.cachedBoundNodeSelVector.setToFiltered();
            memcpy(relScanState.cachedBoundNodeSelVector.getMutableBuffer().data(),
                relScanState.nodeIDVector->state->getSelVector().getMutableBuffer().data(),
                relScanState.nodeIDVector->state->getSelVector().getSelSize() *
                    sizeof(common::sel_t));
        }
        relScanState.cachedBoundNodeSelVector.setSelSize(
            relScanState.nodeIDVector->state->getSelVector().getSelSize());
    }

    auto& lanceScanState = common::dynamic_cast_checked<LanceRelTableScanState&>(relScanState);
    ensureReader(transaction, lanceScanState);

    auto numRows = UINT64_MAX;
    auto startRow = 0u;
    if (layout == LanceRelTableLayout::CSR) {
        loadIndptrData(transaction);
    }

    // Create a set of bound node IDs for fast lookup.
    std::unordered_map<common::offset_t, common::sel_t> boundNodeOffsets;
    for (size_t i = 0; i < lanceScanState.cachedBoundNodeSelVector.getSelSize(); ++i) {
        common::sel_t boundNodeIdx = lanceScanState.cachedBoundNodeSelVector[i];
        const auto boundNodeID = lanceScanState.nodeIDVector->getValue<nodeID_t>(boundNodeIdx);
        boundNodeOffsets.insert({boundNodeID.offset, boundNodeIdx});
    }
    lanceScanState.reset(std::move(boundNodeOffsets));

    // Range-limit the indices scan to the dataset rows that can contain edges of this
    // bound-node batch (CSR is sorted by source node). FWD only; BWD bound nodes are
    // targets scattered through the file. Mirrors IceDiskRelTable.
    if (layout == LanceRelTableLayout::CSR && lanceScanState.direction == RelDataDirection::FWD &&
        !indptrData.empty() && !lanceScanState.boundNodeOffsets.empty()) {
        common::offset_t minNode = std::numeric_limits<common::offset_t>::max();
        common::offset_t maxNode = 0;
        for (auto& entry : lanceScanState.boundNodeOffsets) {
            minNode = std::min(minNode, entry.first);
            maxNode = std::max(maxNode, entry.first);
        }
        if ((uint64_t)minNode + 1 < indptrData.size()) {
            auto endIdx = std::min<uint64_t>((uint64_t)maxNode + 1, indptrData.size() - 1);
            startRow = (uint64_t)indptrData[minNode];
            numRows = (uint64_t)indptrData[endIdx] - startRow;
        }
    }
    lanceScanState.currentBatchStartOffset = static_cast<common::offset_t>(startRow);
    lanceScanState.currentLocalRowIdx = 0;
    lanceScanState.reader->openRange(startRow, numRows);

    // Re-anchor the monotonic CSR source-node cursor to the node containing the first
    // scanned row. Bound-node batches are not guaranteed to arrive in increasing offset
    // order, so the cursor is repositioned per batch. Mirrors IceDiskRelTable.
    if (layout == LanceRelTableLayout::CSR && !indptrData.empty() &&
        lanceScanState.currentBatchStartOffset < indptrData.back()) {
        lanceScanState.csrSrcNodeIdx =
            findSourceNodeForRowInternal(lanceScanState.currentBatchStartOffset, indptrData);
    }
}

void LanceRelTable::loadIndptrData(Transaction* transaction) const {
    if (indptrDatasetPath.empty() || indptrDataLoaded.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lock(indptrDataMutex);
    if (indptrDataLoaded.load(std::memory_order_relaxed)) {
        return;
    }
    {
        auto context = transaction->getClientContext();
        auto indptrDataset = std::make_shared<LanceDataset>(indptrDatasetPath);
        const auto indptrSchema = indptrDataset->schema();
        if (indptrSchema.empty()) {
            throw RuntimeException("Indptr Lance dataset has no columns");
        }
        std::vector<LanceReader::ProjectedColumn> projection;
        projection.push_back(
            LanceReader::ProjectedColumn{indptrSchema[0].name, LogicalType::INT64()});
        LanceReader reader(indptrDataset, std::move(projection));
        reader.openRange(0, UINT64_MAX);
        auto memoryManager = MemoryManager::Get(*context);
        DataChunk chunk(1);
        chunk.insert(0, std::make_shared<ValueVector>(LogicalType::INT64(), memoryManager));
        while (reader.next(chunk) > 0) {
            auto selSize = chunk.state->getSelVector().getSelSize();
            for (size_t i = 0; i < selSize; ++i) {
                indptrData.push_back(chunk.getValueVector(0).getValue<common::offset_t>(i));
            }
            chunk.state->getSelVectorUnsafe().setToFiltered(0);
        }
        indptrDataLoaded.store(true, std::memory_order_release);
    }
}

bool LanceRelTable::scanInternal(Transaction* transaction, TableScanState& scanState) {
    auto& lanceScanState = common::dynamic_cast_checked<LanceRelTableScanState&>(scanState);
    if (layout == LanceRelTableLayout::FLAT) {
        return scanFlat(transaction, lanceScanState);
    }
    return scanCSR(transaction, lanceScanState);
}

bool LanceRelTable::scanCSR(Transaction* transaction, LanceRelTableScanState& lanceScanState) {
    lanceScanState.resetOutVectors();

    if (lanceScanState.boundNodeOffsets.empty()) {
        lanceScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    loadIndptrData(transaction);

    const auto isFwd = lanceScanState.direction != RelDataDirection::BWD;
    uint64_t totalRowsCollected = 0;
    const uint64_t maxRowsPerCall = DEFAULT_VECTOR_CAPACITY;
    auto activeBoundSelPos = INVALID_SEL;
    auto activeBoundOffset = INVALID_OFFSET;
    auto hasActiveBound = false;
    auto differentBoundNodeEncountered = false;

    while (totalRowsCollected < maxRowsPerCall) {
        if (!lanceScanState.cachedBatchData ||
            lanceScanState.currentLocalRowIdx ==
                lanceScanState.cachedBatchData->state->getSelVector().getSelSize()) {
            lanceScanState.currentBatchStartOffset += lanceScanState.currentLocalRowIdx;
            lanceScanState.currentLocalRowIdx = 0;
            fillCachedBatch(transaction, lanceScanState);
        }

        auto selSize = lanceScanState.cachedBatchData->state->getSelVector().getSelSize();
        if (selSize == 0) {
            break;
        }

        for (; lanceScanState.currentLocalRowIdx < selSize && totalRowsCollected < maxRowsPerCall;
             ++lanceScanState.currentLocalRowIdx) {
            const auto currentGlobalRowIdx =
                lanceScanState.currentBatchStartOffset + lanceScanState.currentLocalRowIdx;
            if (indptrData.empty()) {
                continue;
            }
            while (lanceScanState.csrSrcNodeIdx + 1 < indptrData.size() &&
                   indptrData[lanceScanState.csrSrcNodeIdx + 1] <= currentGlobalRowIdx) {
                ++lanceScanState.csrSrcNodeIdx;
            }
            const auto sourceNodeOffset = lanceScanState.csrSrcNodeIdx;

            // Cached column 0 is the destination node offset.
            const auto dstOffset =
                lanceScanState.cachedBatchData->getValueVector(0).getValue<common::offset_t>(
                    lanceScanState.currentLocalRowIdx);
            const auto boundOffset = isFwd ? sourceNodeOffset : dstOffset;
            if (lanceScanState.boundNodeOffsets.find(boundOffset) ==
                lanceScanState.boundNodeOffsets.end()) {
                continue;
            }

            if (!hasActiveBound) {
                hasActiveBound = true;
                activeBoundOffset = boundOffset;
                activeBoundSelPos = lanceScanState.boundNodeOffsets.at(boundOffset);
            } else if (boundOffset != activeBoundOffset) {
                differentBoundNodeEncountered = true;
                break;
            }

            const auto nbrOffset = isFwd ? dstOffset : sourceNodeOffset;
            const auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();
            auto nbrNodeID = internalID_t(nbrOffset, nbrTableID);

            if (!lanceScanState.outputVectors.empty()) {
                lanceScanState.outputVectors[0]->setValue(totalRowsCollected, nbrNodeID);
            }

            for (uint64_t outCol = 1; outCol < lanceScanState.outputVectors.size(); ++outCol) {
                if (outCol >= lanceScanState.columnIDs.size()) {
                    continue;
                }
                const auto colID = lanceScanState.columnIDs[outCol];
                if (colID == INVALID_COLUMN_ID || colID == ROW_IDX_COLUMN_ID ||
                    colID == NBR_ID_COLUMN_ID) {
                    continue;
                }
                if (colID == REL_ID_COLUMN_ID) {
                    // REL_ID is not stored; synthesize from the global row index.
                    lanceScanState.outputVectors[outCol]->setValue<internalID_t>(totalRowsCollected,
                        internalID_t{currentGlobalRowIdx, getTableID()});
                    continue;
                }
                const auto cachedCol = outCol < lanceScanState.outputToCached.size() ?
                                           lanceScanState.outputToCached[outCol] :
                                           -1;
                if (cachedCol < 0 ||
                    (uint64_t)cachedCol >= lanceScanState.cachedBatchData->getNumValueVectors()) {
                    // Property absent from the dataset reads as NULL.
                    lanceScanState.outputVectors[outCol]->setNull(totalRowsCollected, true);
                    continue;
                }
                lanceScanState.outputVectors[outCol]->copyFromVectorData(totalRowsCollected,
                    &lanceScanState.cachedBatchData->getValueVector(cachedCol),
                    lanceScanState.currentLocalRowIdx);
            }

            totalRowsCollected++;
        }

        if (differentBoundNodeEncountered) {
            break;
        }
    }

    if (totalRowsCollected > 0) {
        auto& selVector = lanceScanState.outState->getSelVectorUnsafe();
        selVector.setToUnfiltered(totalRowsCollected);
        lanceScanState.setNodeIDVectorToFlat(activeBoundSelPos);
        return true;
    }
    lanceScanState.outState->getSelVectorUnsafe().setToFiltered(0);
    return false;
}

bool LanceRelTable::scanFlat(Transaction* transaction, LanceRelTableScanState& lanceScanState) {
    lanceScanState.resetOutVectors();

    if (lanceScanState.boundNodeOffsets.empty()) {
        lanceScanState.outState->getSelVectorUnsafe().setToFiltered(0);
        return false;
    }

    const auto isFwd = lanceScanState.direction != RelDataDirection::BWD;
    uint64_t totalRowsCollected = 0;
    const uint64_t maxRowsPerCall = DEFAULT_VECTOR_CAPACITY;
    auto activeBoundSelPos = INVALID_SEL;
    auto activeBoundOffset = INVALID_OFFSET;
    auto hasActiveBound = false;
    auto differentBoundNodeEncountered = false;

    while (totalRowsCollected < maxRowsPerCall) {
        if (!lanceScanState.cachedBatchData ||
            lanceScanState.currentLocalRowIdx ==
                lanceScanState.cachedBatchData->state->getSelVector().getSelSize()) {
            lanceScanState.currentBatchStartOffset += lanceScanState.currentLocalRowIdx;
            lanceScanState.currentLocalRowIdx = 0;
            fillCachedBatch(transaction, lanceScanState);
        }

        auto selSize = lanceScanState.cachedBatchData->state->getSelVector().getSelSize();
        if (selSize == 0) {
            break;
        }

        for (; lanceScanState.currentLocalRowIdx < selSize && totalRowsCollected < maxRowsPerCall;
             ++lanceScanState.currentLocalRowIdx) {
            if (lanceScanState.cachedBatchData->getNumValueVectors() < 2) {
                throw RuntimeException("Flat Lance relationship dataset requires source and "
                                       "target offset columns");
            }
            const auto currentGlobalRowIdx =
                lanceScanState.currentBatchStartOffset + lanceScanState.currentLocalRowIdx;
            const auto srcOffset =
                lanceScanState.cachedBatchData->getValueVector(0).getValue<common::offset_t>(
                    lanceScanState.currentLocalRowIdx);
            const auto dstOffset =
                lanceScanState.cachedBatchData->getValueVector(1).getValue<common::offset_t>(
                    lanceScanState.currentLocalRowIdx);
            const auto boundOffset = isFwd ? srcOffset : dstOffset;
            auto boundIt = lanceScanState.boundNodeOffsets.find(boundOffset);
            if (boundIt == lanceScanState.boundNodeOffsets.end()) {
                continue;
            }

            if (!hasActiveBound) {
                hasActiveBound = true;
                activeBoundOffset = boundOffset;
                activeBoundSelPos = boundIt->second;
            } else if (boundOffset != activeBoundOffset) {
                differentBoundNodeEncountered = true;
                break;
            }

            const auto nbrOffset = isFwd ? dstOffset : srcOffset;
            const auto nbrTableID = isFwd ? getToNodeTableID() : getFromNodeTableID();
            if (!lanceScanState.outputVectors.empty()) {
                lanceScanState.outputVectors[0]->setValue(totalRowsCollected,
                    internalID_t(nbrOffset, nbrTableID));
            }

            for (uint64_t outCol = 1; outCol < lanceScanState.outputVectors.size(); ++outCol) {
                if (outCol >= lanceScanState.columnIDs.size()) {
                    continue;
                }
                const auto colID = lanceScanState.columnIDs[outCol];
                if (colID == INVALID_COLUMN_ID || colID == ROW_IDX_COLUMN_ID ||
                    colID == NBR_ID_COLUMN_ID) {
                    continue;
                }
                if (colID == REL_ID_COLUMN_ID) {
                    lanceScanState.outputVectors[outCol]->setValue<internalID_t>(totalRowsCollected,
                        internalID_t{currentGlobalRowIdx, getTableID()});
                    continue;
                }
                const auto cachedCol = outCol < lanceScanState.outputToCached.size() ?
                                           lanceScanState.outputToCached[outCol] :
                                           -1;
                if (cachedCol < 0 ||
                    (uint64_t)cachedCol >= lanceScanState.cachedBatchData->getNumValueVectors()) {
                    lanceScanState.outputVectors[outCol]->setNull(totalRowsCollected, true);
                    continue;
                }
                lanceScanState.outputVectors[outCol]->copyFromVectorData(totalRowsCollected,
                    &lanceScanState.cachedBatchData->getValueVector(cachedCol),
                    lanceScanState.currentLocalRowIdx);
            }

            totalRowsCollected++;
        }

        if (differentBoundNodeEncountered) {
            break;
        }
    }

    if (totalRowsCollected > 0) {
        auto& selVector = lanceScanState.outState->getSelVectorUnsafe();
        selVector.setToUnfiltered(totalRowsCollected);
        lanceScanState.setNodeIDVectorToFlat(activeBoundSelPos);
        return true;
    }
    lanceScanState.outState->getSelVectorUnsafe().setToFiltered(0);
    return false;
}

row_idx_t LanceRelTable::getTotalRowCount(const Transaction* transaction) const {
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

row_idx_t LanceRelTable::getActiveBoundNodeCount(const Transaction* transaction,
    RelDataDirection direction) const {
    if (layout != LanceRelTableLayout::CSR || direction == RelDataDirection::BWD) {
        return 0;
    }
    loadIndptrData(const_cast<Transaction*>(transaction));
    row_idx_t result = 0;
    for (offset_t i = 0; i + 1 < indptrData.size(); ++i) {
        result += indptrData[i + 1] > indptrData[i];
    }
    return result;
}

std::vector<std::pair<offset_t, row_idx_t>> LanceRelTable::getAllDegreeEntries(
    const Transaction* transaction, RelDataDirection direction) const {
    if (layout != LanceRelTableLayout::CSR || direction == RelDataDirection::BWD) {
        return {};
    }
    loadIndptrData(const_cast<Transaction*>(transaction));
    std::vector<std::pair<offset_t, row_idx_t>> result;
    result.reserve(indptrData.size());
    for (offset_t i = 0; i + 1 < indptrData.size(); ++i) {
        if (indptrData[i + 1] <= indptrData[i]) {
            continue;
        }
        result.emplace_back(i, indptrData[i + 1] - indptrData[i]);
    }
    return result;
}

row_idx_t LanceRelTable::getDegreeForOffsetInternal(const Transaction* transaction,
    RelDataDirection direction, offset_t nodeOffset) const {
    if (layout != LanceRelTableLayout::CSR || direction == RelDataDirection::BWD) {
        return 0;
    }
    loadIndptrData(const_cast<Transaction*>(transaction));
    if (nodeOffset + 1 >= indptrData.size()) {
        return 0;
    }
    if (indptrData[nodeOffset + 1] <= indptrData[nodeOffset]) {
        return 0;
    }
    return indptrData[nodeOffset + 1] - indptrData[nodeOffset];
}

std::vector<std::pair<offset_t, row_idx_t>> LanceRelTable::getTopKDegreeEntries(
    const Transaction* transaction, RelDataDirection direction, common::idx_t k) const {
    if (layout != LanceRelTableLayout::CSR || direction == RelDataDirection::BWD || k == 0) {
        return {};
    }
    loadIndptrData(const_cast<Transaction*>(transaction));
    using degree_entry_t = std::pair<offset_t, row_idx_t>;
    auto better = [](const degree_entry_t& a, const degree_entry_t& b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    };
    auto worseForHeap = [better](const degree_entry_t& a, const degree_entry_t& b) {
        return better(a, b);
    };
    std::priority_queue<degree_entry_t, std::vector<degree_entry_t>, decltype(worseForHeap)> heap{
        worseForHeap};
    for (offset_t i = 0; i + 1 < indptrData.size(); ++i) {
        if (indptrData[i + 1] <= indptrData[i]) {
            continue;
        }
        const auto degree = indptrData[i + 1] - indptrData[i];
        degree_entry_t entry{i, degree};
        if (heap.size() < k) {
            heap.push(entry);
        } else if (better(entry, heap.top())) {
            heap.pop();
            heap.push(entry);
        }
    }
    std::vector<degree_entry_t> result;
    while (!heap.empty()) {
        result.push_back(heap.top());
        heap.pop();
    }
    std::sort(result.begin(), result.end(), better);
    return result;
}

} // namespace lance_extension
} // namespace lbug
