#pragma once

#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/types/types.h"

namespace lbug {
namespace pg_client_extension {

struct PgClientCell {
    std::string value;
    bool isNull;

    PgClientCell() : isNull{true} {}
    PgClientCell(std::string value, bool isNull)
        : value{std::move(value)}, isNull{isNull} {}
};

struct PgClientRow {
    std::vector<PgClientCell> cells;
};

struct PgClientQueryResult {
    std::vector<std::string> columnNames;
    std::vector<common::LogicalType> columnTypes;
    std::vector<PgClientRow> rows;
    uint64_t numRows;
    bool success;
    std::string errorMessage;

    PgClientQueryResult() : numRows{0}, success{false} {}
};

class PgClientConnector {
public:
    PgClientConnector();
    ~PgClientConnector();

    void connect(const std::string& connStr);
    void disconnect();

    bool isConnected() const { return conn != nullptr; }

    PgClientQueryResult executeQuery(const std::string& query) const;

    PGconn* getPGconn() const { return conn; }

private:
    PGconn* conn;
    mutable std::mutex mtx;
};

common::LogicalType pgOidToLogicalType(uint32_t oid);

} // namespace pg_client_extension
} // namespace lbug
