#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "common/exception/runtime.h"
#include "common/types/internal_id_util.h"
#include "storage/lance_reader.h"
#include "storage/table/columnar_rel_table_base.h"
#include "storage/table/ice_disk_rel_table.h"
#include "transaction/transaction.h"

namespace lbug {
namespace lance_extension {

enum class LanceRelTableLayout : uint8_t { CSR, FLAT };

// Extends IceDiskRelTableScanState (rather than RelTableScanState directly) so a single
// shared scan state can serve lance, icebug-disk, Arrow, and native rel tables in
// multi-rel scans; see ExternalTableHooks::scanStateRank. The Parquet reader members
// inherited from the icebug-disk state stay null; Lance reads go through `reader`.
struct LanceRelTableScanState final : storage::IceDiskRelTableScanState {
    std::unique_ptr<LanceReader> reader;
    // Maps scan-output positions to cached-batch positions (catalog column ID order is
    // not assumed to match dataset order; edge properties match by name).
    std::vector<int64_t> outputToCached;
    // Projection column IDs requested from the dataset, parallel to the reader projection.
    // outputToCached[outCol] indexes into this vector; -1 means NULL fill.
    std::vector<common::column_id_t> projColumnIDs;

    LanceRelTableScanState(storage::MemoryManager& mm, common::ValueVector* nodeIDVector,
        std::vector<common::ValueVector*> outputVectors,
        std::shared_ptr<common::DataChunkState> outChunkState)
        : storage::IceDiskRelTableScanState{mm, nodeIDVector, std::move(outputVectors),
              std::move(outChunkState)} {}
    // setToTable is inherited from IceDiskRelTableScanState: CSR column resolution against
    // the (empty) RelTableData scaffolding is safe for Lance tables exactly as for
    // icebug-disk tables; the Lance scan reads through `reader` instead.
};

class LanceRelTable final : public storage::ColumnarRelTableBase {
public:
    LanceRelTable(catalog::RelGroupCatalogEntry* relGroupEntry, common::table_id_t fromTableID,
        common::table_id_t toTableID, const storage::StorageManager* storageManager,
        storage::MemoryManager* memoryManager, main::ClientContext* context = nullptr);

    void initScanState(transaction::Transaction* transaction, storage::TableScanState& scanState,
        bool resetCachedBoundNodeSelVec = true) const override;

    bool scanInternal(transaction::Transaction* transaction,
        storage::TableScanState& scanState) override;

    LanceRelTableLayout getLayout() const { return layout; }
    const std::string& getIndicesDatasetPath() const { return indicesDatasetPath; }

protected:
    std::string getColumnarFormatName() const override { return "lance"; }
    common::row_idx_t getTotalRowCount(const transaction::Transaction* transaction) const override;
    common::row_idx_t getActiveBoundNodeCount(const transaction::Transaction* transaction,
        common::RelDataDirection direction) const override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getAllDegreeEntries(
        const transaction::Transaction* transaction,
        common::RelDataDirection direction) const override;
    common::row_idx_t getDegreeForOffsetInternal(const transaction::Transaction* transaction,
        common::RelDataDirection direction, common::offset_t nodeOffset) const override;
    std::vector<std::pair<common::offset_t, common::row_idx_t>> getTopKDegreeEntries(
        const transaction::Transaction* transaction, common::RelDataDirection direction,
        common::idx_t k) const override;

private:
    std::shared_ptr<LanceConnector> connector;
    LanceRelTableLayout layout;
    std::string indicesDatasetPath;
    std::string indptrDatasetPath;
    mutable std::mutex schemaMutex;
    mutable std::vector<LanceDatasetColumn> indicesSchema;
    mutable std::mutex indptrDataMutex;
    mutable std::vector<common::offset_t> indptrData;
    mutable std::atomic<bool> indptrDataLoaded{false};
    mutable std::atomic<common::row_idx_t> cachedRowCount{common::INVALID_ROW_IDX};

    void ensureSchemasLoaded(main::ClientContext* context) const;
    void checkCompatibility(main::ClientContext* context) const;
    void loadIndptrData(transaction::Transaction* transaction) const;
    void ensureReader(transaction::Transaction* transaction,
        LanceRelTableScanState& scanState) const;
    void fillCachedBatch(transaction::Transaction* transaction,
        LanceRelTableScanState& scanState) const;
    bool scanCSR(transaction::Transaction* transaction, LanceRelTableScanState& scanState);
    bool scanFlat(transaction::Transaction* transaction, LanceRelTableScanState& scanState);
};

} // namespace lance_extension
} // namespace lbug
