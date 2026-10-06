#include "gql_transformer.hpp"

#include "common/exception/runtime.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <utility>

namespace lbug {
namespace gql_extension {

// =============================================================================
// Errors
// =============================================================================

void GqlToCypherTransformer::unsupported(const std::string &feature) {
    throw common::RuntimeException{"GQL feature not supported: " + feature};
}

void GqlToCypherTransformer::schemaError(const std::string &message) {
    throw common::RuntimeException{"[42000] " + message};
}

// Narrow GQLSTATUS stamping: only where the vendored corpus pins the code AND
// the pin's semantics match the rejection reason (three-tier harness fails a
// wrong code harder than no code). 22G0N = label-set cardinality below the
// minimum (anonymous node type = 0 labels, min 1 on LadybugDB); 22G0P = above
// the maximum (multi-label, max 1). Everything else stays untagged.
[[noreturn]] static void unsupportedWithStatus(const std::string &code,
                                               const std::string &feature) {
    throw common::RuntimeException{"[" + code + "] GQL feature not supported: " + feature};
}

bool GqlToCypherTransformer::engineGraphAtLogicalPath(const std::string &logicalPath) const {
    if (!anyGraphResolver) {
        return false;
    }
    if (anyGraphResolver(manglePhysical(logicalPath)).has_value()) {
        return true;
    }
    // Root-path schemas also collide with flat out-of-layer graph names.
    if (logicalPath.size() > 1 && logicalPath[0] == '/' &&
        logicalPath.find('/', 1) == std::string::npos) {
        return anyGraphResolver(logicalPath.substr(1)).has_value();
    }
    return false;
}

// =============================================================================
// Schema catalog (Phase 11)
// =============================================================================

std::set<std::string> SchemaCatalog::directories() const {
    std::set<std::string> dirs;
    for (const auto &path : schemas) {
        // Every non-empty proper prefix ending at a '/' boundary; the root
        // "/" itself is never a directory.
        size_t pos = path.find('/', 1);
        while (pos != std::string::npos) {
            dirs.insert(path.substr(0, pos));
            pos = path.find('/', pos + 1);
        }
    }
    return dirs;
}

bool SchemaCatalog::isDirectory(const std::string &path) const {
    return directories().count(path) != 0;
}

bool SchemaCatalog::hasMembersUnder(const std::string &path) const {
    const std::string prefix = path + "/";
    for (const auto &[logical, member] : members) {
        (void)member;
        if (logical.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

void SchemaCatalog::addMember(const std::string &logical, const std::string &physical,
                              MemberKind kind) {
    // Drop any stale reverse entry for this logical path first so both maps
    // stay consistent when a logical path is re-registered.
    if (auto it = members.find(logical); it != members.end()) {
        physicalToLogical.erase(it->second.physical);
    }
    members[logical] = Member{physical, kind};
    physicalToLogical[physical] = logical;
}

void SchemaCatalog::removeMemberByLogical(const std::string &logical) {
    if (auto it = members.find(logical); it != members.end()) {
        physicalToLogical.erase(it->second.physical);
        members.erase(it);
    }
}

std::string GqlToCypherTransformer::manglePhysical(const std::string &logicalPath) {
    std::string out = "_gqlsch__";
    bool first = true;
    size_t i = 0;
    while (i < logicalPath.size()) {
        if (logicalPath[i] == '/') {
            ++i;
            continue;
        }
        size_t end = logicalPath.find('/', i);
        if (end == std::string::npos) {
            end = logicalPath.size();
        }
        if (!first) {
            out += "__";
        }
        out += logicalPath.substr(i, end - i);
        first = false;
        i = end;
    }
    return out;
}

void GqlToCypherTransformer::checkReservedPrefix(const std::string &identifier) {
    if (identifier.rfind("_gqlsch__", 0) == 0) {
        unsupported("identifier with reserved _gqlsch__ prefix (" + identifier + ")");
    }
}

std::string GqlToCypherTransformer::serializeSchemaCatalog(const SchemaCatalog &catalog) {
    constexpr char TAB = '\t';
    constexpr char NL = '\n';
    std::string out;
    for (const auto &path : catalog.schemas) {
        out += "Z";
        out += TAB;
        out += path;
        out += NL;
    }
    for (const auto &[logical, member] : catalog.members) {
        out += "M";
        out += TAB;
        out += logical;
        out += TAB;
        out += member.physical;
        out += TAB;
        out += member.kind == SchemaCatalog::MemberKind::GRAPH ? "G" : "T";
        out += NL;
    }
    return out;
}

SchemaCatalog GqlToCypherTransformer::deserializeSchemaCatalog(const std::string &data) {
    SchemaCatalog catalog;
    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2 || (line[0] != 'Z' && line[0] != 'M') || line[1] != '\t') {
            continue;
        }
        if (line[0] == 'Z') {
            catalog.schemas.insert(line.substr(2));
            continue;
        }
        // M \t logical \t physical \t K
        std::vector<std::string> parts;
        std::string cur;
        for (size_t i = 2; i < line.size(); ++i) {
            if (line[i] == '\t') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += line[i];
            }
        }
        parts.push_back(cur);
        if (parts.size() == 3) {
            catalog.addMember(parts[0], parts[1],
                              parts[2] == "T" ? SchemaCatalog::MemberKind::GRAPH_TYPE
                                              : SchemaCatalog::MemberKind::GRAPH);
        }
    }
    return catalog;
}

std::string GqlToCypherTransformer::normalizePathWhitespace(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    char quote = '\0';
    for (char c : raw) {
        if (quote != '\0') {
            out += c;
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            quote = c;
            out += c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        out += c;
    }
    return out;
}

// Strips one layer of delimiting quotes so the reserved-prefix check also
// catches delimited spellings like "`_gqlsch__x`".
static std::string stripDelims(const std::string &s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '`' && s.back() == '`'))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// GQL lexes double-quoted character sequences as delimited identifiers, while
// Cypher spells delimited identifiers with backticks — a raw passthrough emits
// `MATCH (n:"My Label")` style invalid Cypher that dies in the engine's parser
// with a misleading syntax error, exactly the "must never reach the Cypher
// parser" failure mode. Convert every `"..."` identifier (GQL escapes an inner
// quote by doubling) to a backtick-quoted Cypher identifier (Cypher escapes an
// inner backtick by doubling). Applied once at the Transform exit so pattern
// declarations, aliases and expression references (`"weird one".prop`) all
// normalize under one rule. Quote-state aware: single-quoted string literals
// (Cypher strings are single-quoted too) and accent-quoted identifiers (already
// Cypher's spelling) are copied verbatim.
static std::string normalizeDelimitedIdents(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    const size_t n = text.size();
    auto copyThrough = [&](char quote) {
        size_t start = i++;
        while (i < n) {
            if (text[i] == quote) {
                if (i + 1 < n && text[i + 1] == quote) {
                    i += 2;
                    continue;
                }
                ++i;
                break;
            }
            ++i;
        }
        out.append(text, start, i - start);
    };
    while (i < n) {
        char c = text[i];
        if (c == '\'' || c == '`') {
            copyThrough(c);
            continue;
        }
        if (c == '"') {
            ++i;
            std::string body;
            while (i < n) {
                if (text[i] == '"') {
                    if (i + 1 < n && text[i + 1] == '"') {
                        body += '"';
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                body += text[i++];
            }
            out += '`';
            for (char ch : body) {
                if (ch == '`') {
                    out += "``";
                } else {
                    out += ch;
                }
            }
            out += '`';
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

void GqlToCypherTransformer::checkReservedInPath(const std::string &path) {
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            ++i;
            continue;
        }
        size_t end = path.find('/', i);
        if (end == std::string::npos) {
            end = path.size();
        }
        checkReservedPrefix(stripDelims(path.substr(i, end - i)));
        i = end;
    }
}

std::string GqlToCypherTransformer::resolvePhysical(const std::string &logicalPath,
                                                    bool createMapping,
                                                    SchemaCatalog::MemberKind kind) {
    checkReservedInPath(logicalPath);
    if (!schemaCatalog) {
        return manglePhysical(logicalPath);
    }
    if (auto it = schemaCatalog->members.find(logicalPath); it != schemaCatalog->members.end()) {
        return it->second.physical;
    }
    std::string physical = manglePhysical(logicalPath);
    if (createMapping) {
        schemaCatalog->addMember(logicalPath, physical, kind);
    }
    return physical;
}

std::string GqlToCypherTransformer::schemaPathText(
    GQLParser::CatalogSchemaParentAndNameContext *ctx) {
    if (!ctx) {
        unsupported("schema statement without a schema name");
    }
    std::string path = normalizePathWhitespace(sourceText(ctx));
    if (path.empty() || path.front() != '/') {
        unsupported("relative schema path (" + path + ")");
    }
    checkReservedInPath(path);
    return path;
}

std::string GqlToCypherTransformer::qualifiedCatalogPath(
    antlr4::ParserRuleContext *whole, GQLParser::CatalogObjectParentReferenceContext *parent,
    antlr4::ParserRuleContext *finalName) {
    if (!parent || !finalName) {
        unsupported("qualified catalog object without a name");
    }
    // ObjectName segments of the parent reference (`(objectName PERIOD)+`
    // and any dotted tail after a schema reference).
    std::vector<GQLParser::ObjectNameContext *> segs;
    for (auto *child : parent->children) {
        if (auto *obj = dynamic_cast<GQLParser::ObjectNameContext *>(child)) {
            segs.push_back(obj);
        }
    }
    if (segs.empty()) {
        // Root-anchored parent with no dotted tail: keep the spelling
        // byte-for-byte (this is the pre-existing absolute-path branch).
        std::string logical = normalizePathWhitespace(
            whole ? sourceText(whole) : sourceText(parent) + sourceText(finalName));
        if (logical.empty() || logical.front() != '/') {
            // Relative (`../x/y`), predefined (`.` / HOME / CURRENT) and
            // parameter schema references have no root-anchored form to map.
            unsupported("relative schema reference not supported");
        }
        return logical;
    }
    // Dotted form (pure `(objectName PERIOD)+`, or an absolute schema
    // reference with a dotted tail): rebuild the absolute path segment by
    // segment FROM THE PARSE TREE — raw-text splitting would over-split
    // backtick/dquote delimited names that may contain periods themselves.
    std::string path;
    if (auto *schemaRef = parent->schemaReference()) {
        path = normalizePathWhitespace(sourceText(schemaRef));
        if (!path.empty() && path.front() == '$') {
            unsupported("schema reference parameter");
        }
        if (path.empty() || path.front() != '/') {
            unsupported("relative schema reference not supported");
        }
    }
    for (auto *obj : segs) {
        if (path.empty() || path.back() != '/') path += '/';
        path += stripDelims(normalizePathWhitespace(sourceText(obj)));
    }
    if (path.empty() || path.back() != '/') path += '/';
    path += stripDelims(normalizePathWhitespace(sourceText(finalName)));
    return path;
}

std::string GqlToCypherTransformer::rewriteGraphExpression(
    GQLParser::GraphExpressionContext *ctx) {
    std::string raw = sourceText(ctx);
    auto *ref = ctx->graphReference();
    if (!ref || !ref->catalogObjectParentReference()) {
        // Plain name / delimited name / CURRENT_GRAPH / parameter — unchanged.
        // The reserved prefix still applies to user-spelled identifiers.
        checkReservedPrefix(stripDelims(normalizePathWhitespace(raw)));
        return raw;
    }
    // Absolute and dotted qualified names both resolve to one absolute
    // logical path here, so `dir.g` and `/dir/g` mangle to the same
    // physical graph (`_gqlsch__dir__g`).
    std::string logical = qualifiedCatalogPath(
        ctx, ref->catalogObjectParentReference(), ref->graphName());
    checkReservedInPath(logical);
    // Lookups resolve through the registry first; an unknown logical path
    // still mangles deterministically (no member registration — USE/DROP must
    // not invent catalog entries for graphs that may not exist).
    return resolvePhysical(logical, /*createMapping=*/false, SchemaCatalog::MemberKind::GRAPH);
}

std::string GqlToCypherTransformer::snippet(const std::string &text) {
    std::string t;
    for (char c : text) {
        if (t.size() >= 48) {
            t += "...";
            break;
        }
        t += (c == '\n' || c == '\t') ? ' ' : c;
    }
    return t;
}

static bool iequals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// =============================================================================
// Graph-type registry (de)serialization for per-database storage
// =============================================================================

std::string GqlToCypherTransformer::serializeGraphTypes(const GraphTypeRegistry &registry) {
    constexpr char TAB = '\t';
    constexpr char NL = '\n';
    std::string out;
    for (const auto &[key, spec] : registry) {
        out += "T";
        out += TAB;
        out += key;
        out += NL;
        for (const auto &n : spec.nodes) {
            out += "N";
            out += TAB;
            out += n.name;
            for (const auto &p : n.props) {
                out += TAB;
                out += p.name + ":" + p.type;
            }
            out += NL;
        }
        for (const auto &e : spec.edges) {
            out += "E";
            out += TAB;
            out += e.name;
            out += TAB;
            out += e.from;
            out += TAB;
            out += e.to;
            for (const auto &p : e.props) {
                out += TAB;
                out += p.name + ":" + p.type;
            }
            out += NL;
        }
    }
    return out;
}

GraphTypeRegistry GqlToCypherTransformer::deserializeGraphTypes(const std::string &data) {
    GraphTypeRegistry registry;
    GraphTypeSpec *current = nullptr;
    std::istringstream in(data);
    std::string line;
    auto splitTabs = [](const std::string &text) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : text) {
            if (c == '	') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        parts.push_back(cur);
        return parts;
    };
    while (std::getline(in, line)) {
        auto parts = splitTabs(line);
        if (parts.empty() || parts[0].empty()) continue;
        if (parts[0] == "T" && parts.size() >= 2) {
            current = &registry[parts[1]];
            *current = GraphTypeSpec{};
        } else if (parts[0] == "N" && current && parts.size() >= 2) {
            GraphTypeNode node;
            node.name = parts[1];
            for (size_t i = 2; i < parts.size(); i++) {
                auto colon = parts[i].find(':');
                if (colon == std::string::npos) continue;
                node.props.push_back({parts[i].substr(0, colon), parts[i].substr(colon + 1)});
            }
            current->nodes.push_back(std::move(node));
        } else if (parts[0] == "E" && current && parts.size() >= 4) {
            GraphTypeEdge edge;
            edge.name = parts[1];
            edge.from = parts[2];
            edge.to = parts[3];
            for (size_t i = 4; i < parts.size(); i++) {
                auto colon = parts[i].find(':');
                if (colon == std::string::npos) continue;
                edge.props.push_back({parts[i].substr(0, colon), parts[i].substr(colon + 1)});
            }
            current->edges.push_back(std::move(edge));
        }
    }
    return registry;
}

// =============================================================================
// Value-shape guard
// =============================================================================
// GQL record values (`{}`, `{k: v}`) have no LadybugDB expression counterpart,
// and GQL list literals preserve per-element types while LadybugDB homogenizes
// mixed literals to one element type at bind time (STRING is the universal
// sink) — silently erasing types and making max()/min() compare the wrong
// values. Reject both shapes at translation time instead of returning wrong
// answers. Exception: a FOR statement's list source is re-emitted element by
// element through _gql_to_json (see translateForStatement), which absorbs the
// type mixing — scanValueShapes exempts that subtree and the emission site
// re-checks whatever the wrap cannot faithfully encode.
// Pattern property maps (`(n {k: v})`) parse as
// elementPropertySpecification and are intentionally untouched.

namespace {

std::string guardText(const std::string &query, antlr4::ParserRuleContext *ctx) {
    if (!ctx) return "";
    auto *startToken = ctx->getStart();
    auto *stopToken = ctx->getStop();
    if (!startToken || !stopToken) return "";
    size_t startIdx = startToken->getStartIndex();
    size_t stopIdx = stopToken->getStopIndex();
    if (startIdx > query.size() || stopIdx + 1 > query.size() || stopIdx < startIdx) {
        return "";
    }
    return query.substr(startIdx, stopIdx - startIdx + 1);
}

std::string trimCopy(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Type class of a list-element expression when it is a simple literal.
// "null" and "unknown" (non-literals) are ignored by the homogeneity check.
std::string literalTypeClass(const std::string &raw) {
    std::string s = trimCopy(raw);
    if (s.empty()) return "unknown";
    if (iequals(s, "null")) return "null";
    if (iequals(s, "true") || iequals(s, "false")) return "bool";
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') return "string";
    if (s.front() == '[') return "list";
    if (s.front() == '{') return "map";
    static const std::regex intRe(R"([+-]?[0-9]+)");
    static const std::regex numRe(R"([+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?)");
    if (std::regex_match(s, intRe)) return "int";
    if (std::regex_match(s, numRe)) return "double";
    return "unknown";
}

// True when `list`'s untyped elements disagree in type class — exactly the
// condition the heterogeneous-literal rejection enforces (null/unknown/map
// never count). Typed enumerations state their element type and never count.
bool listIsHeterogeneous(GQLParser::ListValueConstructorByEnumerationContext *list,
                         const std::string &query) {
    if (list->listValueTypeName()) return false;
    auto *elList = list->listElementList();
    if (!elList) return false;
    std::string seen;
    for (auto *el : elList->listElement()) {
        std::string cls = literalTypeClass(guardText(query, el->valueExpression()));
        if (cls == "null" || cls == "unknown") continue;
        if (cls == "map") continue; // nested record constructor raises on the way down
        if (seen.empty()) {
            seen = cls;
        } else if (seen != cls) {
            return true;
        }
    }
    return false;
}

// True when `node` sits under a FOR statement's list source (`FOR x IN ...`).