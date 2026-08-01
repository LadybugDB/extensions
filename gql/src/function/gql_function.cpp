#include "function/gql_function.h"

#include "GQLLexer.h"
#include "GQLParser.h"
#include "antlr4-runtime.h"
#include "gql_transformer.hpp"

#include "common/exception/runtime.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "main/client_context.h"

#include <format>

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

    GqlBindData(std::string cypherQuery)
        : TableFuncBindData{binder::expression_vector{}, 0 /* maxOffset */},
          cypherQuery{std::move(cypherQuery)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GqlBindData>(*this);
    }
};

// =============================================================================
// Rewrite function: returns the Cypher string to be executed in place of CALL
// =============================================================================

static std::string rewriteFunc(ClientContext & /*context*/,
                                const TableFuncBindData &bindData) {
    auto *gqlData = bindData.constPtrCast<GqlBindData>();
    return gqlData->cypherQuery;
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

    // Check for parse errors
    if (parser.getNumberOfSyntaxErrors() > 0) {
        throw common::RuntimeException{
            std::format("Failed to parse GQL query: {}", trimmed)};
    }

    // Transform GQL AST to Cypher
    GqlToCypherTransformer transformer(trimmed);
    auto cypherQuery = transformer.Transform(*tree);

    if (cypherQuery.empty()) {
        throw common::RuntimeException{
            std::format("Failed to transform GQL to Cypher: {}", trimmed)};
    }

    return std::make_unique<GqlBindData>(std::move(cypherQuery));
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
