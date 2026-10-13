// Phase-3a CDC tests: convention-based sync mapping (register_node /
// register_edge semantics) and the Cypher statement builder. Pure string
// logic, no engine dependency.

#include <string>
#include <vector>

#include "cdc/op_resolver.h"
#include "cdc/sync_cypher.h"
#include "cdc/sync_mapping.h"
#include "gtest/gtest.h"

using namespace lbug::sqlite_extension::cdc;

namespace {

SyncTable makeNodePerson() {
    SyncTable t;
    t.name = "node_person";
    t.columns = {{"id", "INTEGER", true}, {"name", "TEXT", false}, {"age", "BIGINT", false}};
    t.ipkIndex = 0;
    return t;
}

SyncTable makeRelKnows() {
    SyncTable t;
    t.name = "rel_knows";
    t.columns = {{"src_id", "BIGINT", false}, {"dst_id", "BIGINT", false},
        {"since", "BIGINT", false}};
    t.foreignKeys = {{"src_id", "node_person"}, {"dst_id", "node_person"}};
    return t;
}

const TableMapping* findMapping(const MappingResult& result, const std::string& table) {
    for (auto& m : result.mappings) {
        if (m.sqliteTable == table) {
            return &m;
        }
    }
    return nullptr;
}

TEST(CdcSyncMapping, InfersNodeWithIntegerPk) {
    auto result = inferMappings({makeNodePerson()});
    ASSERT_EQ(1u, result.mappings.size());
    auto& m = result.mappings[0];
    EXPECT_EQ(MappingKind::Node, m.kind);
    EXPECT_EQ("node_person", m.label);
    EXPECT_TRUE(m.idFromRowid);
    EXPECT_EQ("id", m.idColumn);
    EXPECT_TRUE(result.skipped.empty());
}

TEST(CdcSyncMapping, InfersNodeWithExplicitPk) {
    SyncTable t;
    t.name = "node_doc";
    t.columns = {{"code", "TEXT", true}, {"v", "INT", false}};
    t.ipkIndex = -1;
    auto result = inferMappings({t});
    ASSERT_EQ(1u, result.mappings.size());
    EXPECT_FALSE(result.mappings[0].idFromRowid);
    EXPECT_EQ(0u, result.mappings[0].idRecordIndex);
    EXPECT_EQ("code", result.mappings[0].idColumn);
}

TEST(CdcSyncMapping, InfersEdgeWithEndpoints) {
    auto result = inferMappings({makeNodePerson(), makeRelKnows()});
    auto* edge = findMapping(result, "rel_knows");
    ASSERT_NE(nullptr, edge);
    EXPECT_EQ(MappingKind::Edge, edge->kind);
    EXPECT_FALSE(edge->csrContract);
    EXPECT_EQ("node_person", edge->srcLabel);
    EXPECT_EQ("node_person", edge->dstLabel);
    EXPECT_EQ(0u, edge->srcColIndex);
    EXPECT_EQ(1u, edge->dstColIndex);
    EXPECT_EQ("id", edge->srcPkColumn);
    EXPECT_TRUE(edge->srcPkFromRowid);
}

TEST(CdcSyncMapping, CsrRelSetsContractFlag) {
    auto node = makeNodePerson();
    SyncTable rel = makeRelKnows();
    rel.name = "csr_rel_knows";
    auto result = inferMappings({node, rel});
    auto* edge = findMapping(result, "csr_rel_knows");
    ASSERT_NE(nullptr, edge);
    EXPECT_TRUE(edge->csrContract);
}

TEST(CdcSyncMapping, SkipsUnmappableTables) {
    SyncTable plain;
    plain.name = "person";
    plain.columns = {{"id", "INTEGER", true}};
    plain.ipkIndex = 0;
    SyncTable nopk;
    nopk.name = "node_nopk";
    nopk.columns = {{"a", "TEXT", false}};
    SyncTable nofk;
    nofk.name = "rel_nofk";
    nofk.columns = {{"a", "INT", false}, {"b", "INT", false}};
    SyncTable dangling;
    dangling.name = "rel_dangling";
    dangling.columns = {{"src_id", "INT", false}, {"dst_id", "INT", false}};
    dangling.foreignKeys = {{"src_id", "node_person"}, {"dst_id", "node_missing"}};
    SyncTable wr;
    wr.name = "node_wr";
    wr.columns = {{"id", "INT", true}};
    wr.withoutRowid = true;
    auto result = inferMappings({makeNodePerson(), plain, nopk, nofk, dangling, wr});
    // Only node_person maps; everything else skips with a reason.
    ASSERT_EQ(1u, result.mappings.size());
    EXPECT_EQ(5u, result.skipped.size());
    for (auto& s : result.skipped) {
        EXPECT_FALSE(s.reason.empty()) << s.name;
    }
}

TEST(CdcSyncCypher, DecltypeMapping) {
    EXPECT_EQ("INT64", cypherTypeForDecl("BIGINT"));
    EXPECT_EQ("INT64", cypherTypeForDecl("INTEGER"));
    EXPECT_EQ("STRING", cypherTypeForDecl("VARCHAR(10)"));
    EXPECT_EQ("STRING", cypherTypeForDecl("TEXT"));
    EXPECT_EQ("DOUBLE", cypherTypeForDecl("DOUBLE"));
    EXPECT_EQ("DOUBLE", cypherTypeForDecl("REAL"));
    EXPECT_EQ("INT64", cypherTypeForDecl("BOOLEAN"));
    EXPECT_EQ("DATE", cypherTypeForDecl("DATE"));
    EXPECT_EQ("TIMESTAMP", cypherTypeForDecl("DATETIME"));
    EXPECT_EQ("BLOB", cypherTypeForDecl("BLOB"));
    EXPECT_EQ("BLOB", cypherTypeForDecl(""));
    EXPECT_EQ("STRING", cypherTypeForDecl("NUMERIC"));
}

TEST(CdcSyncCypher, ValueRendering) {
    SqliteValue v;
    v.type = SqliteValue::Type::Integer;
    v.intVal = -42;
    EXPECT_EQ("-42", renderCypherValue(v, "INT64").literal);
    v.type = SqliteValue::Type::Real;
    v.realVal = 1.5;
    EXPECT_EQ("1.5", renderCypherValue(v, "DOUBLE").literal);
    v.type = SqliteValue::Type::Integer;
    v.intVal = 5;
    EXPECT_EQ("5.0", renderCypherValue(v, "DOUBLE").literal);
    v.type = SqliteValue::Type::Text;
    v.textVal = "O'Brien\\x";
    auto text = renderCypherValue(v, "STRING");
    ASSERT_TRUE(text.ok);
    EXPECT_EQ("'O''Brien\\\\x'", text.literal);
    v.textVal = "1993-05-21";
    EXPECT_EQ("date('1993-05-21')", renderCypherValue(v, "DATE").literal);
    v.textVal = "1993-05-21 08:30:00";
    EXPECT_EQ("timestamp('1993-05-21 08:30:00')", renderCypherValue(v, "TIMESTAMP").literal);
    v.type = SqliteValue::Type::Blob;
    v.blobVal = {0x41, 0x00, 0x27, 0x5c};
    EXPECT_EQ("BLOB('A\\x00''\\\\')", renderCypherValue(v, "BLOB").literal);
    v.type = SqliteValue::Type::Null;
    EXPECT_EQ("null", renderCypherValue(v, "INT64").literal);
    // Strictness: storage kind must match the declared type. Unparseable
    // timestamp/date *strings* pass through and fail at execution with
    // engine context; only structural mismatches are builder errors.
    v.type = SqliteValue::Type::Text;
    v.textVal = "abc";
    EXPECT_FALSE(renderCypherValue(v, "INT64").ok);
    EXPECT_EQ("timestamp('abc')", renderCypherValue(v, "TIMESTAMP").literal);
    v.type = SqliteValue::Type::Real;
    EXPECT_FALSE(renderCypherValue(v, "DATE").ok);
}

TEST(CdcSyncCypher, NodeDdl) {
    auto mappings = inferMappings({makeNodePerson()});
    auto target = bindTarget(mappings.mappings[0], makeNodePerson().columns);
    EXPECT_EQ("CREATE NODE TABLE IF NOT EXISTS `node_person`(`id` INT64, `name` STRING, `age` "
              "INT64, PRIMARY KEY(`id`))",
        buildNodeDdl(target));
}

TEST(CdcSyncCypher, RelDdl) {
    auto mappings = inferMappings({makeNodePerson(), makeRelKnows()});
    const TableMapping* edge = findMapping(mappings, "rel_knows");
    ASSERT_NE(nullptr, edge);
    auto target = bindTarget(*edge, makeRelKnows().columns);
    EXPECT_EQ("CREATE REL TABLE IF NOT EXISTS `rel_knows`(FROM `node_person` TO `node_person`, "
              "`since` INT64)",
        buildRelDdl(target));
}

TEST(CdcSyncCypher, NodeUpsertsAndDeletes) {
    auto mappings = inferMappings({makeNodePerson()});
    auto target = bindTarget(mappings.mappings[0], makeNodePerson().columns);
    RowOp r1, r2;
    r1.table = "node_person";
    r1.rowid = 1;
    r1.values = {SqliteValue{}, SqliteValue{}, SqliteValue{}};
    r1.values[0].type = SqliteValue::Type::Null; // IPK placeholder
    r1.values[1].type = SqliteValue::Type::Text;
    r1.values[1].textVal = "O'Brien";
    r1.values[2].type = SqliteValue::Type::Integer;
    r1.values[2].intVal = 30;
    r2.table = "node_person";
    r2.rowid = 2;
    r2.values = {SqliteValue{}, SqliteValue{}, SqliteValue{}};
    r2.values[1].type = SqliteValue::Type::Text;
    r2.values[1].textVal = "al";
    r2.values[2].type = SqliteValue::Type::Null;
    auto stmts = buildNodeUpserts(target, {r1, r2}, 10);
    ASSERT_EQ(1u, stmts.size());
    EXPECT_EQ("UNWIND [{`id`: 1, `name`: 'O''Brien', `age`: 30}, {`id`: 2, `name`: 'al', `age`: "
              "null}] AS row MERGE (n:`node_person` {`id`: row.`id`}) SET n.`name` = row.`name`, "
              "n.`age` = row.`age`",
        stmts[0]);
    // Chunking.
    auto chunked = buildNodeUpserts(target, {r1, r2}, 1);
    ASSERT_EQ(2u, chunked.size());
    // Deletes.
    RowDelete d1, d2;
    d1.table = "node_person";
    d1.rowid = 1;
    d2.table = "node_person";
    d2.rowid = 2;
    auto dels = buildNodeDeletes(target, {d1, d2}, 10);
    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ("UNWIND [1, 2] AS row MATCH (n:`node_person` {`id`: row}) DETACH DELETE n", dels[0]);
}

TEST(CdcSyncCypher, ExplicitPkDeleteRendersValue) {
    SyncTable t;
    t.name = "node_doc";
    t.columns = {{"code", "TEXT", true}, {"v", "INT", false}};
    auto mappings = inferMappings({t});
    ASSERT_EQ(1u, mappings.mappings.size());
    auto target = bindTarget(mappings.mappings[0], t.columns);
    RowDelete d;
    d.table = "node_doc";
    d.rowid = 9;
    d.hasIdentity = true;
    d.identity.type = SqliteValue::Type::Text;
    d.identity.textVal = "K-1";
    auto dels = buildNodeDeletes(target, {d}, 10);
    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ("UNWIND ['K-1'] AS row MATCH (n:`node_doc` {`code`: row}) DETACH DELETE n", dels[0]);
}

TEST(CdcSyncCypher, EdgeUpsertsAndDeletes) {
    auto mappings = inferMappings({makeNodePerson(), makeRelKnows()});
    const TableMapping* edge = findMapping(mappings, "rel_knows");
    ASSERT_NE(nullptr, edge);
    auto target = bindTarget(*edge, makeRelKnows().columns);
    RowOp r;
    r.table = "rel_knows";
    r.rowid = 1;
    r.values = {SqliteValue{}, SqliteValue{}, SqliteValue{}};
    r.values[0].type = SqliteValue::Type::Integer;
    r.values[0].intVal = 1;
    r.values[1].type = SqliteValue::Type::Integer;
    r.values[1].intVal = 2;
    r.values[2].type = SqliteValue::Type::Integer;
    r.values[2].intVal = 5;
    auto stmts = buildEdgeUpserts(target, {r}, 10);
    ASSERT_EQ(1u, stmts.size());
    EXPECT_EQ("UNWIND [{`src`: 1, `dst`: 2, `since`: 5}] AS row MATCH (a:`node_person` {`id`: "
              "row.`src`}), (b:`node_person` {`id`: row.`dst`}) MERGE (a)-[e:`rel_knows`]->(b) "
              "SET e.`since` = row.`since`",
        stmts[0]);
    auto dels = buildEdgeDeletes(target, {r}, 10);
    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ("UNWIND [{`src`: 1, `dst`: 2}] AS row MATCH (a:`node_person` {`id`: row.`src`})-"
              "[e:`rel_knows`]->(b:`node_person` {`id`: row.`dst`}) DELETE e",
        dels[0]);
}

TEST(CdcSyncCypher, CursorStatements) {
    EXPECT_EQ("CREATE NODE TABLE IF NOT EXISTS `__sqlite_cdc_cursor`(`sync` STRING, `salt1` INT64,"
              " `salt2` INT64, `applied` INT64, PRIMARY KEY(`sync`))",
        buildCursorDdl());
    EXPECT_EQ("MERGE (c:`__sqlite_cdc_cursor` {`sync`: 'main'}) SET c.`salt1` = 1, c.`salt2` = 2, "
              "c.`applied` = 42",
        buildCursorUpsert("main", 1, 2, 42));
}

} // namespace
