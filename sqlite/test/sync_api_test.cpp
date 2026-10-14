// Incremental CDC end-to-end: writes go to SQLite (vendored C API),
// CALL sqlite_sync(path) replicates them into Ladybug native tables, and
// the mirror converges across updates, deletes, and schema changes. Sync
// takes the file path directly (no ATTACH): synced native tables cannot
// share names with attached shadows.

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "api_test/api_test.h"
#include "sqlite3.h"
#include <format>

using namespace lbug::common;

namespace lbug {
namespace testing {

namespace {

struct SqliteFile {
    std::string dir;
    std::string path;
    sqlite3* db = nullptr;

    SqliteFile() {
        static int counter = 0;
        dir = std::filesystem::temp_directory_path().string() + "/lbug_sync_api_" +
              std::to_string(::getpid()) + "_" + std::to_string(counter++);
        std::filesystem::create_directories(dir);
        path = dir + "/graph.db";
        EXPECT_EQ(SQLITE_OK, sqlite3_open(path.c_str(), &db));
        exec("PRAGMA journal_mode=WAL;");
    }
    ~SqliteFile() {
        if (db) {
            sqlite3_close(db);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void exec(const std::string& sql) {
        char* err = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err)) << sql;
        sqlite3_free(err);
    }
};

std::vector<std::string> queryStrings(main::Connection* conn, const std::string& cypher) {
    auto result = conn->query(cypher);
    EXPECT_TRUE(result->isSuccess()) << cypher << ": " << result->getErrorMessage();
    return TestHelper::convertResultToString(*result);
}

void expectRows(main::Connection* conn, const std::string& cypher,
    std::vector<std::string> expected) {
    EXPECT_EQ(expected, queryStrings(conn, cypher)) << cypher;
}

std::string lit(const std::string& path) {
    return "'" + path + "'";
}

// Polls a MATCH until it returns the expected rows or the deadline passes.
// Daemon ticks are asynchronous; polling is the only sane assertion.
bool pollRows(main::Connection* conn, const std::string& cypher, std::vector<std::string> expected,
    int timeoutMs = 15000) {
    int waited = 0;
    while (waited < timeoutMs) {
        auto result = conn->query(cypher);
        if (result->isSuccess() && TestHelper::convertResultToString(*result) == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waited += 50;
    }
    return false;
}

} // namespace

TEST_F(ApiTest, SqliteSyncGuardRejectsAttachedFile) {
#ifndef __STATIC_LINK_EXTENSION_TEST__
    ASSERT_TRUE(conn->query(std::format("LOAD EXTENSION '{}'",
                                TestHelper::appendLbugRootPath(
                                    "extension/sqlite/build/libsqlite.lbug_extension")))
                    ->isSuccess());
#endif
    SqliteFile sqlite;
    sqlite.exec("CREATE TABLE node_person(id INTEGER PRIMARY KEY, name TEXT);");
    sqlite.exec("INSERT INTO node_person VALUES (1, 'ann');");
    ASSERT_TRUE(conn->query(std::format("ATTACH '{}' AS syncdb (dbtype sqlite)", sqlite.path))
                    ->isSuccess());
    auto result = conn->query(std::format("CALL sqlite_sync({}) RETURN *;", lit(sqlite.path)));
    EXPECT_FALSE(result->isSuccess());
    EXPECT_NE(std::string::npos, result->getErrorMessage().find("DETACH"))
        << result->getErrorMessage();
    ASSERT_TRUE(conn->query("DETACH syncdb;")->isSuccess());
    // After detach the same sync succeeds.
    auto stats = queryStrings(conn.get(),
        std::format("CALL sqlite_sync({}) RETURN tables_synced, rows_upserted;", lit(sqlite.path)));
    ASSERT_EQ(1u, stats.size());
    EXPECT_NE(std::string::npos, stats[0].find("1|1")) << stats[0];
}

TEST_F(ApiTest, SqliteSyncIncremental) {
#ifndef __STATIC_LINK_EXTENSION_TEST__
    ASSERT_TRUE(conn->query(std::format("LOAD EXTENSION '{}'",
                                TestHelper::appendLbugRootPath(
                                    "extension/sqlite/build/libsqlite.lbug_extension")))
                    ->isSuccess());
#endif
    SqliteFile sqlite;
    sqlite.exec("CREATE TABLE node_person(id INTEGER PRIMARY KEY, name TEXT);");
    sqlite.exec("CREATE TABLE node_company(id INTEGER PRIMARY KEY, name TEXT);");
    sqlite.exec("CREATE TABLE rel_works(src_id BIGINT REFERENCES node_person(id), dst_id BIGINT "
                "REFERENCES node_company(id), since BIGINT);");
    sqlite.exec("INSERT INTO node_person VALUES (1, 'ann'), (2, 'bob');");
    sqlite.exec("INSERT INTO node_company VALUES (10, 'acme');");
    sqlite.exec("INSERT INTO rel_works VALUES (1, 10, 2020);");

    // Explicit registration (inference would also map these).
    expectRows(conn.get(),
        std::format("CALL sqlite_register_node({}, 'node_person') RETURN *;", lit(sqlite.path)),
        {"Registered node_person as node node_person (id id)"});
    expectRows(conn.get(),
        std::format("CALL sqlite_register_edge({}, 'rel_works') RETURN *;", lit(sqlite.path)),
        {"Registered rel_works as edge rel_works (src_id -> dst_id)"});

    auto stats = queryStrings(conn.get(),
        std::format("CALL sqlite_sync({}) RETURN tables_synced, rows_upserted, rows_deleted, "
                    "status;",
            lit(sqlite.path)));
    ASSERT_EQ(1u, stats.size());
    EXPECT_NE(std::string::npos, stats[0].find("3|4|0|")) << stats[0];

    expectRows(conn.get(), "MATCH (p:node_person) RETURN p.name ORDER BY p.name;", {"ann", "bob"});
    expectRows(conn.get(),
        "MATCH (p:node_person)-[w:rel_works]->(c:node_company) RETURN p.name, "
        "c.name, w.since;",
        {"ann|acme|2020"});

    // Incremental round: update + delete + insert + new table.
    sqlite.exec("UPDATE node_person SET name = 'ann2' WHERE id = 1;");
    sqlite.exec("DELETE FROM rel_works WHERE src_id = 1;");
    sqlite.exec("INSERT INTO node_person VALUES (3, 'cy');");
    sqlite.exec("INSERT INTO rel_works VALUES (3, 10, 2024);");
    sqlite.exec("CREATE TABLE node_dept(id INTEGER PRIMARY KEY, name TEXT);");
    sqlite.exec("INSERT INTO node_dept VALUES (7, 'eng');");

    auto stats2 = queryStrings(conn.get(),
        std::format("CALL sqlite_sync({}) RETURN tables_synced, rows_upserted, rows_deleted, "
                    "status;",
            lit(sqlite.path)));
    ASSERT_EQ(1u, stats2.size());
    EXPECT_NE(std::string::npos, stats2[0].find("4|")) << stats2[0];

    expectRows(conn.get(), "MATCH (p:node_person) RETURN p.name ORDER BY p.name;",
        {"ann2", "bob", "cy"});
    expectRows(conn.get(),
        "MATCH (p:node_person)-[w:rel_works]->(c:node_company) RETURN p.name, "
        "c.name, w.since;",
        {"cy|acme|2024"});
    expectRows(conn.get(), "MATCH (d:node_dept) RETURN d.name;", {"eng"});

    // Quiet sync: no new commits, nothing applied.
    auto stats3 = queryStrings(conn.get(),
        std::format("CALL sqlite_sync({}) RETURN tables_synced, rows_upserted, rows_deleted, "
                    "status;",
            lit(sqlite.path)));
    ASSERT_EQ(1u, stats3.size());
    EXPECT_NE(std::string::npos, stats3[0].find("|0|0|")) << stats3[0];
}

TEST_F(ApiTest, SqliteSyncDaemonConverges) {
#ifndef __STATIC_LINK_EXTENSION_TEST__
    ASSERT_TRUE(conn->query(std::format("LOAD EXTENSION '{}'",
                                TestHelper::appendLbugRootPath(
                                    "extension/sqlite/build/libsqlite.lbug_extension")))
                    ->isSuccess());
#endif
    SqliteFile sqlite;
    sqlite.exec("CREATE TABLE node_person(id INTEGER PRIMARY KEY, name TEXT);");
    sqlite.exec("INSERT INTO node_person VALUES (1, 'ann');");

    expectRows(conn.get(),
        std::format("CALL sqlite_sync_start({}, 50) RETURN *;", lit(sqlite.path)),
        {std::format("Sync daemon started for '{}' every 50ms", sqlite.path)});
    // Duplicate start is rejected.
    {
        auto dup =
            conn->query(std::format("CALL sqlite_sync_start({}, 50) RETURN *;", lit(sqlite.path)));
        EXPECT_FALSE(dup->isSuccess());
    }
    EXPECT_TRUE(
        pollRows(conn.get(), "MATCH (p:node_person) RETURN p.name ORDER BY p.name;", {"ann"}));

    sqlite.exec("UPDATE node_person SET name = 'ann2' WHERE id = 1;");
    sqlite.exec("INSERT INTO node_person VALUES (2, 'bob');");
    EXPECT_TRUE(pollRows(conn.get(), "MATCH (p:node_person) RETURN p.name ORDER BY p.name;",
        {"ann2", "bob"}));

    sqlite.exec("DELETE FROM node_person WHERE id = 1;");
    EXPECT_TRUE(
        pollRows(conn.get(), "MATCH (p:node_person) RETURN p.name ORDER BY p.name;", {"bob"}));

    // Status reports the running daemon.
    {
        auto status = queryStrings(conn.get(),
            "CALL sqlite_sync_status() RETURN running, sync ORDER BY sync;");
        ASSERT_EQ(1u, status.size());
        EXPECT_NE(std::string::npos, status[0].find("1|")) << status[0];
        EXPECT_NE(std::string::npos, status[0].find(sqlite.path)) << status[0];
    }
    expectRows(conn.get(), std::format("CALL sqlite_sync_stop({}) RETURN *;", lit(sqlite.path)),
        {std::format("Sync daemon stopped for '{}'", sqlite.path)});
    // Stopping twice is an error; status is empty afterwards.
    {
        auto dup =
            conn->query(std::format("CALL sqlite_sync_stop({}) RETURN *;", lit(sqlite.path)));
        EXPECT_FALSE(dup->isSuccess());
    }
    expectRows(conn.get(), "CALL sqlite_sync_status() RETURN *;", {});
}

} // namespace testing
} // namespace lbug
