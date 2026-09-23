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

inline bool isPlaceholderSchema(duckdb::MaterializedQueryResult& result) {
    if (result.RowCount() != 1) {
        return false;
    }
    const auto nameColumn = std::find(result.names.begin(), result.names.end(), "column_name");
    const auto typeColumn = std::find(result.names.begin(), result.names.end(), "data_type");
    if (nameColumn == result.names.end() || typeColumn == result.names.end()) {
        return false;
    }
    return result.GetValue(std::distance(result.names.begin(), nameColumn), 0)
                   .GetValue<std::string>() == "__" &&
           result.GetValue(std::distance(result.names.begin(), typeColumn), 0)
                   .GetValue<std::string>() == "UNKNOWN";
}

} // namespace duckdb_extension
} // namespace lbug
