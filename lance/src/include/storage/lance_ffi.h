#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/arrow/arrow.h"
#include "common/exception/runtime.h"

// C ABI of the lance_ladybug_ffi Rust staticlib (extension/lance/rust).
// Conventions mirror lance-duckdb's FFI: opaque void* handles, int return
// codes (0 ok, 1 exhausted where noted, -1 error), and thread-local error
// details via lance_last_error_* (taking the message transfers ownership to
// the caller, who frees it with lance_free_string).
extern "C" {

void* lance_open_dataset(const char* path);
void lance_close_dataset(void* dataset);
int64_t lance_dataset_count_rows(void* dataset);
int lance_dataset_version(void* dataset, uint64_t* out_version);
int lance_schema_to_arrow(void* dataset, struct ArrowSchema* out_schema);
const char* lance_schema_version_string(void* dataset);
void* lance_scan_open(void* dataset, const char* const* columns, size_t columns_len, int64_t limit,
    uint64_t offset);
int lance_scan_next(void* stream, void** out_batch);
void lance_close_scan(void* stream);
int lance_batch_to_arrow(void* batch, struct ArrowArray* out_array, struct ArrowSchema* out_schema);
void lance_free_batch(void* batch);
void lance_free_string(const char* string);
int lance_last_error_code();
const char* lance_last_error_message();

} // extern "C"

namespace lbug {
namespace lance_extension {

// Throws RuntimeException with the pending Rust error (if any).
inline void checkLance(int rc, const std::string& what) {
    if (rc == 0) {
        return;
    }
    std::string detail;
    if (const char* msg = lance_last_error_message()) {
        detail = msg;
        lance_free_string(msg);
    }
    throw common::RuntimeException{what + (detail.empty() ? "" : ": " + detail)};
}

struct LanceDatasetHandle {
    void* handle = nullptr;
    explicit LanceDatasetHandle(const std::string& path) {
        handle = lance_open_dataset(path.c_str());
        if (!handle) {
            checkLance(-1, "Failed to open Lance dataset '" + path + "'");
        }
    }
    ~LanceDatasetHandle() {
        if (handle) {
            lance_close_dataset(handle);
        }
    }
    LanceDatasetHandle(const LanceDatasetHandle&) = delete;
    LanceDatasetHandle& operator=(const LanceDatasetHandle&) = delete;
    LanceDatasetHandle(LanceDatasetHandle&& other) noexcept : handle{other.handle} {
        other.handle = nullptr;
    }
    LanceDatasetHandle& operator=(LanceDatasetHandle&& other) noexcept {
        if (this != &other) {
            if (handle) {
                lance_close_dataset(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
};

struct LanceScanHandle {
    void* handle = nullptr;
    LanceScanHandle() = default;
    LanceScanHandle(const LanceScanHandle&) = delete;
    LanceScanHandle& operator=(const LanceScanHandle&) = delete;
    LanceScanHandle(LanceScanHandle&& other) noexcept : handle{other.handle} {
        other.handle = nullptr;
    }
    LanceScanHandle& operator=(LanceScanHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~LanceScanHandle() { close(); }
    void close() {
        if (handle) {
            lance_close_scan(handle);
            handle = nullptr;
        }
    }
};

// One exported batch slice: the Rust batch is freed right after export (the
// exported array/schema hold their own references), so this only owns the
// Arrow C structs, released by the RAII wrappers on scope exit.
struct LanceBatchSlice {
    ArrowArrayWrapper array;
    ArrowSchemaWrapper schema;
    LanceBatchSlice() = default;
    LanceBatchSlice(const LanceBatchSlice&) = delete;
    LanceBatchSlice& operator=(const LanceBatchSlice&) = delete;
    LanceBatchSlice(LanceBatchSlice&&) = default;
    LanceBatchSlice& operator=(LanceBatchSlice&&) = default;

    static LanceBatchSlice importBatch(void* rawBatch, const std::string& datasetPath) {
        LanceBatchSlice slice;
        checkLance(lance_batch_to_arrow(rawBatch, &slice.array, &slice.schema),
            "Failed to export Lance batch from '" + datasetPath + "'");
        lance_free_batch(rawBatch);
        return slice;
    }
};

} // namespace lance_extension
} // namespace lbug
