#include "connector/lance_connector.h"

#include "common/exception/runtime.h"

namespace lbug {
namespace lance_extension {

void LanceConnector::connect(const std::string& /*dbPath*/, const std::string& /*catalogName*/,
    const std::string& /*schemaName*/, main::ClientContext* context) {
    // Creates an in-memory duckdb instance, then loads the lance extension (the
    // same engine as lance-duckdb: a DuckDB core extension) and httpfs for
    // object-store URIs.
    instance = std::make_unique<duckdb::DuckDB>(nullptr);
    connection = std::make_unique<duckdb::Connection>(*instance);
    executeQuery("INSTALL lance;");
    executeQuery("LOAD lance;");
    executeQuery("INSTALL httpfs;");
    executeQuery("LOAD httpfs;");
    initRemoteFSSecrets(context);
}

std::unique_ptr<duckdb::Connection> LanceConnector::newConnection(main::ClientContext* context) {
    DASSERT(instance != nullptr);
    auto conn = std::make_unique<duckdb::Connection>(*instance);
    // LOAD is idempotent, and object-store secrets configured by connect() live on the
    // shared instance, so every connection inherits them.
    auto result = conn->Query("LOAD lance;");
    if (result->HasError()) {
        throw common::RuntimeException{
            "Failed to load lance extension on scan connection: " + result->GetError()};
    }
    return conn;
}

std::unique_ptr<duckdb::QueryResult> LanceConnector::executeOn(duckdb::Connection& connection,
    const std::string& query) const {
    auto result = connection.Query(query);
    if (result->HasError()) {
        throw common::Exception{result->GetError()};
    }
    return result;
}

std::string LanceConnector::escapeLiteral(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    return out;
}

std::string LanceConnector::quoteIdent(const std::string& ident) {
    std::string out = "\"";
    for (char c : ident) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

std::string LanceConnector::scanAllQuery(const std::string& datasetPath) {
    return "SELECT * FROM '" + escapeLiteral(datasetPath) + "'";
}

std::string LanceConnector::countRowsQuery(const std::string& datasetPath) {
    return "SELECT COUNT(*) FROM '" + escapeLiteral(datasetPath) + "'";
}

std::string LanceConnector::schemaQuery(const std::string& datasetPath) {
    return scanAllQuery(datasetPath) + " LIMIT 0";
}

} // namespace lance_extension
} // namespace lbug
