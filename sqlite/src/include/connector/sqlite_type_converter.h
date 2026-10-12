#pragma once

#include <string>

#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/types/types.h"

namespace lbug {
namespace sqlite_extension {

// Maps a SQLite declared column type (as reported by PRAGMA table_info, i.e.
// the CREATE TABLE decltype) to a ladybug LogicalType.
//
// SQLite uses type affinity rather than rigid types, so the mapping follows
// affinity rules (https://sqlite.org/datatype3.html) with two refinements
// that preserve the behaviour of the previous DuckDB-backed implementation:
//   - BOOLEAN/BOOL map to INT64 (SQLite gives them NUMERIC affinity and the
//     DuckDB sqlite extension reported them as BIGINT).
//   - An empty/NULL decltype (BLOB affinity) maps to BLOB.
// Unknown types fall back to STRING so that exotic decltypes stay readable
// instead of failing to attach.
inline common::LogicalType sqliteDeclaredTypeToLogicalType(std::string declType) {
    declType = common::StringUtils::ltrim(declType);
    declType = common::StringUtils::rtrim(declType);
    auto upper = declType;
    common::StringUtils::toUpper(upper);
    if (upper.empty() || upper == "NULL" || upper == "BLOB") {
        return common::LogicalType(common::LogicalTypeID::BLOB);
    }
    if (upper == "DATE" || upper.starts_with("DATE(")) {
        return common::LogicalType(common::LogicalTypeID::DATE);
    }
    if (upper.find("DATETIME") != std::string::npos ||
        upper.find("TIMESTAMP") != std::string::npos) {
        return common::LogicalType(common::LogicalTypeID::TIMESTAMP);
    }
    if (upper.find("INT") != std::string::npos) {
        return common::LogicalType(common::LogicalTypeID::INT64);
    }
    if (upper == "BOOLEAN" || upper == "BOOL") {
        return common::LogicalType(common::LogicalTypeID::INT64);
    }
    if (upper.find("CHAR") != std::string::npos || upper.find("CLOB") != std::string::npos ||
        upper.find("TEXT") != std::string::npos) {
        return common::LogicalType(common::LogicalTypeID::STRING);
    }
    if (upper.find("REAL") != std::string::npos || upper.find("FLOA") != std::string::npos ||
        upper.find("DOUB") != std::string::npos) {
        return common::LogicalType(common::LogicalTypeID::DOUBLE);
    }
    if (upper.find("NUMERIC") != std::string::npos || upper.find("DECIMAL") != std::string::npos) {
        return common::LogicalType(common::LogicalTypeID::DOUBLE);
    }
    // Anything else (UUID, JSON, ...) is read as text.
    return common::LogicalType(common::LogicalTypeID::STRING);
}

} // namespace sqlite_extension
} // namespace lbug
