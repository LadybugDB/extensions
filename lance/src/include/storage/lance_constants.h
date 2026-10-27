#pragma once

#include <string>

#include "common/constants.h"

namespace lbug {
namespace lance_extension {

// Version and tuning constants for the Lance storage format (v1).
// Mirrors IceDiskConstants, but for Lance datasets instead of Parquet files.
struct LanceConstants {
    static constexpr std::string_view V1 = "v1";

    static constexpr std::string_view CURRENT_VERSION = V1;

    // Contract key for the dataset schema metadata written by the converter.
    // v1 readers validate structural compatibility at CREATE TABLE time (see
    // LanceUtils::checkCompatibility); strict metadata enforcement arrives
    // with the native Lance reader (see docs/lance.md).
    static constexpr std::string_view VERSION_METADATA_KEY = "lance_version";

    static constexpr std::string_view DATASET_SUFFIX = ".lance";

    // Node-table scan morsels are row ranges read with limit/offset pushed
    // into the Lance scan, so each morsel is one efficient range query.
    // Bounded to keep per-morsel materialized results small.
    static constexpr uint64_t MORSEL_TARGET_ROWS = 8192;
    static constexpr uint64_t MAX_MORSELS = 4096;
};

} // namespace lance_extension
} // namespace lbug
