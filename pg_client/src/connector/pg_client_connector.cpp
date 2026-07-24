#include "connector/pg_client_connector.h"

#include <libpq-fe.h>
#include <format>

#include "common/exception/runtime.h"
#include "common/types/types.h"

namespace lbug {
namespace pg_client_extension {

PgClientConnector::PgClientConnector() : conn{nullptr} {}

PgClientConnector::~PgClientConnector() {
    disconnect();
}

void PgClientConnector::connect(const std::string& connStr) {
    if (conn) {
        disconnect();
    }
    conn = PQconnectdb(connStr.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string error = PQerrorMessage(conn);
        disconnect();
        throw common::RuntimeException(
            std::format("Failed to connect to PostgreSQL: {}", error));
    }
}

void PgClientConnector::disconnect() {
    if (conn) {
        PQfinish(conn);
        conn = nullptr;
    }
}

PgClientQueryResult PgClientConnector::executeQuery(const std::string& query) const {
    PgClientQueryResult result;
    if (!conn) {
        result.success = false;
        result.errorMessage = "Not connected to PostgreSQL";
        return result;
    }

    std::lock_guard<std::mutex> lk{mtx};
    PGresult* pgResult = PQexec(conn, query.c_str());
    if (!pgResult) {
        result.success = false;
        result.errorMessage = "Query execution returned null result";
        return result;
    }

    ExecStatusType status = PQresultStatus(pgResult);
    if (status != PGRES_TUPLES_OK) {
        result.success = false;
        result.errorMessage = PQresultErrorMessage(pgResult);
        PQclear(pgResult);
        return result;
    }

    int numCols = PQnfields(pgResult);
    int numRows = PQntuples(pgResult);

    // Get column names and types
    result.columnNames.reserve(numCols);
    result.columnTypes.reserve(numCols);
    for (int i = 0; i < numCols; i++) {
        result.columnNames.push_back(PQfname(pgResult, i));
        uint32_t oid = PQftype(pgResult, i);
        result.columnTypes.push_back(pgOidToLogicalType(oid));
    }

    // Get rows with null tracking
    result.rows.reserve(numRows);
    for (int i = 0; i < numRows; i++) {
        PgClientRow row;
        row.cells.reserve(numCols);
        for (int j = 0; j < numCols; j++) {
            bool isNull = PQgetisnull(pgResult, i, j);
            std::string val;
            if (!isNull) {
                val = PQgetvalue(pgResult, i, j);
            }
            row.cells.emplace_back(std::move(val), isNull);
        }
        result.rows.push_back(std::move(row));
    }

    result.numRows = numRows;
    result.success = true;

    PQclear(pgResult);
    return result;
}

common::LogicalType pgOidToLogicalType(uint32_t oid) {
    switch (oid) {
    case 16: // bool
        return common::LogicalType(common::LogicalTypeID::BOOL);
    case 17: // bytea
        return common::LogicalType(common::LogicalTypeID::BLOB);
    case 20: // int8
    case 21: // int2
    case 23: // int4
    case 26: // oid
        return common::LogicalType(common::LogicalTypeID::INT64);
    case 700: // float4
        return common::LogicalType(common::LogicalTypeID::FLOAT);
    case 701: // float8
        return common::LogicalType(common::LogicalTypeID::DOUBLE);
    case 1043: // varchar
    case 25: // text
    case 1042: // char
    case 142: // xml
        return common::LogicalType(common::LogicalTypeID::STRING);
    case 1082: // date
        return common::LogicalType(common::LogicalTypeID::DATE);
    case 1114: // timestamp
    case 1184: // timestamptz
        return common::LogicalType(common::LogicalTypeID::TIMESTAMP);
    case 1186: // interval
        return common::LogicalType(common::LogicalTypeID::INTERVAL);
    case 2950: // uuid
        return common::LogicalType(common::LogicalTypeID::UUID);
    case 1700: // numeric
        return common::LogicalType(common::LogicalTypeID::DOUBLE);
    default:
        // Default to string for unknown types
        return common::LogicalType(common::LogicalTypeID::STRING);
    }
}

} // namespace pg_client_extension
} // namespace lbug
