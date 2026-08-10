#include "function/gql_function.h"

#include "GQLLexer.h"
#include "GQLParser.h"
#include "antlr4-runtime.h"
#include "gql_transformer.hpp"

#include "catalog/catalog.h"
#include "catalog/catalog_entry/graph_catalog_entry.h"
#include "common/exception/runtime.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "main/client_context.h"
#include "main/database.h"
#include "transaction/transaction.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <vector>

namespace lbug {
namespace gql_extension {

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::main;

// =============================================================================
// Bind data: holds the Cypher query produced by GQL→Cypher translation
// =============================================================================

struct GqlBindData final : TableFuncBindData {
    std::string cypherQuery;
    // True when the statement is "CREATE [PROPERTY] GRAPH IF NOT EXISTS <name>".
    // LadybugDB Cypher has no IF NOT EXISTS for CREATE GRAPH, so the existence
    // semantics are enforced at rewrite time.
    bool ifNotExistsCreateGraph;
    std::string graphName;

    GqlBindData(std::string cypherQuery, bool ifNotExistsCreateGraph = false,
                std::string graphName = {})
        : TableFuncBindData{binder::expression_vector{}, 0 /* maxOffset */},
          cypherQuery{std::move(cypherQuery)},
          ifNotExistsCreateGraph{ifNotExistsCreateGraph},
          graphName{std::move(graphName)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GqlBindData>(*this);
    }
};

// =============================================================================
// Rewrite function: returns the Cypher string to be executed in place of CALL
// =============================================================================

static std::string rewriteFunc(ClientContext &context,
                                const TableFuncBindData &bindData) {
    auto *gqlData = bindData.constPtrCast<GqlBindData>();

    // Emulate GQL "CREATE GRAPH IF NOT EXISTS" semantics: if a graph with the
    // target name already exists, rewrite the statement to a no-op.
    if (gqlData->ifNotExistsCreateGraph && !gqlData->graphName.empty()) {
        auto *transaction = transaction::Transaction::Get(context);
        auto *catalog = context.getDatabase()->getCatalog();
        auto graphEntries = catalog->getGraphEntries(transaction);
        for (auto *entry : graphEntries) {
            if (entry->getName() == gqlData->graphName) {
                return "RETURN 0"; // graph already exists — nothing to do
            }
        }
    }

    return gqlData->cypherQuery;
}

// LadybugDB-specific DDL that the GQL grammar doesn't cover. These are passed
// through verbatim so statements such as CALL GQL("CREATE NODE TABLE ...")
// work unchanged. Everything else that fails GQL parsing is a genuine GQL
// syntax error and must not be forwarded to the Cypher parser.
static bool isDdlPassThrough(const std::string &query) {
    static const std::vector<std::string> prefixes = {
        "CREATE NODE TABLE", "CREATE REL TABLE", "CREATE EDGE TABLE",
        "DROP NODE TABLE",   "DROP REL TABLE",   "DROP EDGE TABLE",
        "ALTER NODE TABLE",  "ALTER REL TABLE",  "ALTER EDGE TABLE",
        "COPY ",             "LOAD FROM",
    };
    std::string upper(query.size(), '\0');
    std::transform(query.begin(), query.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    for (const auto &prefix : prefixes) {
        if (upper.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Bind function: parses GQL and produces Cypher
// =============================================================================

static std::unique_ptr<TableFuncBindData> bindFunc(ClientContext *context,
                                                    const TableFuncBindInput *input) {
    (void)context; // Not needed during bind — the Cypher query is executed later

    // Get the GQL query string from the first parameter
    auto gqlQuery = input->getLiteralVal<std::string>(0);

    // Strip trailing semicolon and whitespace
    std::string trimmed = gqlQuery;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed.pop_back();
    }

    if (trimmed.empty()) {
        throw common::RuntimeException{"GQL query string must not be empty"};
    }

    // Parse GQL using ANTLR
    antlr4::ANTLRInputStream antlrInput(trimmed);
    GQLLexer lexer(&antlrInput);
    antlr4::CommonTokenStream tokens(&lexer);
    GQLParser parser(&tokens);

    // Remove default error listeners to avoid printing to stderr
    lexer.removeErrorListeners();
    parser.removeErrorListeners();

    auto tree = parser.gqlProgram();

    // If GQL parsing fails, only LadybugDB-specific DDL (CREATE NODE TABLE,
    // CREATE REL TABLE, etc.) is passed through as raw Cypher. Anything else is
    // a genuine GQL syntax error and gets a GQL-specific error message.
    if (parser.getNumberOfSyntaxErrors() > 0) {
        if (isDdlPassThrough(trimmed)) {
            return std::make_unique<GqlBindData>(trimmed);
        }
        throw common::RuntimeException{"Failed to parse GQL query: " + trimmed};
    }

    // Transform GQL AST to Cypher
    GqlToCypherTransformer transformer(trimmed);
    auto cypherQuery = transformer.Transform(*tree);

    if (cypherQuery.empty()) {
        return std::make_unique<GqlBindData>(trimmed);
    }

    return std::make_unique<GqlBindData>(std::move(cypherQuery),
        transformer.sawIfNotExistsCreateGraph, transformer.createGraphName);
}

// =============================================================================
// Function set
// =============================================================================

function_set GqlFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<TableFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::STRING});
    func->tableFunc = TableFunction::emptyTableFunc;
    func->bindFunc = bindFunc;
    func->initSharedStateFunc = TableFunction::initEmptySharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->rewriteFunc = rewriteFunc;
    func->canParallelFunc = [] { return false; };
    functionSet.push_back(std::move(func));
    return functionSet;
}

} // namespace gql_extension
} // namespace lbug
