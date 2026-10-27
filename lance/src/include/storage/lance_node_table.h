#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "storage/lance_reader.h"
#include "storage/table/columnar_node_table_base.h"

namespace lbug {
namespace lance_extension {

// Read-only node table backed by a Lance dataset. Mirrors IceDiskNodeTable:
// one dataset row per node, node offsets are positional row indices, and the
// primary key is a regular property column. The dataset is
// `<dir>/nodes_<table>.lance`, or a single `<file>.lance` dataset when storage
// points at one directly.
struct LanceNodeTableScanState final : storage::ColumnarNodeTableScanState {
    std::shared_ptr<LanceConnector> connector;
    std::string datasetPath;
    std::unique_ptr<LanceReader> reader;
    // Current morsel: global row range [morselStart, morselStart + morselLimit).
    uint64_t morselStart = 0;
    uint64_t morselLimit = 0;
    // Rows of the current morsel consumed so far (for global offset mapping).
    uint64_t morselConsumed = 0;
    // Requested column IDs, snapshotted when the reader is built.
    std::vector<common::column_id_t> readerColumnIDs;

    LanceNodeTableScanState([[maybe_unused]] storage::MemoryManager& mm,
        common::ValueVector* nodeIDVector, std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : storage::ColumnarNodeTableScanState{mm, nodeIDVector, std::move(outputVectors),
              std::move(outChunkState)} {}
};

struct LanceNodeTableScanSharedState final : storage::ColumnarNodeTableScanSharedState {
private:
    std::mutex mtx;
    common::node_group_idx_t currentMorselIdx = 0;
    common::node_group_idx_t numMorsels = 0;

public:
    void reset(common::node_group_idx_t totalMorsels) {
        std::lock_guard<std::mutex> lock(mtx);
        currentMorselIdx = 0;
        numMorsels = totalMorsels;
    }

    bool getNextMorsel(common::node_group_idx_t& assignedMorselIdx) {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentMorselIdx < numMorsels) {
            assignedMorselIdx = currentMorselIdx++;
            return true;
        }
        return false;
    }

    bool getNextMorsel(storage::ColumnarNodeTableScanState*) override { return false; }
};

class LanceNodeTable final : public storage::ColumnarNodeTableBase {
public:
    LanceNodeTable(const storage::StorageManager* storageManager,
        const catalog::NodeTableCatalogEntry* nodeTableEntry, storage::MemoryManager* memoryManager,
        main::ClientContext* context = nullptr);

    void initializeScanCoordination(const transaction::Transaction* transaction) override;

    void initScanState(transaction::Transaction* transaction, storage::TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction,
        storage::TableScanState& scanState) override;

    bool isVisible(const transaction::Transaction* transaction,
        common::offset_t offset) const override;
    bool isVisibleNoLock(const transaction::Transaction* transaction,
        common::offset_t offset) const override;
    bool lookupPK(const transaction::Transaction* transaction, common::ValueVector* keyVector,
        uint64_t vectorPos, common::offset_t& result) const override;

    const std::string& getDatasetPath() const { return datasetPath; }
    // Morsel count for ScanNodeTableSharedState::initialize (via ExternalTableHooks).
    common::node_group_idx_t getNumScanMorsels(const transaction::Transaction* transaction) const;

protected:
    std::string getColumnarFormatName() const override { return "lance"; }
    common::node_group_idx_t getNumBatches(
        const transaction::Transaction* transaction) const override;
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;

private:
    std::shared_ptr<LanceConnector> connector;
    std::string datasetPath;
    mutable std::mutex schemaMutex;
    mutable std::vector<LanceDatasetColumn> datasetSchema;
    mutable std::atomic<common::row_idx_t> cachedRowCount{common::INVALID_ROW_IDX};

    void ensureSchemaLoaded(main::ClientContext* context) const;
    void checkCompatibility(main::ClientContext* context) const;
    void initReaderForMorsel(transaction::Transaction* transaction,
        LanceNodeTableScanState& scanState) const;
};

} // namespace lance_extension
} // namespace lbug
