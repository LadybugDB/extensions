// Phase-1 CDC tests: WAL tailer + b-tree page decoder, cross-checked
// against ground truth from the vendored SQLite itself. The fixture builds
// real databases in WAL mode, so every assertion runs against genuine WAL
// bytes (magic, salts, checksums, commit framing included).

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cdc/btree_page_decoder.h"
#include "cdc/wal_tailer.h"
#include "gtest/gtest.h"
#include "sqlite3.h"

using namespace lbug::sqlite_extension::cdc;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

struct Fixture {
    std::string dir;
    std::string dbPath;
    sqlite3* db = nullptr;

    Fixture() {
        // Unique temp dir without extra deps: pid + counter.
        static int counter = 0;
        dir = std::filesystem::temp_directory_path().string() + "/lbug_cdc_test_" +
              std::to_string(::getpid()) + "_" + std::to_string(counter++);
        std::filesystem::create_directories(dir);
        dbPath = dir + "/test.db";
        EXPECT_EQ(SQLITE_OK, sqlite3_open(dbPath.c_str(), &db));
        exec("PRAGMA journal_mode=WAL;");
    }
    ~Fixture() {
        if (db) {
            sqlite3_close(db);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void exec(const std::string& sql) {
        char* err = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err))
            << (err ? err : sql);
        sqlite3_free(err);
    }
    std::vector<uint8_t> walBytes() { return readFile(dbPath + "-wal"); }
    std::vector<uint8_t> dbBytes() { return readFile(dbPath); }
};

WalParseResult parseCurrentWal(Fixture& f) {
    auto bytes = f.walBytes();
    return parseWal(bytes.data(), bytes.size());
}

// WAL-first, DB-second page fetcher over the fixture's current files.
struct FixturePages {
    std::vector<uint8_t> wal;
    std::vector<uint8_t> db;
    uint32_t pageSize = 4096;
    const uint8_t* fetch(uint32_t pageNo) const {
        // WAL frames newest-first so later images shadow earlier ones.
        size_t frameSize = WAL_FRAME_HDR_SIZE + pageSize;
        const uint8_t* found = nullptr;
        for (size_t off = WAL_HDR_SIZE; off + frameSize <= wal.size(); off += frameSize) {
            uint32_t pg = readU32BE(wal.data() + off);
            if (pg == pageNo) {
                found = wal.data() + off + WAL_FRAME_HDR_SIZE;
            }
        }
        if (found) {
            return found;
        }
        size_t dbOff = 100 + (static_cast<size_t>(pageNo) - 1) * pageSize;
        if (pageNo == 1) {
            dbOff = 0; // Page 1 carries the 100-byte DB header.
        }
        if (dbOff + pageSize <= db.size()) {
            return db.data() + dbOff;
        }
        return nullptr;
    }
};

TEST(CdcWalTailer, ParsesSingleCommit) {
    Fixture f;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL);");
    f.exec("INSERT INTO t VALUES (1, 'alice', 1.5), (2, 'bob', NULL);");
    auto result = parseCurrentWal(f);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(4096u, result.log.header.pageSize);
    ASSERT_FALSE(result.log.commits.empty());
    // CREATE + INSERT each committed: at least two commit batches.
    EXPECT_GE(result.log.commits.size(), 2u);
    for (auto& batch : result.log.commits) {
        EXPECT_LE(batch.firstFrame, batch.lastFrame);
        EXPECT_NE(0u, batch.commitSize);
    }
}

TEST(CdcWalTailer, RejectsCorruption) {
    Fixture f;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY);");
    f.exec("INSERT INTO t VALUES (1);");
    auto bytes = f.walBytes();
    ASSERT_GT(bytes.size(), WAL_HDR_SIZE + WAL_FRAME_HDR_SIZE);
    // Corrupt a page-image byte: checksum must fail.
    bytes[WAL_HDR_SIZE + WAL_FRAME_HDR_SIZE] ^= 0xff;
    auto result = parseWal(bytes.data(), bytes.size());
    EXPECT_FALSE(result.ok);
}

TEST(CdcWalTailer, DropsTornTailFrame) {
    Fixture f;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY);");
    f.exec("INSERT INTO t VALUES (1);");
    auto bytes = f.walBytes();
    // Append a partial frame: header present, image truncated.
    bytes.insert(bytes.end(), WAL_FRAME_HDR_SIZE - 4, 0);
    auto result = parseWal(bytes.data(), bytes.size());
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_FALSE(result.log.commits.empty());
}

