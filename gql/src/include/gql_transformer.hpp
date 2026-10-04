#pragma once

#include "GQLParser.h"

// ANTLR exposes this implementation detail as a macro.
#ifdef INVALID_INDEX
#undef INVALID_INDEX
#endif

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lbug {
namespace gql_extension {

// Canonical graph type model (aliases stripped — the form Neo4j's
// GraphTypeCanonicalizer normalizes to; see THIRD_PARTY_NOTICES.md).
struct GraphTypeProp {
    std::string name;
    std::string type; // LadybugDB column type text (INT64, STRING, ...)
};

struct GraphTypeNode {
    std::string name;
    std::vector<GraphTypeProp> props;
};

struct GraphTypeEdge {
    std::string name;
    std::string from; // endpoint node type name
    std::string to;
    std::vector<GraphTypeProp> props;
};

struct GraphTypeSpec {
    std::vector<GraphTypeNode> nodes;
    std::vector<GraphTypeEdge> edges;
};

// Graph-type registry keyed by upper-cased type name. Owned by the caller
// (per-database state — see GqlExtension::load / ExtensionManager::setData).
using GraphTypeRegistry = std::map<std::string, GraphTypeSpec>;

// Schema catalog (Phase 11): logical schema paths + graph/graph-type member
// registrations, serialized into the SAME ExtensionManager slot as the
// graph-type registry (in-memory only, not WAL-backed — restart drops it).
//
// Model:
//  - `schemas`: registered logical schema paths ("/myschema", "/foo/myschema").
//  - `members`: logical path -> {physical engine name, kind} for graphs and
//    graph types created through the layer (qualified or not; unqualified
//    names register under the root path "/<name>").
//  - directory := any non-empty proper prefix of a registered schema path at
//    a '/' boundary ("/foo/myschema" registers directory "/foo"); no
//    directory entity is stored.
//  - physical name mangling for qualified paths: "_gqlsch__" + path segments
//    joined by "__" ("/foo/mygraph" -> "_gqlsch__foo__mygraph").
struct SchemaCatalog {
    enum class MemberKind { GRAPH, GRAPH_TYPE };
    struct Member {
        std::string physical;
        MemberKind kind = MemberKind::GRAPH;
    };

    std::set<std::string> schemas;
    std::map<std::string, Member> members;             // logical -> physical
    std::map<std::string, std::string> physicalToLogical; // physical -> logical

    // Proper non-empty prefixes of every schema path ('/' boundary, no root).
    std::set<std::string> directories() const;
    bool isDirectory(const std::string &path) const;
    // All members strictly under a schema path ("/foo" owns "/foo/g").
    bool hasMembersUnder(const std::string &path) const;
    void addMember(const std::string &logical, const std::string &physical,
                   MemberKind kind);
    void removeMemberByLogical(const std::string &logical);
};

// GQL → Cypher translator (the dialect bridge for LadybugDB's Cypher engine).
//
// Design: explicit top-level dispatch over the GQL parse tree — every statement
// kind is either translated, routed to an equivalent native Cypher statement,
// or rejected with a clear "GQL feature not supported" error. There is no
// whole-text pass-through fallback: a GQL statement that we do not understand
// must never reach the Cypher parser and fail there with a misleading error.
//
// Composition model: one CALL GQL statement translates to one (occasionally
// multi-statement, ';'-separated) Cypher query string, re-parsed by the
// standalone-call rewrite path in ClientContext. The last Cypher statement is
// the visible one.
//
// Function-name mappings are adapted from Neo4j's Cypher front-end
// (GQLAliasFunctionNameRewriter, Apache-2.0) and re-targeted to LadybugDB's
// own function catalog. See THIRD_PARTY_NOTICES.md.
class GqlToCypherTransformer {
public:
    // Resolves whether a named graph ("" = the session's current graph) is an
    // open ANY graph (labels live in a STRING[] column) vs a typed/tabled graph
    // (one label = table name). Returns nullopt when the graph cannot be
    // resolved. Used for label-expression translation, whose predicate form
    // differs per graph kind.
    using AnyGraphResolver = std::function<std::optional<bool>(const std::string &)>;

    explicit GqlToCypherTransformer(const std::string &query_p, GraphTypeRegistry *registry_p,
                                    SchemaCatalog *schemaCatalog_p = nullptr,
                                    AnyGraphResolver anyGraphResolver_p = nullptr)
        // Member-init order must follow declaration order (GCC -Wreorder):
        // anyGraphResolver is declared well before query/registry/schemaCatalog.
        : anyGraphResolver(std::move(anyGraphResolver_p)), query(query_p),
          registry(registry_p), schemaCatalog(schemaCatalog_p) {}

    // Absolute source span in `query`, with the replacement text to use when
    // the span is rewritten (aggregate → alias etc.).
    struct Span {
        size_t start = 0;
        size_t stop = 0; // inclusive
        std::string replacement;
    };

    // Walk the GQL parse tree and return the equivalent Cypher query string.
    // Throws common::RuntimeException on unsupported GQL constructs.
    std::string Transform(GQLParser::GqlProgramContext &root);

    // Set when the statement is "CREATE [PROPERTY] GRAPH IF NOT EXISTS <name>".
    // LadybugDB Cypher has no IF NOT EXISTS for CREATE GRAPH, so the extension
    // enforces the GQL existence semantics at bind/rewrite time.
    bool sawIfNotExistsCreateGraph = false;
    std::string createGraphName;

    // Counter for auto-generated path variable names (`_gql_pp0`, ...) used by
    // whole-pattern path-mode filters. Reset per Transform call.
    int autoPathIdx = 0;
    // Counter for auto-generated node variable names (`_gql_nl0`, ...) used by
    // compound label expressions on anonymous nodes.
    int autoLabelIdx = 0;
    // Counter for auto-generated var-length slot names (`_gql_ve0`, ...) and
    // per-repetition interior edge names (`_gql_ue0`, ...) in QPPI expansion.
    // Reset per Transform call.
    int autoVarLenIdx = 0;
    // Set whenever a synthetic pattern binding name is generated (_gql_nl /
    // _gql_pp / _gql_ve / _gql_ue / _gql_vn). A bare star projection would
    // leak those names as extra result columns, so RETURN */SELECT * is
    // rejected once this is set (red line: no silently wrong column sets).
    bool sawGeneratedBinding = false;
    // Graph kind for label-expression translation (true = ANY graph, false =
    // typed/tabled graph), resolved at Transform entry. Nullopt = unresolvable
    // (compound label expressions are then rejected).
    std::optional<bool> labelGraphIsAny;
    AnyGraphResolver anyGraphResolver;

    [[noreturn]] static void unsupported(const std::string &feature);

    // (De)serialization of the graph-type registry for per-database storage.
    static std::string serializeGraphTypes(const GraphTypeRegistry &registry);
    static GraphTypeRegistry deserializeGraphTypes(const std::string &data);

    // Schema catalog is stored in the SAME ExtensionManager slot: appended
    // after the graph-type lines as "Z\t<schema>" / "M\t<logical>\t<physical>\t<K>".
    static std::string serializeSchemaCatalog(const SchemaCatalog &catalog);
    // Parses only the schema-catalog records; graph-type lines are ignored.
    static SchemaCatalog deserializeSchemaCatalog(const std::string &data);

    // GQLSTATUS-tagged schema-catalog errors: message starts with
    // "[42000] " (schema semantic errors + the statement-combination rule)
    // or "[25G03] " (READ ONLY transaction-wrapped program rejection).
    [[noreturn]] static void schemaError(const std::string &message);

    // "_gqlsch__" + segments joined by "__": "/foo/mygraph" ->
    // "_gqlsch__foo__mygraph"; "/myschema" -> "_gqlsch__myschema".
    static std::string manglePhysical(const std::string &logicalPath);
    // Loud rejection of user identifiers inside the reserved prefix.
    static void checkReservedPrefix(const std::string &identifier);

    // True when the engine catalog already holds a graph at this logical
    // path — the mangled physical name, or (root paths) the flat out-of-layer
    // name ("CREATE GRAPH x ANY" issued outside CALL GQL is invisible to the
    // registry; the conflict check consults the catalog too).
    bool engineGraphAtLogicalPath(const std::string &logicalPath) const;

    // GQL catalog statements produce empty results; LadybugDB DDL returns a
    // message row, so catalog translations end with this zero-row tail (TCK:
    // "Then the result should be empty").
    static constexpr const char *EMPTY_RESULT_CYPHER =
        "WITH 0 AS _gql_r WHERE false RETURN _gql_r";

private:
    struct SelectItemInfo {
        antlr4::ParserRuleContext *exprCtx = nullptr;
        std::string exprText; // expression source text (unmodified)
        std::string alias;    // user alias, empty when absent
        size_t exprStart = 0;
        size_t exprStop = 0;
        bool hasAggregate = false;
    };

    // ---------- top-level dispatch ----------
    std::string translateSessionActivity(GQLParser::SessionActivityContext *ctx);
    std::string translateTransactionActivity(GQLParser::TransactionActivityContext *ctx);
    std::string translateProcedureSpecification(GQLParser::ProcedureSpecificationContext *ctx);
    std::string translateStatementBlock(GQLParser::StatementBlockContext *ctx);
    std::string translateStatement(GQLParser::StatementContext *ctx);
    // Statement body dispatch (what translateStatement did before the QPPI
    // multi-hop expansion driver wrapped it).
    std::string translateStatementImpl(GQLParser::StatementContext *ctx);
    std::string translateCompositeQuery(GQLParser::CompositeQueryStatementContext *ctx);
    std::string translateLinearCatalog(GQLParser::LinearCatalogModifyingStatementContext *ctx);
    std::string translateLinearQuery(GQLParser::LinearQueryStatementContext *ctx);
    std::string translateLinearData(GQLParser::LinearDataModifyingStatementContext *ctx);

    // ---------- statement primitives ----------
    std::string translateQueryPrimitive(GQLParser::PrimitiveQueryStatementContext *ctx);
    std::string translateDataPrimitive(GQLParser::PrimitiveDataModifyingStatementContext *ctx);
    std::string translatePrimitiveResult(GQLParser::PrimitiveResultStatementContext *ctx);

    // ---------- query primitives ----------
    std::string translateSelectStatement(GQLParser::SelectStatementContext *ctx);
    std::string translateMatchStatement(GQLParser::MatchStatementContext *ctx);
    std::string translateMatchStatement(GQLParser::MatchStatementContext *ctx,
                                        std::vector<std::string> &wheres);
    std::string translateReturnStatement(GQLParser::ReturnStatementContext *ctx,
                                         GQLParser::OrderByAndPageStatementContext *page);
    std::string translateFilterStatement(GQLParser::FilterStatementContext *ctx);
    std::string translateForStatement(GQLParser::ForStatementContext *ctx);
    std::string translateOrderByAndPage(GQLParser::OrderByAndPageStatementContext *ctx);

    // ---------- graph patterns (QPPI / path modes / search prefixes) ----------
    // `wheres` collects predicates hoisted from inline element WHERE fillers and
    // any pattern-level WHERE; the caller merges them into the clause's WHERE.
    std::string translateGraphPattern(GQLParser::GraphPatternContext *ctx,
                                      std::vector<std::string> &wheres);
    // `forcedRecType` supplies a whole-pattern mode when the statement has no
    // path-mode prefix of its own (DIFFERENT EDGES -> "TRAIL"); an explicit
    // prefix in the pattern wins over it.
    std::string translatePathPattern(GQLParser::PathPatternContext *ctx,
                                     std::vector<std::string> &wheres,
                                     const std::string &forcedRecType = "");
    // `recType` is injected into each var-length slot ("", "TRAIL", "ACYCLIC",
    // "SHORTEST", "ALL SHORTEST"). Whole-pattern mode semantics are applied by
    // translatePathPattern via a path-variable wrap (IS_TRAIL / IS_ACYCLIC).
    std::string translatePathTerm(GQLParser::PathTermContext *ctx,
                                  std::vector<std::string> &wheres,
                                  const std::string &recType,
                                  int *edgeCountOut = nullptr);
    std::string translateEdgePattern(GQLParser::EdgePatternContext *ctx,
                                     const std::string &recDetail,
                                     const std::string &renameVar = "");
    std::string translateNodePattern(GQLParser::NodePatternContext *ctx,
                                     std::vector<std::string> &wheres);
    // Splits a filler into the bracket head (variable + :labels) and the
    // trailing property map; an inline WHERE is pushed onto `wheres`.
    // `isNodePattern` enables compound label-expression translation (edge
    // type expressions stay unsupported). `renameVar`, when non-null and
    // non-empty, replaces the declared element variable in the emitted head
    // (QPPI: a quantified slot's edge binding becomes a synthetic name).
    void translateFiller(GQLParser::ElementPatternFillerContext *ctx,
                         std::vector<std::string> &wheres, std::string &head,
                         std::string &props, bool isNodePattern = true,
                         const std::string *renameVar = nullptr);
    // GQL label expression (ISO GQL feature G074) → Cypher boolean over the
    // bound variable's labels. Simple names stay as pattern labels; compound
    // expressions become WHERE predicates whose form depends on the graph
    // kind (ANY: list_contains(labels(v), ...); typed: labels(v) = ...);
    // `%` is TRUE on typed graphs (a node always has its one table label)
    // and size(labels(v)) > 0 on ANY graphs.
    std::string translateLabelExpression(GQLParser::LabelExpressionContext *ctx,
                                         const std::string &var) const;
    // Resolve `labelGraphIsAny` from the statement's graph references
    // (FROM GRAPH / USE GRAPH / SESSION SET GRAPH; empty name = current).
    void resolveLabelGraphKind(GQLParser::GqlProgramContext *root);
    // Path mode / search prefix -> iC_RecursiveType text ("", "TRAIL",
    // "ACYCLIC", "SHORTEST", "ALL SHORTEST"). Throws on unsearchable forms.
    std::string translatePathPatternPrefix(GQLParser::PathPatternPrefixContext *ctx);

    // ---------- QPPI multi-hop expansion (unnamed interior, slice A) ----------
    // One multi-edge quantified interior whose quantifier spans a range
    // ({m,n}, n > m): the statement is re-translated once per expansion count
    // and the branches are joined with UNION [ALL].
    struct QppiUnrollFactor {
        GQLParser::PfQuantifiedPathPrimaryContext *factor = nullptr;
        int lo = 0;
        int hi = 0;
    };
    // Prescan result for the statement currently being translated.
    std::vector<QppiUnrollFactor> qppiUnrollFactors;
    // Set by prescanQuantifiedPaths when any quantified/questioned factor
    // would emit synthetic pattern names / WITH re-binds (edge renames,
    // interior edge/node lists). Combined with a bare star projection
    // (SELECT * / RETURN *) in translateStatement → loud rejection: the
    // synthetic names would leak into the result as extra columns.
    bool qppiSyntheticBinding = false;
    // Element-variable declarations seen by prescanQuantifiedPaths:
    // name -> {declarations under a quantified factor, total declarations
    // in the statement}. A name with a quantified declaration and more than
    // one total declaration is a binding reuse → loud rejection; the
    // quantified subset also drives the property-access rejection (a
    // quantified binding is a LIST, so `x.v` cannot be translated).
    std::map<std::string, std::pair<int, int>> qppiBindingUse;
    // Walks the statement rejecting `x.prop` property references whose base
    // binding variable is declared under a quantified factor (B3: those
    // bind lists — property access would be a silent wrong answer or an
    // engine error with a misleading message).
    void rejectQuantifiedBindingPropertyRefs(antlr4::tree::ParseTree *node);
    // Active U-M branch: factor ctx -> chosen repetition count, consulted by
    // translatePathTerm during one whole-statement re-translation.
    std::map<antlr4::ParserRuleContext *, int> qppiUnrollCounts;
    // WITH-suffix items accumulated for the MATCH being translated
    // ("relationships(_gql_ve0) AS e", "[_gql_ue0_0, _gql_ue0_1] AS e");
    // merged into a single trailing WITH by translateMatchStatement.
    std::vector<std::string> qppiWithSuffixes;
    // Walks the statement tree classifying quantified path factors; throws
    // the Class X rejections (unbounded / lower bound 0 / range too wide) for
    // expandable multi-edge interiors and records U-M factors.
    void prescanQuantifiedPaths(antlr4::tree::ParseTree *node, bool inQuantifiedInterior);
    void classifyQuantifiedFactor(GQLParser::PathPrimaryContext *primary,
                                  GQLParser::GraphPatternQuantifierContext *quant,
                                  bool questioned, antlr4::ParserRuleContext *factorCtx);
    // Joins the per-count whole-statement translations (strips a shared
    // "USE GRAPH ...;" prefix, then UNION [ALL] the bodies).
    std::string mergeQuantifiedExpansions(const std::vector<std::string> &branches,
                                          GQLParser::StatementContext *ctx);

    // ---------- write primitives ----------
    std::string translateInsertStatement(GQLParser::InsertStatementContext *ctx);
    std::string translateSetStatement(GQLParser::SetStatementContext *ctx);
    std::string translateRemoveStatement(GQLParser::RemoveStatementContext *ctx);
    std::string translateDeleteStatement(GQLParser::DeleteStatementContext *ctx);

    // ---------- catalog / session ----------
    std::string translateCreateGraphStatement(GQLParser::CreateGraphStatementContext *ctx);
    std::string translateDropGraphStatement(GQLParser::DropGraphStatementContext *ctx);
    std::string translateCreateGraphTypeStatement(
        GQLParser::CreateGraphTypeStatementContext *ctx);
    std::string translateDropGraphTypeStatement(GQLParser::DropGraphTypeStatementContext *ctx);
    std::string translateCreateSchemaStatement(GQLParser::CreateSchemaStatementContext *ctx);
    std::string translateDropSchemaStatement(GQLParser::DropSchemaStatementContext *ctx);
    std::string translateSessionSetGraphClause(GQLParser::SessionSetGraphClauseContext *ctx);

    // ---------- schema-path / qualified-name helpers ----------
    // Logical path from a catalogSchemaParentAndName context ("/foo/myschema"),
    // whitespace-normalized; validates the reserved prefix per segment.
    std::string schemaPathText(GQLParser::CatalogSchemaParentAndNameContext *ctx);
    // Removes whitespace outside quoted spans; keeps quoted content intact.
    static std::string normalizePathWhitespace(const std::string &raw);
    // Rejects any path segment whose identifier starts with "_gqlsch__".
    static void checkReservedInPath(const std::string &path);
    // Logical path ("/foo/g") -> registered-or-mangled physical name.
    // createMapping also registers the logical->physical member entry.
    std::string resolvePhysical(const std::string &logicalPath, bool createMapping,
                                SchemaCatalog::MemberKind kind);
    // Rewrites a graphExpression (USE GRAPH /foo/g, SESSION SET GRAPH, FROM ...)
    // to the physical graph name; plain names unchanged.
    std::string rewriteGraphExpression(GQLParser::GraphExpressionContext *ctx);
    // Absolute logical path for a qualified catalog object reference. The
    // root-anchored form keeps its spelling byte-for-byte; the dotted form
    // `(objectName PERIOD)+ graphName` is rebuilt segment-by-segment from
    // the parse tree ("/dir/g" for `dir.g`, delimited segments stripped
    // after the tree split, so a backticked name may contain periods).
    // Relative/predefined/parameter schema references reject loudly.
    // `whole` is the enclosing parent+name context (raw-text source for the
    // root-anchored branch), `parent` the (non-null) parent reference.
    std::string qualifiedCatalogPath(antlr4::ParserRuleContext *whole,
                                     GQLParser::CatalogObjectParentReferenceContext *parent,
                                     antlr4::ParserRuleContext *finalName);
    // Graph type reference -> physical type name ("$param" rejected; qualified
    // references resolved through the schema catalog).
    std::string graphTypeRefName(GQLParser::GraphTypeReferenceContext *ref);
    // Rejects schema catalog statements combined with anything else
    // (NEXT chains, juxtaposed catalog statements, other statement kinds).
    void checkSchemaStatementAlone(antlr4::ParserRuleContext *ctx);

    // ---------- helpers ----------
    std::string sourceText(antlr4::ParserRuleContext *ctx) const;
    std::string renderSelectItems(const std::vector<SelectItemInfo> &items,
                                  const std::vector<Span> &replacements) const;

    // ---------- Q2 comparison bridge (ANY graphs) ----------
    // On an open ANY graph a property comparison is a text-order comparison at
    // the engine — a silent wrong answer for JSON values. `emitValueExpression`
    // rewrites GQL `<` `<=` `>` `>=` to the extension's total-order predicates
    // _gql_lt/_gql_le/_gql_gt/_gql_ge (ANY x ANY -> BOOL), recursing through
    // every nested expression position. Typed graphs and unresolvable graph
    // kinds return the source text byte-for-byte (no behavior change).
    std::string emitValueExpression(GQLParser::ValueExpressionContext *ctx) const;
    // A single <comparisonExprAlt>: emit both operands recursively, then the
    // total-order call for the operator (equality stays textual until B2).
    std::string emitComparison(GQLParser::ComparisonExprAltContext *ctx) const;
    // One `v IS [NOT] LABELED <labelExpr>` / `v:<labelExpr>` predicate → the
    // Cypher label predicate from translateLabelExpression (negated as a
    // whole for IS NOT), on both graph kinds.
    std::string emitLabeledPredicate(GQLParser::LabeledPredicateContext *ctx) const;
    // Q5-3: a typed-graph list literal holding a non-literal element →
    // `_gql_list_checked(e1, ..., en)`; each element is emitted recursively
    // so nested rewrite targets compose inside the call.
    std::string emitCheckedList(
        GQLParser::ListValueConstructorByEnumerationContext *list) const;
    // Source text of `node` with every top-level rewrite target replaced by
    // its emitted form (right-to-left splices over absolute source offsets):
    // comparisons on ANY graphs, IS [NOT] LABELED predicates everywhere,
    // guarded list literals on typed graphs.
    // Descending stops at aggregate boundaries so span-based aggregate alias
    // rewrites (replaceExprs over raw source text) keep matching.
    std::string emitRewrittenExpr(antlr4::tree::ParseTree *node) const;
    // Any expression-bearing subtree (search conditions, projection items,
    // HAVING) → emitRewrittenExpr.
    std::string emitExpr(antlr4::tree::ParseTree *node) const;
    // Renders an ORDER BY clause with `pairs` (projected expression -> output
    // alias) applied. On ANY graphs each mapped sort key is wrapped in
    // _gql_sortkey so JSON values sort under GQL's total order, not text order.
    std::string renderOrderBy(
        GQLParser::OrderByClauseContext *ctx,
        const std::vector<std::pair<std::string, std::string>> &pairs) const;

    // Reject GQL-only pattern features (quantified paths, path modes/search
    // prefixes, exotic label expressions) with a named error.
    // `allowLabelExpr` = label-expression operators are translated elsewhere
    // (filler label expressions) rather than rejected.
    void checkPatternSupported(antlr4::ParserRuleContext *ctx, bool allowLabelExpr = false);

    // Collect absolute spans of every aggregateFunction subtree under ctx.
    static void collectAggregates(antlr4::tree::ParseTree *node,
                                  std::vector<Span> &out, int &counter);
    static bool containsAggregate(antlr4::tree::ParseTree *node);

    // Literal-aware identifier rewrite (GQL function spellings → LadybugDB).
    static std::string mapIdentifiers(const std::string &text);
    // Literal-aware operator rewrite (`||` → `+`).
    static std::string mapOperators(const std::string &text);
    // Apply expression-level mappings to a source-text fragment.
    std::string finishExpr(const std::string &text) const;

    static std::string snippet(const std::string &text);
    static std::string replaceWord(const std::string &str, const std::string &from,
                                   const std::string &to);

    const std::string &query;
    GraphTypeRegistry *registry;
    SchemaCatalog *schemaCatalog;
};

} // namespace gql_extension
} // namespace lbug
