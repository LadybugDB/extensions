#pragma once

#include "gql_ast.hpp"

#include "GQLBaseVisitor.h"

// ANTLR exposes this implementation detail as a macro.
#ifdef INVALID_INDEX
#undef INVALID_INDEX
#endif

#include <string>
#include <any>

namespace lbug {
namespace gql_extension {

// GQL → Cypher transformer. Uses the ANTLR-generated GQL parser and visitor
// to walk the parse tree and produce equivalent Cypher query strings.
//
// Strategy: For common query shapes (MATCH...RETURN, INSERT), GQL and Cypher
// syntax are nearly identical. We extract the source text from the parse tree
// and apply minimal keyword-level transformations (e.g., INSERT → CREATE).
// For unsupported GQL features (CREATE GRAPH, DROP GRAPH, etc.), we return
// an informational RETURN message.
class GqlToCypherTransformer : private GQLBaseVisitor {
public:
    explicit GqlToCypherTransformer(const std::string &query_p) : query(query_p) {}

    // Walk the GQL parse tree and return an equivalent Cypher query string.
    std::string Transform(GQLParser::GqlProgramContext &root);

    // Set when the statement is "CREATE [PROPERTY] GRAPH IF NOT EXISTS <name>".
    // LadybugDB Cypher has no IF NOT EXISTS for CREATE GRAPH, so the extension
    // enforces the GQL existence semantics at bind/rewrite time.
    bool sawIfNotExistsCreateGraph = false;
    std::string createGraphName;

private:
    const std::string &query;
    std::string cypherResult;

    // --- Visitor overrides for top-level statements ---
    std::any visitMatchStatement(GQLParser::MatchStatementContext *ctx) override;
    std::any visitInsertStatement(GQLParser::InsertStatementContext *ctx) override;
    std::any visitCreateGraphStatement(GQLParser::CreateGraphStatementContext *ctx) override;
    std::any visitDropGraphStatement(GQLParser::DropGraphStatementContext *ctx) override;
    std::any visitSessionSetGraphClause(GQLParser::SessionSetGraphClauseContext *ctx) override;

    // Default — called for all other statement types.
    // Extracts the full source text since GQL and Cypher share syntax.
    std::any visitChildren(antlr4::tree::ParseTree *node) override;

    // --- Helpers ---
    std::string sourceText(antlr4::ParserRuleContext *ctx) const;

    // Case-insensitive word-boundary replacement.
    static std::string replaceWord(const std::string &str, const std::string &from,
                                   const std::string &to);
};

} // namespace gql_extension
} // namespace lbug
