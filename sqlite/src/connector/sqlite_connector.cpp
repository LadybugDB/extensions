#include "connector/sqlite_connector.h"

#include "common/exception/runtime.h"
#include "sqlite3.h"
#include <format>

namespace lbug {
namespace sqlite_extension {

std::string sqliteStorageClassToString(SqliteStorageClass storage) {
    switch (storage) {
    case SqliteStorageClass::NULL_VALUE:
        return "null";
    case SqliteStorageClass::INTEGER:
        return "integer";
    case SqliteStorageClass::REAL:
        return "real";
    case SqliteStorageClass::TEXT:
        return "text";
    case SqliteStorageClass::BLOB:
        return "blob";
    }
    return "null";
}

SqliteConnector::SqliteConnector() : db{nullptr} {}

SqliteConnector::~SqliteConnector() {
    disconnect();
}

void SqliteConnector::connect(const std::string& dbPath) {
    if (db) {
        disconnect();
    }
    sqlite3* handle = nullptr;
    // Read-only: ladybug never writes to attached SQLite files. Writes go to
    // SQLite itself and reach ladybug through replication (see docs on the
    // LTX-based setup). A read-only open also refuses to create a missing
    // file, surfacing typos instead of silently attaching an empty database.
    int rc = sqlite3_open_v2(dbPath.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::string error = handle ? sqlite3_errmsg(handle) : sqlite3_errstr(rc);
        if (handle) {
            sqlite3_close(handle);
        }
        throw common::RuntimeException(
            std::format("Failed to open SQLite database '{}': {}", dbPath, error));
    }
    // Tolerate a concurrent writer holding locks for a short while.
    sqlite3_busy_timeout(handle, 5000);
    db = handle;
    this->dbPath = dbPath;
}

void SqliteConnector::disconnect() {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
    }
}

SqliteQueryResult SqliteConnector::executeQuery(const std::string& query) const {
    SqliteQueryResult result;
    if (!db) {
        result.success = false;
        result.errorMessage = "Not connected to SQLite";
        return result;
    }
    std::lock_guard<std::mutex> lk{mtx};
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        result.success = false;
        result.errorMessage = sqlite3_errmsg(db);
        return result;
    }
    int numCols = sqlite3_column_count(stmt);
    result.columnNames.reserve(numCols);
    for (int i = 0; i < numCols; i++) {
        const char* name = sqlite3_column_name(stmt, i);
        result.columnNames.emplace_back(name ? name : "");
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SqliteRow row;
        row.cells.reserve(numCols);
        for (int i = 0; i < numCols; i++) {
            SqliteCell cell;
            int colType = sqlite3_column_type(stmt, i);
            switch (colType) {
            case SQLITE_INTEGER: {
                cell.storage = SqliteStorageClass::INTEGER;
                cell.intVal = sqlite3_column_int64(stmt, i);
                cell.textVal = std::to_string(cell.intVal);
            } break;
            case SQLITE_FLOAT: {
                cell.storage = SqliteStorageClass::REAL;
                cell.realVal = sqlite3_column_double(stmt, i);
                // SQLite's own rendering gives the shortest round-trip
                // representation (e.g. "5.872"), which is what a STRING read
                // of the value must return.
                const unsigned char* text = sqlite3_column_text(stmt, i);
                int bytes = sqlite3_column_bytes(stmt, i);
                if (text) {
                    cell.textVal.assign(reinterpret_cast<const char*>(text), bytes);
                }
            } break;
            case SQLITE_TEXT: {
                cell.storage = SqliteStorageClass::TEXT;
                const unsigned char* text = sqlite3_column_text(stmt, i);
                int bytes = sqlite3_column_bytes(stmt, i);
                if (text) {
                    cell.textVal.assign(reinterpret_cast<const char*>(text), bytes);
                }
            } break;
            case SQLITE_BLOB: {
                cell.storage = SqliteStorageClass::BLOB;
                const void* blob = sqlite3_column_blob(stmt, i);
                int bytes = sqlite3_column_bytes(stmt, i);
                if (blob && bytes > 0) {
                    const auto* begin = static_cast<const uint8_t*>(blob);
                    cell.blobVal.assign(begin, begin + bytes);
                }
            } break;
            default: {
                cell.storage = SqliteStorageClass::NULL_VALUE;
            } break;
            }
            row.cells.push_back(std::move(cell));
        }
        result.rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) {
        result.success = false;
        result.errorMessage = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return result;
    }
    sqlite3_finalize(stmt);
    result.numRows = result.rows.size();
    result.success = true;
    return result;
}

std::vector<std::string> SqliteConnector::listTables() const {
    auto result = executeQuery(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
        "ORDER BY name");
    if (!result.success) {
        throw common::RuntimeException(
            std::format("Failed to list SQLite tables: {}", result.errorMessage));
    }
    std::vector<std::string> tables;
    tables.reserve(result.rows.size());
    for (auto& row : result.rows) {
        tables.push_back(row.cells[0].textVal);
    }
    return tables;
}

std::vector<SqliteConnector::ColumnInfo> SqliteConnector::getTableInfo(
    const std::string& tableName) const {
    auto result = executeQuery(std::format("PRAGMA table_info({})", quoteIdentifier(tableName)));
    if (!result.success) {
        throw common::RuntimeException(std::format("Failed to read schema of SQLite table '{}': {}",
            tableName, result.errorMessage));
    }
    // PRAGMA table_info yields (cid, name, type, notnull, dflt_value, pk).
    std::vector<ColumnInfo> columns;
    columns.reserve(result.rows.size());
    for (auto& row : result.rows) {
        ColumnInfo info;
        info.name = row.cells[1].textVal;
        if (row.cells[2].storage != SqliteStorageClass::NULL_VALUE) {
            info.declaredType = row.cells[2].textVal;
        }
        if (row.cells[5].storage == SqliteStorageClass::INTEGER) {
            info.pkOrder = static_cast<int>(row.cells[5].intVal);
        }
        columns.push_back(std::move(info));
    }
    return columns;
}

std::vector<SqliteConnector::ForeignKeyInfo> SqliteConnector::getForeignKeys(
    const std::string& tableName) const {
    auto result =
        executeQuery(std::format("PRAGMA foreign_key_list({})", quoteIdentifier(tableName)));
    if (!result.success) {
        return {};
    }
    // PRAGMA foreign_key_list yields (id, seq, table, from, to, on_update,
    // on_delete, match).
    std::vector<ForeignKeyInfo> keys;
    for (auto& row : result.rows) {
        ForeignKeyInfo info;
        info.referencedTable = row.cells[2].textVal;
        info.fromColumn = row.cells[3].textVal;
        if (row.cells[4].storage != SqliteStorageClass::NULL_VALUE) {
            info.toColumn = row.cells[4].textVal;
        }
        keys.push_back(std::move(info));
    }
    return keys;
}

std::string SqliteConnector::quoteIdentifier(const std::string& name) {
    std::string out;
    out.reserve(name.size() + 2);
    out.push_back('"');
    for (char c : name) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string SqliteConnector::escapeSingleQuotes(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\'') {
            out.push_back('\'');
        }
        out.push_back(c);
    }
    return out;
}

} // namespace sqlite_extension
} // namespace lbug
