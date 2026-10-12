#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common/types/types.h"

struct sqlite3;

namespace lbug {
namespace sqlite_extension {

// Storage class of a single SQLite value, mirroring sqlite3_column_type().
enum class SqliteStorageClass : uint8_t {
    NULL_VALUE = 0,
    INTEGER = 1,
    REAL = 2,
    TEXT = 3,
    BLOB = 4,
};

std::string sqliteStorageClassToString(SqliteStorageClass storage);

struct SqliteCell {
    SqliteStorageClass storage = SqliteStorageClass::NULL_VALUE;
    int64_t intVal = 0;
    double realVal = 0.0;
    // Canonical text representation. For TEXT this is the value itself; for
    // INTEGER/REAL it is SQLite's own shortest round-trip rendering (used
    // when a column is read as STRING, e.g. under sqlite_all_varchar).
    std::string textVal;
    std::vector<uint8_t> blobVal;
};

struct SqliteRow {
    std::vector<SqliteCell> cells;
};

struct SqliteQueryResult {
    std::vector<std::string> columnNames;
    std::vector<SqliteRow> rows;
    uint64_t numRows = 0;
    bool success = false;
    std::string errorMessage;
};

// Thin wrapper around a single sqlite3 connection. The database file is
// opened read-only: ladybug never writes to attached SQLite files (writes go
// to SQLite itself; see docs on LTX-based replication). All queries are fully
// materialized under a mutex so one connection can serve parallel scan
// workers, mirroring the pg_client connector.
class SqliteConnector {
public:
    SqliteConnector();
    ~SqliteConnector();

    SqliteConnector(const SqliteConnector&) = delete;
    SqliteConnector& operator=(const SqliteConnector&) = delete;

    void connect(const std::string& dbPath);
    void disconnect();

    bool isConnected() const { return db != nullptr; }

    // Executes the query and materializes every row. Throws
    // common::Exception when the query cannot be prepared or stepped.
    SqliteQueryResult executeQuery(const std::string& query) const;

    struct ColumnInfo {
        std::string name;
        std::string declaredType;
    };

    std::vector<std::string> listTables() const;
    std::vector<ColumnInfo> getTableInfo(const std::string& tableName) const;

    struct ForeignKeyInfo {
        std::string fromColumn;
        std::string referencedTable;
        std::string toColumn;
    };
    std::vector<ForeignKeyInfo> getForeignKeys(const std::string& tableName) const;

    static std::string quoteIdentifier(const std::string& name);
    static std::string escapeSingleQuotes(const std::string& value);

private:
    sqlite3* db;
    mutable std::mutex mtx;
};

} // namespace sqlite_extension
} // namespace lbug
