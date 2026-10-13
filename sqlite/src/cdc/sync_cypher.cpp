#include "cdc/sync_cypher.h"

#include <cctype>
#include <charconv>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

namespace {

std::string toUpper(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

bool containsWord(const std::string& upper, const char* word) {
    return upper.find(word) != std::string::npos;
}

// Shortest round-trip double rendering (C++17 shortest-representation).
std::string renderDouble(double d) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    return std::string(buf, res.ptr);
}

// Cypher single-quoted string escaping.
std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '\'':
            out += "''";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

// Blob bytes as a BLOB('...') literal: printable ASCII raw (except ' and
// \), everything else \xHH, mirroring the engine's own literal style.
std::string escapeBlob(const uint8_t* data, size_t size) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(size + 2);
    for (size_t i = 0; i < size; i++) {
        uint8_t b = data[i];
        if (b == '\'') {
            out += "''";
        } else if (b == '\\') {
            out += "\\\\";
        } else if (b >= 0x20 && b <= 0x7e) {
            out.push_back(static_cast<char>(b));
        } else {
            out.push_back('\\');
            out.push_back('x');
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0xf]);
        }
    }
    return out;
}

RenderResult renderInt(int64_t v) {
    RenderResult out;
    out.ok = true;
    out.literal = std::to_string(v);
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

// Renders one op's identity literal (node PK or edge endpoint value).
bool renderIdentity(const BoundTarget& target, const RowOp& op, std::string& literal,
    std::string& error) {
    if (target.mapping->idFromRowid) {
        literal = std::to_string(op.rowid);
        return true;
    }
    if (target.mapping->idRecordIndex >= op.values.size()) {
        error = "Record shorter than id column index";
        return false;
    }
    auto rendered = renderCypherValue(op.values[target.mapping->idRecordIndex], target.idType);
    if (!rendered.ok) {
        error = rendered.error;
        return false;
    }
    literal = rendered.literal;
    return true;
}

bool renderEndpoint(const std::vector<SqliteValue>& values, size_t recordIndex,
    const std::string& idType, std::string& literal, std::string& error) {
    if (recordIndex >= values.size()) {
        error = "Record shorter than endpoint column index";
        return false;
    }
    auto rendered = renderCypherValue(values[recordIndex], idType);
    if (!rendered.ok) {
        error = rendered.error;
        return false;
    }
    literal = rendered.literal;
    return true;
}

} // namespace

std::string cypherTypeForDecl(const std::string& decltype_) {
    auto upper = toUpper(decltype_);
    // Trimmed comparison is unnecessary: affinity keywords match by
    // containment, exactly like the scan's type converter.
    if (upper.empty() || upper == "NULL" || containsWord(upper, "BLOB")) {
        return "BLOB";
    }
    if (upper == "DATE" || upper.rfind("DATE(", 0) == 0) {
        return "DATE";
    }
    if (containsWord(upper, "DATETIME") || containsWord(upper, "TIMESTAMP")) {
        return "TIMESTAMP";
    }
    if (containsWord(upper, "INT")) {
        return "INT64";
    }
    if (upper == "BOOLEAN" || upper == "BOOL") {
        // Matches the scan, which reports SQLite BOOLEAN as INT64.
        return "INT64";
    }
    if (containsWord(upper, "CHAR") || containsWord(upper, "CLOB") || containsWord(upper, "TEXT")) {
        return "STRING";
    }
    if (containsWord(upper, "REAL") || containsWord(upper, "FLOA") || containsWord(upper, "DOUB")) {
        return "DOUBLE";
    }
    // NUMERIC/DECIMAL and anything unknown read as text through the scan;
    // keep DDL consistent withble that. NUMERIC affinity columns land here.
    return "STRING";
}

std::string quoteCypherIdent(const std::string& name) {
    std::string out = "`";
    for (char c : name) {
        if (c == '`') {
            out += "``";
        } else {
            out.push_back(c);
        }
    }
    out += "`";
    return out;
}

