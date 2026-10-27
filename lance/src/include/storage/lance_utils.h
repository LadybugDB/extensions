#pragma once

#include <string>

#include "common/string_utils.h"
#include "storage/lance_constants.h"

namespace lbug {
namespace lance_extension {

// Path helpers for the Lance storage layout (v1). Mirrors IceDiskUtils, with
// `.lance` datasets in place of `.parquet` files:
//
//   <dir>/nodes_<table>.lance     node table dataset
//   <dir>/indices_<table>.lance   CSR edge targets (+ edge properties)
//   <dir>/indptr_<table>.lance    CSR row pointers (single integer column)
//   <file>.lance                  single-dataset storage (node table, or FLAT rel table)
struct LanceCSRPaths {
    std::string indices;
    std::string indptr;
};

class LanceUtils {
public:
    // Joins a base path with a filename. When base is empty the filename is returned
    // as-is (i.e. relative to the current working directory).
    static std::string joinPath(const std::string& base, const std::string& part) {
        if (base.empty()) {
            return part;
        }
        const char last = base.back();
        if (last == '/' || last == '\\') {
            return base + part;
        }
        return base + "/" + part;
    }

    static bool isSingleDatasetPath(const std::string& storage) {
        return common::StringUtils::getLower(storage).ends_with(LanceConstants::DATASET_SUFFIX);
    }

    // Get the dataset path for a node table.
    static std::string constructNodeTablePath(const std::string& dir, const std::string& name) {
        if (isSingleDatasetPath(dir)) {
            return dir;
        }
        return joinPath(dir, "nodes_" + name + std::string(LanceConstants::DATASET_SUFFIX));
    }

    // Get the dataset paths for a rel table's CSR files.
    static LanceCSRPaths constructCSRPaths(const std::string& dir, const std::string& name) {
        return {joinPath(dir, "indices_" + name + std::string(LanceConstants::DATASET_SUFFIX)),
            joinPath(dir, "indptr_" + name + std::string(LanceConstants::DATASET_SUFFIX))};
    }

    // Get the dataset path for a flat relationship table. The dataset contains source
    // and target node offsets followed by relationship property columns.
    static std::string constructFlatRelTablePath(const std::string& dir, const std::string& name) {
        if (isSingleDatasetPath(dir)) {
            return dir;
        }
        return joinPath(dir, "rels_" + name + std::string(LanceConstants::DATASET_SUFFIX));
    }
};

} // namespace lance_extension
} // namespace lbug
