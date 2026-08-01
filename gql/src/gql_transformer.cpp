#include "gql_transformer.hpp"

#include "GQLLexer.h"
#include "GQLParser.h"
#include "antlr4-runtime.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace lbug {
namespace gql_extension {

// =============================================================================
// Public entry point
// =============================================================================

std::string GqlToCypherTransformer::Transform(GQLParser::GqlProgramContext &root) {
    cypherResult.clear();

    // Visit the parse tree. The visitor will dispatch to the appropriate
    // handler based on the statement type.
    visit(&root);

    // If no specific handler produced a result, use the full input text
    // with basic keyword transformations (GQL and Cypher share most syntax).
    if (cypherResult.empty()) {
        cypherResult = query;
    }

    // Apply universal keyword transformations:
    // GQL INSERT → Cypher CREATE
    cypherResult = replaceWord(cypherResult, "INSERT", "CREATE");

    return cypherResult;
}

// =============================================================================
// Visitor: MATCH statement
// =============================================================================

std::any GqlToCypherTransformer::visitMatchStatement(
    GQLParser::MatchStatementContext * /*ctx*/) {
    // MATCH statements in GQL are syntactically almost identical to Cypher.
    // Let the default handler (full text pass-through) handle this.
    // We don't set cypherResult here, so the fallback in Transform() is used.
    return {};
}

// =============================================================================
// Visitor: INSERT statement → CREATE
// =============================================================================

std::any GqlToCypherTransformer::visitInsertStatement(
    GQLParser::InsertStatementContext *ctx) {
    if (!ctx) return {};

    // Extract the full source text of the INSERT statement
    std::string text = sourceText(ctx);
    cypherResult = text;
    return {};
}

// =============================================================================
// Visitor: CREATE GRAPH (unsupported via CALL GQL)
// =============================================================================

std::any GqlToCypherTransformer::visitCreateGraphStatement(
    GQLParser::CreateGraphStatementContext * /*ctx*/) {
    cypherResult = "RETURN 'CREATE GRAPH must be executed directly, not via CALL GQL' "
                   "AS message";
    return {};
}

// =============================================================================
// Visitor: DROP GRAPH (unsupported via CALL GQL)
// =============================================================================

std::any GqlToCypherTransformer::visitDropGraphStatement(
    GQLParser::DropGraphStatementContext * /*ctx*/) {
    cypherResult = "RETURN 'DROP GRAPH must be executed directly, not via CALL GQL' "
                   "AS message";
    return {};
}

// =============================================================================
// Visitor: SESSION SET GRAPH (unsupported via CALL GQL)
// =============================================================================

std::any GqlToCypherTransformer::visitSessionSetGraphClause(
    GQLParser::SessionSetGraphClauseContext * /*ctx*/) {
    cypherResult =
        "RETURN 'SESSION SET GRAPH must be executed directly, not via CALL GQL' "
        "AS message";
    return {};
}

// =============================================================================
// Default visitor: pass through
// =============================================================================

std::any GqlToCypherTransformer::visitChildren(antlr4::tree::ParseTree * /*node*/) {
    // Default behavior: don't set cypherResult.
    // The Transform() method will use the full query text as fallback.
    return {};
}

// =============================================================================
// Helpers
// =============================================================================

std::string GqlToCypherTransformer::sourceText(antlr4::ParserRuleContext *ctx) const {
    if (!ctx) return "";

    auto startToken = ctx->getStart();
    auto stopToken = ctx->getStop();

    if (!startToken || !stopToken) return "";

    size_t startIdx = startToken->getStartIndex();
    size_t stopIdx = stopToken->getStopIndex();

    if (startIdx > query.size() || stopIdx + 1 > query.size() || stopIdx < startIdx) {
        return "";
    }

    return query.substr(startIdx, stopIdx - startIdx + 1);
}

std::string GqlToCypherTransformer::replaceWord(const std::string &str,
                                                  const std::string &from,
                                                  const std::string &to) {
    // Case-insensitive word-boundary replacement using regex
    std::regex wordRe("\\b" + from + "\\b", std::regex::icase);
    return std::regex_replace(str, wordRe, to);
}

} // namespace gql_extension
} // namespace lbug