RenderResult renderCypherValue(const SqliteValue& value, const std::string& cypherType) {
    RenderResult out;
    if (value.type == SqliteValue::Type::Null) {
        out.ok = true;
        out.literal = "null";
        return out;
    }
    if (cypherType == "INT64" || cypherType == "INT32" || cypherType == "INT16" ||
        cypherType == "INT8") {
        if (value.type != SqliteValue::Type::Integer) {
            out.error = "Non-integer storage in integer column";
            return out;
        }
        return renderInt(value.intVal);
    }
    if (cypherType == "DOUBLE" || cypherType == "FLOAT") {
        if (value.type == SqliteValue::Type::Integer) {
            out.ok = true;
            out.literal = std::to_string(value.intVal) + ".0";
            return out;
        }
        if (value.type != SqliteValue::Type::Real) {
            out.error = "Non-numeric storage in float column";
            return out;
        }
        out.ok = true;
        out.literal = renderDouble(value.realVal);
        return out;
    }
    if (cypherType == "BOOLEAN") {
        if (value.type != SqliteValue::Type::Integer || (value.intVal != 0 && value.intVal != 1)) {
            out.error = "Non-boolean storage in boolean column";
            return out;
        }
        out.ok = true;
        out.literal = value.intVal ? "true" : "false";
        return out;
    }
    if (cypherType == "DATE") {
        if (value.type != SqliteValue::Type::Text) {
            out.error = "Non-text storage in DATE column";
            return out;
        }
        out.ok = true;
        out.literal = "date('" + escapeString(value.textVal) + "')";
        return out;
    }
    if (cypherType == "TIMESTAMP") {
        if (value.type != SqliteValue::Type::Text) {
            out.error = "Non-text storage in TIMESTAMP column";
            return out;
        }
        out.ok = true;
        out.literal = "timestamp('" + escapeString(value.textVal) + "')";
        return out;
    }
    if (cypherType == "BLOB") {
        out.ok = true;
        if (value.type == SqliteValue::Type::Blob) {
            out.literal = "BLOB('" + escapeBlob(value.blobVal.data(), value.blobVal.size()) + "')";
        } else if (value.type == SqliteValue::Type::Text) {
            out.literal = "BLOB('" +
                          escapeBlob(reinterpret_cast<const uint8_t*>(value.textVal.data()),
                              value.textVal.size()) +
                          "')";
        } else {
            out.error = "Non-blob storage in BLOB column";
            out.ok = false;
        }
        return out;
    }
    // STRING and everything else: stringify any storage, like the scan.
    out.ok = true;
    if (value.type == SqliteValue::Type::Integer) {
        out.literal = "'" + std::to_string(value.intVal) + "'";
    } else if (value.type == SqliteValue::Type::Real) {
        out.literal = "'" + renderDouble(value.realVal) + "'";
    } else if (value.type == SqliteValue::Type::Blob) {
        out.literal = "'" + escapeBlob(value.blobVal.data(), value.blobVal.size()) + "'";
    } else {
        out.literal = "'" + escapeString(value.textVal) + "'";
    }
    return out;
}

BoundTarget bindTarget(const TableMapping& mapping, const std::vector<SyncColumn>& recordColumns) {
    BoundTarget target;
    target.mapping = &mapping;
    for (size_t i = 0; i < recordColumns.size(); i++) {
        BoundTarget::Prop prop;
        prop.name = recordColumns[i].name;
        prop.type = cypherTypeForDecl(recordColumns[i].decltype_);
        prop.recordIndex = i;
        target.columns.push_back(prop);
    }
    if (mapping.idFromRowid) {
        target.idType = "INT64";
    } else if (mapping.idRecordIndex < target.columns.size()) {
        target.idType = target.columns[mapping.idRecordIndex].type;
    }
    target.srcIdType = cypherTypeForDecl(mapping.srcPkDecltype);
    target.dstIdType = cypherTypeForDecl(mapping.dstPkDecltype);
    for (auto& prop : target.columns) {
        if (mapping.kind == MappingKind::Node && prop.recordIndex == mapping.idRecordIndex &&
            !mapping.idFromRowid) {
            continue; // Identity lives in the MERGE key, not SET.
        }
        if (mapping.kind == MappingKind::Node && mapping.idFromRowid &&
            prop.name == mapping.idColumn) {
            continue;
        }
        if (mapping.kind == MappingKind::Edge &&
            (prop.recordIndex == mapping.srcColIndex || prop.recordIndex == mapping.dstColIndex)) {
            continue;
        }
        target.props.push_back(prop);
    }
    return target;
}

std::string buildNodeDdl(const BoundTarget& target) {
    std::vector<std::string> defs;
    for (auto& col : target.columns) {
        defs.push_back(quoteCypherIdent(col.name) + " " + col.type);
    }
    defs.push_back("PRIMARY KEY(" + quoteCypherIdent(target.mapping->idColumn) + ")");
    return "CREATE NODE TABLE IF NOT EXISTS " + quoteCypherIdent(target.mapping->label) + "(" +
           join(defs, ", ") + ")";
}

