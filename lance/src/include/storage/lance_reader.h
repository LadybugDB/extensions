#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/arrow/arrow_converter.h"
#include "common/data_chunk/data_chunk.h"
#include "storage/lance_ffi.h"

namespace lbug {
namespace lance_extension {

// One dataset column: its name and Ladybug type (mapped from the dataset's
// Arrow schema with ArrowConverter, so conversion of batch arrays with the
// same schema is exact by construction).
struct LanceDatasetColumn {
    std::string name;
    common::LogicalType ladybugType;
};

// Shared, thread-safe handle to one open Lance dataset (a stable snapshot of
// the latest version at open time). Tables hold one; scan states borrow it
// for their range scans.
class LanceDataset {
public:
    explicit LanceDataset(std::string datasetPath);
    ~LanceDataset() = default;
    LanceDataset(const LanceDataset&) = delete;
    LanceDataset& operator=(const LanceDataset&) = delete;

    const std::string& getPath() const { return datasetPath; }
    uint64_t countRows();
    uint64_t version();
    // Dataset schema: column names and mapped types, in dataset order.
    std::vector<LanceDatasetColumn> schema();
    // The `lance_version` schema-metadata marker, or empty when absent.
    std::string versionString();

    void* handle() { return dataset.handle; }

private:
    std::string datasetPath;
    LanceDatasetHandle dataset;
    std::mutex mutex;
    bool rowCountCached = false;
    uint64_t cachedRowCount = 0;
};

// Stateful range reader over one Lance dataset. Opens one scan per range
// (`SELECT`-equivalent projection + limit/offset pushed into Lance) and
// decodes slices into Ladybug DataChunks with ArrowConverter. One reader
// serves one scan state; ranges open sequentially, so no locking is needed.
class LanceReader {
public:
    // One projected output column: the dataset column name and Ladybug type.
    // Types must match the dataset schema exactly (validated at CREATE).
    struct ProjectedColumn {
        std::string datasetName;
        common::LogicalType ladybugType;
    };

    LanceReader(std::shared_ptr<LanceDataset> dataset, std::vector<ProjectedColumn> projection);

    // Issues a new range scan, replacing any active one. limit == UINT64_MAX scans to end.
    void openRange(uint64_t offset, uint64_t limit);
    // Converts the next slice of the active range into a fresh `out` (whose vectors must
    // match the projection in order). Returns rows read; 0 means exhausted.
    uint64_t next(common::DataChunk& out);

    const std::vector<ProjectedColumn>& getProjection() const { return projection; }
    std::vector<common::LogicalType> getProjectionTypes() const;

private:
    std::shared_ptr<LanceDataset> dataset;
    std::vector<ProjectedColumn> projection;
    LanceScanHandle scan;
    std::string datasetPath;
};

} // namespace lance_extension
} // namespace lbug
