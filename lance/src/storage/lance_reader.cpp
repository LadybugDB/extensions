#include "storage/lance_reader.h"

#include <limits>
#include <vector>

#include "common/exception/runtime.h"
#include <format>

namespace lbug {
namespace lance_extension {

using namespace common;

LanceDataset::LanceDataset(std::string datasetPath_)
    : datasetPath{std::move(datasetPath_)}, dataset{datasetPath} {}

uint64_t LanceDataset::countRows() {
    std::lock_guard lock{mutex};
    if (rowCountCached) {
        return cachedRowCount;
    }
    const auto rows = lance_dataset_count_rows(dataset.handle);
    if (rows < 0) {
        checkLance(-1, "Failed to count rows of Lance dataset '" + datasetPath + "'");
    }
    cachedRowCount = static_cast<uint64_t>(rows);
    rowCountCached = true;
    return cachedRowCount;
}

uint64_t LanceDataset::version() {
    uint64_t out = 0;
    checkLance(lance_dataset_version(dataset.handle, &out),
        "Failed to read version of Lance dataset '" + datasetPath + "'");
    return out;
}

std::vector<LanceDatasetColumn> LanceDataset::schema() {
    ArrowSchemaWrapper schema;
    checkLance(lance_schema_to_arrow(dataset.handle, &schema),
        "Failed to read schema of Lance dataset '" + datasetPath + "'");
    std::vector<LanceDatasetColumn> columns;
    for (int64_t i = 0; i < schema.n_children; ++i) {
        const auto* child = schema.children[i];
        columns.push_back(LanceDatasetColumn{child->name ? child->name : "",
            ArrowConverter::fromArrowSchema(child)});
    }
    return columns;
}

std::string LanceDataset::versionString() {
    const char* raw = lance_schema_version_string(dataset.handle);
    if (!raw) {
        checkLance(-1, "Failed to read schema metadata of Lance dataset '" + datasetPath + "'");
    }
    std::string out = raw;
    lance_free_string(raw);
    return out;
}

LanceReader::LanceReader(std::shared_ptr<LanceDataset> dataset_,
    std::vector<ProjectedColumn> projection_)
    : dataset{std::move(dataset_)}, projection{std::move(projection_)},
      datasetPath{dataset->getPath()} {}

std::vector<LogicalType> LanceReader::getProjectionTypes() const {
    std::vector<LogicalType> types;
    types.reserve(projection.size());
    for (auto& col : projection) {
        types.push_back(col.ladybugType.copy());
    }
    return types;
}

void LanceReader::openRange(uint64_t offset, uint64_t limit) {
    scan.close();
    std::vector<const char*> columns;
    columns.reserve(projection.size());
    for (auto& col : projection) {
        columns.push_back(col.datasetName.c_str());
    }
    const int64_t ffiLimit =
        limit == UINT64_MAX ? -1 : static_cast<int64_t>(std::min<uint64_t>(limit, INT64_MAX));
    void* stream =
        lance_scan_open(dataset->handle(), columns.data(), columns.size(), ffiLimit, offset);
    if (!stream) {
        checkLance(-1, "Failed to open Lance scan on '" + datasetPath + "' at offset " +
                           std::to_string(offset));
    }
    scan.handle = stream;
}

uint64_t LanceReader::next(DataChunk& out) {
    if (!scan.handle) {
        return 0;
    }
    void* rawBatch = nullptr;
    const int rc = lance_scan_next(scan.handle, &rawBatch);
    if (rc == 1) {
        scan.close();
        return 0;
    }
    checkLance(rc, "Failed to read Lance dataset '" + datasetPath + "'");
    auto slice = LanceBatchSlice::importBatch(rawBatch, datasetPath);
    const auto rows = static_cast<uint64_t>(slice.array.length);
    // Detach vector states for conversion: ArrowConverter consults the output
    // vector state, and the caller's (usually filtered) chunk state would
    // suppress all writes. Restored before returning.
    std::vector<std::shared_ptr<DataChunkState>> savedStates;
    for (auto i = 0u; i < projection.size(); ++i) {
        auto& vec = out.getValueVectorMutable(i);
        savedStates.push_back(vec.state);
        vec.setState(nullptr);
    }
    // Match batch columns to the projection by name (scan output order is not
    // part of the contract).
    for (auto i = 0u; i < projection.size(); ++i) {
        const auto* childSchema = static_cast<const ArrowSchema*>(nullptr);
        const auto* childArray = static_cast<const ArrowArray*>(nullptr);
        for (int64_t j = 0; j < slice.schema.n_children; ++j) {
            const char* name = slice.schema.children[j]->name;
            if (name && projection[i].datasetName == name) {
                childSchema = slice.schema.children[j];
                childArray = slice.array.children[j];
                break;
            }
        }
        if (!childSchema || !childArray) {
            throw RuntimeException{"Lance scan of '" + datasetPath + "' is missing projected " +
                                   "column '" + projection[i].datasetName + "'"};
        }
        ArrowConverter::fromArrowArray(childSchema, childArray, out.getValueVectorMutable(i));
    }
    for (auto i = 0u; i < projection.size(); ++i) {
        out.getValueVectorMutable(i).setState(savedStates[i]);
    }
    out.state->getSelVectorUnsafe().setToUnfiltered(rows);
    return rows;
}

} // namespace lance_extension
} // namespace lbug
