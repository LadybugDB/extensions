#pragma once

#include <algorithm>
#include <iterator>
#include <string>

#include "connector/duckdb_connector.h"

namespace lbug {
namespace duckdb_extension {

inline std::string escapeSingleQuotes(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (auto c : value) {
        if (c == '\'') {
            result += "''";
        } else {
            result += c;
        }
    }
    return result;
}

inline std::string quoteDuckDBIdentifier(const std::string& value) {
    std::string result = "\"";
    for (auto c : value) {
        result += c;
        if (c == '"') {
            result += '"';
        }
    }
    result += '"';
    return result;
}

// In DuckDB 2.0.0 QueryResult is a forward-only stream: Fetch() consumes.
// Take the already-fetched first chunk (plus names/row count) instead of
// fetching again so callers can keep streaming the remaining chunks.
inline bool isPlaceholderSchema(const std::vector<duckdb::Identifier>& names,
    const duckdb::DataChunk& firstChunk, duckdb::idx_t totalRowCount) {
    if (totalRowCount != 1 || firstChunk.size() != 1) {
        return false;
    }
    const auto nameColumn = std::find(names.begin(), names.end(), "column_name");
    const auto typeColumn = std::find(names.begin(), names.end(), "data_type");
    if (nameColumn == names.end() || typeColumn == names.end()) {
        return false;
    }
    return firstChunk.GetValue(std::distance(names.begin(), nameColumn), 0)
                   .GetValue<std::string>() == "__" &&
           firstChunk.GetValue(std::distance(names.begin(), typeColumn), 0)
                   .GetValue<std::string>() == "UNKNOWN";
}

} // namespace duckdb_extension
} // namespace lbug
