#include "connector/duckdb_connector.h"

#include <algorithm>
#include <format>

#include "common/exception/binder.h"

namespace lbug {
namespace duckdb_extension {

std::unique_ptr<duckdb::QueryResult> DuckDBConnector::executeQuery(
    std::string query) const {
    DASSERT(instance != nullptr && connection != nullptr);
    auto result = connection->Query(query);
    if (result->HasError()) {
        throw common::Exception{result->GetError()};
    }
    return result;
}

void DuckDBConnector::validateSelectQuery(const std::string& query) const {
    DASSERT(instance != nullptr && connection != nullptr);
    auto statements = connection->ExtractStatements(query);
    const auto isSelectOnly = !statements.empty() &&
        std::all_of(statements.begin(), statements.end(), [](const auto& stmt) {
            return stmt->type == duckdb::StatementType::SELECT_STATEMENT;
        });
    if (!isSelectOnly) {
        throw common::BinderException{std::format("No fields returned by query \"{}\" - "
                                                 "the query must be a SELECT statement that "
                                                 "returns at least one column",
            query)};
    }
}

} // namespace duckdb_extension
} // namespace lbug