TEST(CdcPageDecoder, DecodesIntTextRealNull) {
    Fixture f;
    // NOTE: the INTEGER PRIMARY KEY column is a rowid alias: its record
    // slot holds NULL and the cell rowid is authoritative (verified against
    // real databases, see cdc_design.md). The extra INT column covers
    // stored integers.
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, n INT, name TEXT, score REAL);");
    f.exec("INSERT INTO t VALUES (1, 7, 'alice', 1.5), (2, -3, 'bob', NULL), (3, 0, '', 0.0);");
    auto parsed = parseCurrentWal(f);
    ASSERT_TRUE(parsed.ok) << parsed.error;

    FixturePages pages{f.walBytes(), f.dbBytes(), parsed.log.header.pageSize};
    // sqlite_master root page lookup for table t.
    uint32_t rootPage = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(f.db, "SELECT rootpage FROM sqlite_master WHERE name='t'", -1, &stmt,
                nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(stmt));
        rootPage = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
        sqlite3_finalize(stmt);
    }
    ASSERT_NE(0u, rootPage);
    const uint8_t* image = pages.fetch(rootPage);
    ASSERT_NE(nullptr, image);
    PageFetcher fetcher = [&](uint32_t pg) { return pages.fetch(pg); };
    auto decoded = decodeTableLeafPage(image, pages.pageSize, pages.pageSize, 0, 1, fetcher);
    ASSERT_TRUE(decoded.ok) << decoded.error;
    ASSERT_EQ(3u, decoded.rows.size());
    EXPECT_EQ(1, decoded.rows[0].rowid);
    ASSERT_EQ(4u, decoded.rows[0].values.size());
    EXPECT_EQ(SqliteValue::Type::Null, decoded.rows[0].values[0].type);
    EXPECT_EQ(SqliteValue::Type::Integer, decoded.rows[0].values[1].type);
    EXPECT_EQ(7, decoded.rows[0].values[1].intVal);
    EXPECT_EQ(SqliteValue::Type::Text, decoded.rows[0].values[2].type);
    EXPECT_EQ("alice", decoded.rows[0].values[2].textVal);
    EXPECT_EQ(SqliteValue::Type::Real, decoded.rows[0].values[3].type);
    EXPECT_DOUBLE_EQ(1.5, decoded.rows[0].values[3].realVal);
    EXPECT_EQ(2, decoded.rows[1].rowid);
    EXPECT_EQ(-3, decoded.rows[1].values[1].intVal);
    EXPECT_EQ(SqliteValue::Type::Null, decoded.rows[1].values[3].type);
    EXPECT_EQ(3, decoded.rows[2].rowid);
    EXPECT_EQ("", decoded.rows[2].values[2].textVal);
}

TEST(CdcPageDecoder, DecodesBlobsAndOverflowRows) {
    Fixture f;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, payload BLOB);");
    // 100KB blob forces overflow chains; small blob stays local.
    std::string big(100000, 'x');
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(f.db, "INSERT INTO t VALUES (1, ?), (2, zeroblob(10));",
                             -1, &stmt, nullptr));
    ASSERT_EQ(SQLITE_OK,
        sqlite3_bind_blob(stmt, 1, big.data(), static_cast<int>(big.size()), SQLITE_TRANSIENT));
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(stmt));
    sqlite3_finalize(stmt);

    auto parsed = parseCurrentWal(f);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    FixturePages pages{f.walBytes(), f.dbBytes(), parsed.log.header.pageSize};
    uint32_t rootPage = 0;
    {
        sqlite3_stmt* q = nullptr;
        ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(f.db, "SELECT rootpage FROM sqlite_master WHERE name='t'", -1, &q,
                nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(q));
        rootPage = static_cast<uint32_t>(sqlite3_column_int(q, 0));
        sqlite3_finalize(q);
    }
    PageFetcher fetcher = [&](uint32_t pg) { return pages.fetch(pg); };
    auto decoded =
        decodeTableLeafPage(pages.fetch(rootPage), pages.pageSize, pages.pageSize, 0, 1, fetcher);
    ASSERT_TRUE(decoded.ok) << decoded.error;
    ASSERT_EQ(2u, decoded.rows.size());
    EXPECT_EQ(1, decoded.rows[0].rowid);
    ASSERT_EQ(SqliteValue::Type::Blob, decoded.rows[0].values[1].type);
    EXPECT_EQ(big.size(), decoded.rows[0].values[1].blobVal.size());
    EXPECT_EQ(0, memcmp(big.data(), decoded.rows[0].values[1].blobVal.data(), big.size()));
    ASSERT_EQ(SqliteValue::Type::Blob, decoded.rows[1].values[1].type);
    EXPECT_EQ(10u, decoded.rows[1].values[1].blobVal.size());
}

TEST(CdcPageDecoder, ReflectsUpdateAndDelete) {
    Fixture f;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INT);");
    f.exec("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);");
    f.exec("UPDATE t SET v = 99 WHERE id = 2;");
    f.exec("DELETE FROM t WHERE id = 1;");
    auto parsed = parseCurrentWal(f);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_GE(parsed.log.commits.size(), 3u);

    FixturePages pages{f.walBytes(), f.dbBytes(), parsed.log.header.pageSize};
    uint32_t rootPage = 0;
    {
        sqlite3_stmt* q = nullptr;
        ASSERT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(f.db, "SELECT rootpage FROM sqlite_master WHERE name='t'", -1, &q,
                nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(q));
        rootPage = static_cast<uint32_t>(sqlite3_column_int(q, 0));
        sqlite3_finalize(q);
    }
    PageFetcher fetcher = [&](uint32_t pg) { return pages.fetch(pg); };
    // Newest image of the root page must show the post-update/delete state.
    auto decoded =
        decodeTableLeafPage(pages.fetch(rootPage), pages.pageSize, pages.pageSize, 0, 1, fetcher);
    ASSERT_TRUE(decoded.ok) << decoded.error;
    ASSERT_EQ(2u, decoded.rows.size());
    EXPECT_EQ(2, decoded.rows[0].rowid);
    EXPECT_EQ(99, decoded.rows[0].values[1].intVal);
    EXPECT_EQ(3, decoded.rows[1].rowid);
    EXPECT_EQ(30, decoded.rows[1].values[1].intVal);
}

} // namespace