std::string buildRelDdl(const BoundTarget& target) {
    std::string ddl = "CREATE REL TABLE IF NOT EXISTS " + quoteCypherIdent(target.mapping->label) +
                      "(FROM " + quoteCypherIdent(target.mapping->srcLabel) + " TO " +
                      quoteCypherIdent(target.mapping->dstLabel);
    for (auto& prop : target.props) {
        ddl += ", " + quoteCypherIdent(prop.name) + " " + prop.type;
    }
    return ddl + ")";
}

std::string buildCursorDdl() {
    return "CREATE NODE TABLE IF NOT EXISTS `__sqlite_cdc_cursor`(`sync` STRING, `salt1` INT64, "
           "`salt2` INT64, `applied` INT64, PRIMARY KEY(`sync`))";
}

namespace {} // namespace

std::vector<std::string> buildNodeUpserts(const BoundTarget& target, const std::vector<RowOp>& ops,
    size_t batchSize) {
    std::vector<std::string> statements;
    if (batchSize == 0) {
        batchSize = 1;
    }
    std::string label = quoteCypherIdent(target.mapping->label);
    std::string idKey = quoteCypherIdent(target.mapping->idColumn);
    for (size_t begin = 0; begin < ops.size(); begin += batchSize) {
        size_t end = std::min(ops.size(), begin + batchSize);
        std::vector<std::string> items;
        for (size_t i = begin; i < end; i++) {
            std::string idLit;
            std::string error;
            if (!renderIdentity(target, ops[i], idLit, error)) {
                continue; // Skip unrenderable rows; caller surfaces count.
            }
            std::vector<std::string> rowItems{idKey + ": " + idLit};
            for (auto& prop : target.props) {
                if (prop.recordIndex >= ops[i].values.size()) {
                    continue;
                }
                auto rendered = renderCypherValue(ops[i].values[prop.recordIndex], prop.type);
                if (!rendered.ok) {
                    continue;
                }
                rowItems.push_back(quoteCypherIdent(prop.name) + ": " + rendered.literal);
            }
            items.push_back("{" + join(rowItems, ", ") + "}");
        }
        if (items.empty()) {
            continue;
        }
        std::vector<std::string> sets;
        for (auto& prop : target.props) {
            sets.push_back(
                "n." + quoteCypherIdent(prop.name) + " = row." + quoteCypherIdent(prop.name));
        }
        std::string stmt = "UNWIND [" + join(items, ", ") + "] AS row MERGE (n:" + label + " {" +
                           idKey + ": row." + idKey + "})";
        if (!sets.empty()) {
            stmt += " SET " + join(sets, ", ");
        }
        statements.push_back(std::move(stmt));
    }
    return statements;
}

std::vector<std::string> buildNodeDeletes(const BoundTarget& target,
    const std::vector<RowDelete>& ops, size_t batchSize) {
    std::vector<std::string> statements;
    if (batchSize == 0) {
        batchSize = 1;
    }
    std::string label = quoteCypherIdent(target.mapping->label);
    std::string idKey = quoteCypherIdent(target.mapping->idColumn);
    for (size_t begin = 0; begin < ops.size(); begin += batchSize) {
        size_t end = std::min(ops.size(), begin + batchSize);
        std::vector<std::string> items;
        for (size_t i = begin; i < end; i++) {
            // Rowid-sourced identities render from the rowid; explicit-PK
            // deletes carry the retained PK value from the resolver.
            std::string idLit = std::to_string(ops[i].rowid);
            if (!target.mapping->idFromRowid && ops[i].hasIdentity) {
                auto rendered = renderCypherValue(ops[i].identity, target.idType);
                if (!rendered.ok) {
                    continue;
                }
                idLit = rendered.literal;
            } else if (!target.mapping->idFromRowid) {
                continue; // No identity retained; cannot address the row.
            }
            items.push_back(idLit);
        }
        if (items.empty()) {
            continue;
        }
        statements.push_back("UNWIND [" + join(items, ", ") + "] AS row MATCH (n:" + label + " {" +
                             idKey + ": row}) DETACH DELETE n");
    }
    return statements;
}

