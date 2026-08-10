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

    // Visit the parse tree. The visitor dispatches to the appropriate
    // handler based on the statement type found in the tree.
    // Note: we override visitChildren below to ensure tree walking works.
    GQLBaseVisitor::visit(&root);

    // If no specific handler produced a result, use the full input text
    // with basic keyword transformations (GQL and Cypher share most syntax).
    if (cypherResult.empty()) {
        cypherResult = query;
    }

    // Apply universal keyword transformations:
    // GQL INSERT → Cypher CREATE
    cypherResult = replaceWord(cypherResult, "INSERT", "CREATE");

    // Strip GQL keywords that LadybugDB Cypher doesn't support:
    // - PROPERTY (for open type graphs it's a no-op modifier)
    // - IF NOT EXISTS (LadybugDB Cypher doesn't support this for CREATE GRAPH)
    if (cypherResult.find("CREATE") == 0 || cypherResult.find("create") == 0) {
        cypherResult = replaceWord(cypherResult, "PROPERTY", "");
        cypherResult = replaceWord(cypherResult, "IF NOT EXISTS", "");
        // Collapse multiple spaces
        cypherResult = std::regex_replace(cypherResult, std::regex("  +"), " ");
    }

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
// Visitor: CREATE GRAPH → CREATE GRAPH <name>
//
// GQL:  CREATE [PROPERTY] GRAPH [IF NOT EXISTS] <name> [ANY] [...]
// Cypher: CREATE GRAPH <name>
//
// Strips PROPERTY, ANY, IF NOT EXISTS, OR REPLACE since LadybugDB Cypher
// doesn't support these modifiers for CREATE GRAPH.
// =============================================================================

std::any GqlToCypherTransformer::visitCreateGraphStatement(
    GQLParser::CreateGraphStatementContext *ctx) {
    if (!ctx) return {};

    // OR REPLACE is not supported — reject with a clear message
    if (ctx->OR() || ctx->REPLACE()) {
        cypherResult =
            "RETURN 'CREATE OR REPLACE GRAPH is not supported via CALL GQL' "
            "AS message";
        return {};
    }

    // Record GQL "IF NOT EXISTS" semantics — enforced by the extension at
    // rewrite time (LadybugDB Cypher has no IF NOT EXISTS for CREATE GRAPH).
    sawIfNotExistsCreateGraph = ctx->IF() && ctx->NOT();

    // Extract the graph name
    auto parentAndName = ctx->catalogGraphParentAndName();
    if (!parentAndName) {
        // Fall back to source-text based extraction if the parse tree
        // doesn't have the expected shape (e.g. for PROPERTY GRAPH variants).
        std::string fullText = sourceText(ctx);
        // Strip CREATE [OR REPLACE] [PROPERTY] prefix → CREATE GRAPH
        // Strip trailing type modifiers (ANY, etc.)
        fullText = replaceWord(fullText, "OR REPLACE", "");
        fullText = replaceWord(fullText, "PROPERTY", "");
        fullText = replaceWord(fullText, "IF NOT EXISTS", "");
        fullText = replaceWord(fullText, "ANY", "");
        // Collapse whitespace
        fullText = std::regex_replace(fullText, std::regex("\\s+"), " ");
        // Trim
        while (!fullText.empty() && std::isspace(fullText.back())) fullText.pop_back();
        while (!fullText.empty() && std::isspace(fullText.front())) fullText = fullText.substr(1);
        cypherResult = fullText;
        return {};
    }

    auto graphName = parentAndName->graphName();
    if (!graphName) {
        cypherResult = "RETURN 'Invalid CREATE GRAPH: missing graph name' AS message";
        return {};
    }

    std::string name = sourceText(graphName);
    createGraphName = name;

    // Build Cypher: CREATE GRAPH <name>
    // (IF NOT EXISTS, PROPERTY, ANY are all stripped — LadybugDB Cypher
    //  doesn't use these modifiers for open type graphs)
    std::ostringstream out;
    out << "CREATE GRAPH " << name;
    cypherResult = out.str();
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
// Default visitor: walk children to find specific statement types
// =============================================================================

std::any GqlToCypherTransformer::visitChildren(antlr4::tree::ParseTree *node) {
    // Delegate to the base class which recursively visits all children.
    // This ensures visitMatchStatement, visitInsertStatement, etc. are called
    // when the corresponding nodes are found in the parse tree.
    return GQLBaseVisitor::visitChildren(node);
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