std::vector<std::string> buildEdgeUpserts(const BoundTarget& target, const std::vector<RowOp>& ops,
    size_t batchSize) {
    std::vector<std::string> statements;
    if (batchSize == 0) {
        batchSize = 1;
    }
    std::string label = quoteCypherIdent(target.mapping->label);
    std::string srcLabel = quoteCypherIdent(target.mapping->srcLabel);
    std::string dstLabel = quoteCypherIdent(target.mapping->dstLabel);
    std::string srcKey = quoteCypherIdent(target.mapping->srcPkColumn);
    std::string dstKey = quoteCypherIdent(target.mapping->dstPkColumn);
    for (size_t begin = 0; begin < ops.size(); begin += batchSize) {
        size_t end = std::min(ops.size(), begin + batchSize);
        std::vector<std::string> items;
        for (size_t i = begin; i < end; i++) {
            std::string srcLit, dstLit, error;
            if (!renderEndpoint(ops[i].values, target.mapping->srcColIndex, target.srcIdType,
                    srcLit, error) ||
                !renderEndpoint(ops[i].values, target.mapping->dstColIndex, target.dstIdType,
                    dstLit, error)) {
                continue;
            }
            std::vector<std::string> rowItems{"`src`: " + srcLit, "`dst`: " + dstLit};
            for (auto& prop : target.props) {
                if (prop.recordIndex >= ops[i].values.size()) {
                    continue;
                }
                auto rendered = renderCypherValue(ops[i].values[prop.recordIndex], prop.type);
                if (!rendered.ok) {
                    continue;
                }
                rowItems.push_back(quoteCypherIdent(prop.name) + ": " + rendered.literal);
            }
            items.push_back("{" + join(rowItems, ", ") + "}");
        }
        if (items.empty()) {
            continue;
        }
        std::vector<std::string> sets;
        for (auto& prop : target.props) {
            sets.push_back(
                "e." + quoteCypherIdent(prop.name) + " = row." + quoteCypherIdent(prop.name));
        }
        std::string stmt = "UNWIND [" + join(items, ", ") + "] AS row MATCH (a:" + srcLabel + " {" +
                           srcKey + ": row.`src`}), (b:" + dstLabel + " {" + dstKey +
                           ": row.`dst`}) MERGE (a)-[e:" + label + "]->(b)";
        if (!sets.empty()) {
            stmt += " SET " + join(sets, ", ");
        }
        statements.push_back(std::move(stmt));
    }
    return statements;
}

std::vector<std::string> buildEdgeDeletes(const BoundTarget& target, const std::vector<RowOp>& ops,
    size_t batchSize) {
    // Edge deletes need endpoint values, which live in records: callers
    // pass the deleted rows' last images as RowOps.
    std::vector<std::string> statements;
    if (batchSize == 0) {
        batchSize = 1;
    }
    std::string label = quoteCypherIdent(target.mapping->label);
    std::string srcLabel = quoteCypherIdent(target.mapping->srcLabel);
    std::string dstLabel = quoteCypherIdent(target.mapping->dstLabel);
    std::string srcKey = quoteCypherIdent(target.mapping->srcPkColumn);
    std::string dstKey = quoteCypherIdent(target.mapping->dstPkColumn);
    for (size_t begin = 0; begin < ops.size(); begin += batchSize) {
        size_t end = std::min(ops.size(), begin + batchSize);
        std::vector<std::string> items;
        for (size_t i = begin; i < end; i++) {
            std::string srcLit, dstLit, error;
            if (!renderEndpoint(ops[i].values, target.mapping->srcColIndex, target.srcIdType,
                    srcLit, error) ||
                !renderEndpoint(ops[i].values, target.mapping->dstColIndex, target.dstIdType,
                    dstLit, error)) {
                continue;
            }
            items.push_back("{`src`: " + srcLit + ", `dst`: " + dstLit + "}");
        }
        if (items.empty()) {
            continue;
        }
        statements.push_back("UNWIND [" + join(items, ", ") + "] AS row MATCH (a:" + srcLabel +
                             " {" + srcKey + ": row.`src`})-[e:" + label + "]->(b:" + dstLabel +
                             " {" + dstKey + ": row.`dst`}) DELETE e");
    }
    return statements;
}

std::string buildCursorUpsert(const std::string& syncName, uint32_t salt1, uint32_t salt2,
    size_t appliedFrames) {
    return "MERGE (c:`__sqlite_cdc_cursor` {`sync`: '" + escapeString(syncName) + "'}) SET " +
           "c.`salt1` = " + std::to_string(salt1) + ", c.`salt2` = " + std::to_string(salt2) +
           ", c.`applied` = " + std::to_string(appliedFrames);
}

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
