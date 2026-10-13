// Phase-2 CDC tests: OpResolver turns WAL commit frames into row ops
// without table scans. The SyncFixture below plays the future sync loop:
// parse WAL, group commits, apply incrementally, reconcile on restart --
// and accumulates batches into a shadow map validated against SQL ground
// truth (exactly what the phase-3 applier will do).

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cdc/btree_page_decoder.h"
#include "cdc/op_resolver.h"
#include "cdc/wal_tailer.h"
#include "gtest/gtest.h"
#include "sqlite3.h"

using namespace lbug::sqlite_extension::cdc;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

struct SyncFixture {
    std::string dir;
    std::string dbPath;
    sqlite3* db = nullptr;

    SyncFixture() {
        static int counter = 0;
        dir = std::filesystem::temp_directory_path().string() + "/lbug_cdc_op_" +
              std::to_string(::getpid()) + "_" + std::to_string(counter++);
        std::filesystem::create_directories(dir);
        dbPath = dir + "/test.db";
        open();
    }
    ~SyncFixture() {
        if (db) {
            sqlite3_close(db);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void open() {
        EXPECT_EQ(SQLITE_OK, sqlite3_open(dbPath.c_str(), &db));
        exec("PRAGMA journal_mode=WAL;");
    }
    void reopen() {
        EXPECT_EQ(SQLITE_OK, sqlite3_close(db));
        db = nullptr;
        open();
    }
    void exec(const std::string& sql) {
        char* err = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err))
            << (err ? err : sql);
        sqlite3_free(err);
    }
    struct Snap {
        WalParseResult parsed;
        std::vector<uint8_t> dbBytes;
    };
    Snap snap() {
        Snap s;
        auto wal = readFile(dbPath + "-wal");
        if (!wal.empty()) {
            s.parsed = parseWal(wal.data(), wal.size());
        } else {
            s.parsed.ok = true; // No WAL yet: nothing committed via WAL.
        }
        s.dbBytes = readFile(dbPath);
        return s;
    }
    struct FullSchema {
        TableSchema schema;
        int ipkIndex = -1; // column index of INTEGER PRIMARY KEY, if any
    };
    std::vector<FullSchema> fullSchemas() {
        std::vector<FullSchema> out;
        sqlite3_stmt* q = nullptr;
        EXPECT_EQ(SQLITE_OK,
            sqlite3_prepare_v2(db,
                "SELECT name, rootpage, sql FROM sqlite_master WHERE type='table' AND "
                "name NOT LIKE 'sqlite_%' ORDER BY name",
                -1, &q, nullptr));
        while (sqlite3_step(q) == SQLITE_ROW) {
            FullSchema fs;
            fs.schema.name = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
            fs.schema.rootPage = static_cast<uint32_t>(sqlite3_column_int(q, 1));
            const char* sql = reinterpret_cast<const char*>(sqlite3_column_text(q, 2));
            fs.schema.withoutRowid =
                sql != nullptr && std::string(sql).find("WITHOUT ROWID") != std::string::npos;
            // Primary-key detection via PRAGMA table_info: a single INT
            // PK is rowid-sourced, a single non-INT PK is record-sourced.
            sqlite3_stmt* p = nullptr;
            std::string pragma = "PRAGMA table_info(\"" + fs.schema.name + "\")";
            if (sqlite3_prepare_v2(db, pragma.c_str(), -1, &p, nullptr) == SQLITE_OK) {
                int pkCols = 0;
                int pkIdx = -1;
                bool pkIsInt = false;
                int colIdx = 0;
                while (sqlite3_step(p) == SQLITE_ROW) {
                    const char* type = reinterpret_cast<const char*>(sqlite3_column_text(p, 2));
                    int pk = sqlite3_column_int(p, 5);
                    std::string upper = type ? type : "";
                    for (auto& ch : upper) {
                        ch = static_cast<char>(toupper(ch));
                    }
                    if (pk > 0) {
                        pkCols++;
                        pkIdx = colIdx;
                        pkIsInt = upper.find("INT") != std::string::npos;
                    }
                    colIdx++;
                }
                if (pkCols == 1) {
                    fs.schema.pkRecordIndex = pkIdx;
                    fs.schema.pkFromRowid = pkIsInt;
                    if (pkIsInt) {
                        fs.ipkIndex = pkIdx;
                    }
                }
            }
            sqlite3_finalize(p);
            out.push_back(std::move(fs));
        }
        sqlite3_finalize(q);
        return out;
    }
    std::vector<TableSchema> schemas() {
        auto full = fullSchemas();
        std::vector<TableSchema> out;
        for (auto& fs : full) {
            out.push_back(fs.schema);
        }
        return out;
    }
    // WAL-first overlay limited to frames[0, frameLimit) so uncommitted
    // tails never leak into decoded state. Frame images are copied: the
    // overlay must outlive the snap (production will mmap instead).
    PageFetcher overlay(const Snap& snap, size_t frameLimit, uint32_t pageSize) {
        struct Shared {
            std::vector<std::vector<uint8_t>> frames;
            std::unordered_map<uint32_t, const uint8_t*> images;
            std::vector<uint8_t> db;
            uint32_t pageSize = 0;
        };
        auto shared = std::make_shared<Shared>();
        shared->db = snap.dbBytes;
        shared->pageSize = pageSize;
        if (snap.parsed.ok) {
            size_t n = std::min(frameLimit, snap.parsed.log.frames.size());
            shared->frames.reserve(n);
            for (size_t i = 0; i < n; i++) {
                shared->frames.push_back(snap.parsed.log.frames[i].image);
                shared->images[snap.parsed.log.frames[i].pageNo] = shared->frames.back().data();
            }
        }
        return [shared](uint32_t pageNo) -> const uint8_t* {
            auto it = shared->images.find(pageNo);
            if (it != shared->images.end()) {
                return it->second;
            }
            size_t off =
                pageNo == 1 ? 0 : 100 + (static_cast<size_t>(pageNo) - 1) * shared->pageSize;
            if (off + shared->pageSize <= shared->db.size()) {
                return shared->db.data() + off;
            }
            return nullptr;
        };
    }
};

// Shadow copy of resolver output, mirroring the future applier.
struct ShadowTable {
    int ipkIndex = -1;
    std::map<int64_t, std::vector<SqliteValue>> rows; // rowid -> record values
};
using ShadowDb = std::map<std::string, ShadowTable>;

struct SyncLoop {
    OpResolver resolver;
    WalClock clock;
    ShadowDb shadow;
    std::vector<std::string> dropped;
    std::vector<std::string> skipped;
    std::vector<TableSchema> lastSchemas;
    std::vector<RowDelete> allDeletes;

    // Production-shaped loop: incremental per commit while the schema is
    // stable; any batch touching page 1 (DDL) or changing schemas falls
    // back to reconcile, which diffs current full state and converges
    // without era-accurate schema tracking.
    bool sync(SyncFixture& f, std::string& error) {
        auto snap = f.snap();
        if (!snap.parsed.ok) {
            error = snap.parsed.error;
            return false;
        }
        uint32_t pageSize = 4096;
        if (!snap.parsed.log.frames.empty()) {
            pageSize = snap.parsed.log.header.pageSize;
        } else if (snap.dbBytes.size() >= 100) {
            pageSize = (static_cast<uint32_t>(snap.dbBytes[16]) << 8) | snap.dbBytes[17];
        }
        auto schemas = f.schemas();
        bool needReconcile =
            !snap.parsed.log.frames.empty() && walRestarted(clock, snap.parsed.log.header);
        if (!needReconcile) {
            // DDL inside the new frames invalidates era assumptions.
            for (size_t i = clock.appliedFrames; i < snap.parsed.log.frames.size(); i++) {
                if (snap.parsed.log.frames[i].pageNo == 1) {
                    needReconcile = true;
                    break;
                }
            }
            needReconcile = needReconcile || schemas != lastSchemas;
        }
        if (needReconcile) {
            auto r = resolver.reconcile(schemas,
                f.overlay(snap, snap.parsed.log.frames.size(), pageSize), pageSize);
            if (!r.ok) {
                error = r.error;
                return false;
            }
            applyBatch(f, r.batch);
            if (!snap.parsed.log.frames.empty()) {
                clock.salt1 = snap.parsed.log.header.salt1;
                clock.salt2 = snap.parsed.log.header.salt2;
                clock.appliedFrames = snap.parsed.log.frames.size();
            }
            lastSchemas = schemas;
            return true;
        }
        for (auto& commit : snap.parsed.log.commits) {
            if (commit.lastFrame < clock.appliedFrames) {
                continue; // Already applied.
            }
            std::vector<uint32_t> dirty;
            for (size_t i = clock.appliedFrames; i <= commit.lastFrame; i++) {
                dirty.push_back(snap.parsed.log.frames[i].pageNo);
            }
            auto r = resolver.applyFrames(schemas, dirty,
                f.overlay(snap, commit.lastFrame + 1, pageSize), pageSize);
            if (!r.ok) {
                error = r.error;
                return false;
            }
            applyBatch(f, r.batch);
            clock.appliedFrames = commit.lastFrame + 1;
        }
        if (!snap.parsed.log.frames.empty()) {
            clock.salt1 = snap.parsed.log.header.salt1;
            clock.salt2 = snap.parsed.log.header.salt2;
        }
        lastSchemas = schemas;
        return true;
    }

    void applyBatch(SyncFixture& f, const OpBatch& batch) {
        auto full = f.fullSchemas();
        std::unordered_map<std::string, int> ipk;
        for (auto& fs : full) {
            ipk[fs.schema.name] = fs.ipkIndex;
        }
        applyBatch(ipk, batch);
    }

    void applyBatch(const std::unordered_map<std::string, int>& ipk, const OpBatch& batch) {
        for (auto& del : batch.deletes) {
            allDeletes.push_back(del);
        }
        for (auto& op : batch.upserts) {
            shadow[op.table].rows[op.rowid] = op.values;
            auto it = ipk.find(op.table);
            shadow[op.table].ipkIndex = it != ipk.end() ? it->second : -1;
        }
        for (auto& del : batch.deletes) {
            auto it = shadow.find(del.table);
            if (it != shadow.end()) {
                it->second.rows.erase(del.rowid);
            }
        }
        for (auto& t : batch.droppedTables) {
            shadow.erase(t);
            dropped.push_back(t);
        }
        for (auto& t : batch.skippedTables) {
            skipped.push_back(t);
        }
    }
};

// Compares the shadow accumulated from op batches against SQL ground truth.
void expectShadowMatchesSql(SyncFixture& f, const ShadowDb& shadow) {
    auto full = f.fullSchemas();
    size_t expectTables = 0;
    for (auto& fs : full) {
        if (!fs.schema.withoutRowid) {
            expectTables++;
        }
    }
    ASSERT_EQ(expectTables, shadow.size());
    for (auto& fs : full) {
        if (fs.schema.withoutRowid) {
            continue;
        }
        auto it = shadow.find(fs.schema.name);
        ASSERT_TRUE(it != shadow.end()) << fs.schema.name;
        std::string sql = "SELECT rowid, * FROM \"" + fs.schema.name + "\" ORDER BY rowid";
        sqlite3_stmt* q = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(f.db, sql.c_str(), -1, &q, nullptr));
        size_t count = 0;
        while (sqlite3_step(q) == SQLITE_ROW) {
            int64_t rowid = sqlite3_column_int64(q, 0);
            auto rit = it->second.rows.find(rowid);
            ASSERT_TRUE(rit != it->second.rows.end()) << fs.schema.name << " rowid " << rowid;
            int ncols = sqlite3_column_count(q) - 1;
            ASSERT_EQ(static_cast<size_t>(ncols), rit->second.size()) << "column count";
            for (int c = 0; c < ncols; c++) {
                // IPK slot holds a NULL placeholder; the rowid is authoritative.
                if (c == fs.ipkIndex) {
                    EXPECT_EQ(SqliteValue::Type::Null, rit->second[c].type);
                    EXPECT_EQ(rowid, sqlite3_column_int64(q, c + 1));
                    continue;
                }
                int ctype = sqlite3_column_type(q, c + 1);
                auto& v = rit->second[c];
                if (ctype == SQLITE_NULL) {
                    EXPECT_EQ(SqliteValue::Type::Null, v.type);
                } else if (ctype == SQLITE_INTEGER) {
                    ASSERT_EQ(SqliteValue::Type::Integer, v.type);
                    EXPECT_EQ(sqlite3_column_int64(q, c + 1), v.intVal);
                } else if (ctype == SQLITE_FLOAT) {
                    ASSERT_EQ(SqliteValue::Type::Real, v.type);
                    EXPECT_DOUBLE_EQ(sqlite3_column_double(q, c + 1), v.realVal);
                } else if (ctype == SQLITE_TEXT) {
                    ASSERT_EQ(SqliteValue::Type::Text, v.type);
                    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(q, c + 1));
                    int bytes = sqlite3_column_bytes(q, c + 1);
                    EXPECT_EQ(std::string(text ? text : "", bytes), v.textVal);
                } else {
                    ASSERT_EQ(SqliteValue::Type::Blob, v.type);
                    const void* blob = sqlite3_column_blob(q, c + 1);
                    int bytes = sqlite3_column_bytes(q, c + 1);
                    ASSERT_EQ(static_cast<size_t>(bytes), v.blobVal.size());
                    EXPECT_EQ(0, memcmp(blob, v.blobVal.data(), bytes));
                }
            }
            count++;
        }
        sqlite3_finalize(q);
        EXPECT_EQ(it->second.rows.size(), count) << fs.schema.name;
    }
}

TEST(CdcOpResolver, BootstrapEmitsAllRows) {
    SyncFixture f;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INT);");
    f.exec("INSERT INTO t VALUES (1, 10), (2, 20);");
    OpResolver resolver;
    auto snap = f.snap();
    ASSERT_TRUE(snap.parsed.ok);
    uint32_t pageSize = snap.parsed.log.header.pageSize;
    auto r = resolver.bootstrap(f.schemas(),
        f.overlay(snap, snap.parsed.log.frames.size(), pageSize), pageSize);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(2u, r.batch.upserts.size());
    EXPECT_TRUE(r.batch.deletes.empty());
    EXPECT_EQ(2u, resolver.trackedRowCount("t"));
    // Bootstrap is blind: a second bootstrap re-emits everything.
    auto r2 = resolver.bootstrap(f.schemas(),
        f.overlay(snap, snap.parsed.log.frames.size(), pageSize), pageSize);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(2u, r2.batch.upserts.size());
}

TEST(CdcOpResolver, IncrementalInsertUpdateDelete) {
    SyncFixture f;
    SyncLoop loop;
    std::string error;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INT);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_EQ(0u, loop.shadow["t"].rows.size());

    f.exec("INSERT INTO t VALUES (1, 10), (2, 20);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_EQ(2u, loop.shadow["t"].rows.size());
    EXPECT_EQ(2u, loop.resolver.trackedRowCount("t"));

    f.exec("UPDATE t SET v = 99 WHERE id = 2;");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_EQ(99, loop.shadow["t"].rows[2][1].intVal);

    f.exec("DELETE FROM t WHERE id = 1;");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_EQ(1u, loop.shadow["t"].rows.size());
    EXPECT_EQ(1u, loop.resolver.trackedRowCount("t"));
    expectShadowMatchesSql(f, loop.shadow);
}

TEST(CdcOpResolver, MixedSequenceConverges) {
    SyncFixture f;
    SyncLoop loop;
    std::string error;
    f.exec("CREATE TABLE a(id INTEGER PRIMARY KEY, v TEXT);");
    f.exec("CREATE TABLE b(id INTEGER PRIMARY KEY, v INT);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    f.exec("INSERT INTO a VALUES (1, 'x'), (2, 'y');");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    f.exec("INSERT INTO b VALUES (1, 1);");
    f.exec("UPDATE a SET v = 'z' WHERE id = 1;");
    f.exec("DELETE FROM b WHERE id = 1;");
    f.exec("INSERT INTO b VALUES (5, 50), (6, 60);");
    // Sync once across four commits: batching is order-independent.
    ASSERT_TRUE(loop.sync(f, error)) << error;
    expectShadowMatchesSql(f, loop.shadow);
    // And reconcile afterwards must be quiet: steady state converged.
    auto snap = f.snap();
    auto r = loop.resolver.reconcile(f.schemas(),
        f.overlay(snap, snap.parsed.log.frames.size(), snap.parsed.log.header.pageSize),
        snap.parsed.log.header.pageSize);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.batch.upserts.empty());
    EXPECT_TRUE(r.batch.deletes.empty());
}

TEST(CdcOpResolver, DropAndCreateTable) {
    SyncFixture f;
    SyncLoop loop;
    std::string error;
    f.exec("CREATE TABLE gone(id INTEGER PRIMARY KEY, v INT);");
    f.exec("INSERT INTO gone VALUES (1, 1);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_EQ(1u, loop.shadow["gone"].rows.size());

    f.exec("DROP TABLE gone;");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_TRUE(loop.dropped.end() != std::find(loop.dropped.begin(), loop.dropped.end(), "gone"));
    EXPECT_TRUE(loop.shadow.find("gone") == loop.shadow.end());
    f.exec("CREATE TABLE fresh(id INTEGER PRIMARY KEY, v INT);");
    f.exec("INSERT INTO fresh VALUES (7, 70);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_EQ(1u, loop.shadow["fresh"].rows.size());
    expectShadowMatchesSql(f, loop.shadow);
}

TEST(CdcOpResolver, CheckpointRestartRecoversMissedCommit) {
    SyncFixture f;
    SyncLoop loop;
    std::string error;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v INT);");
    f.exec("INSERT INTO t VALUES (1, 1);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    // A commit the loop never sees, then close (checkpoint) + reopen.
    f.exec("INSERT INTO t VALUES (2, 2);");
    f.reopen();
    f.exec("INSERT INTO t VALUES (3, 3);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    expectShadowMatchesSql(f, loop.shadow);
    EXPECT_EQ(3u, loop.shadow["t"].rows.size());
}

TEST(CdcOpResolver, StressManyRowsAndSplits) {
    SyncFixture f;
    SyncLoop loop;
    std::string error;
    f.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    for (int batch = 0; batch < 10; batch++) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = 0; i < 500; i++) {
            int id = batch * 500 + i;
            sql += "(" + std::to_string(id) + ", 'val" + std::to_string(id) + "'),";
        }
        sql.back() = ';';
        f.exec(sql);
        ASSERT_TRUE(loop.sync(f, error)) << error;
    }
    EXPECT_EQ(5000u, loop.shadow["t"].rows.size());
    EXPECT_EQ(5000u, loop.resolver.trackedRowCount("t"));
    expectShadowMatchesSql(f, loop.shadow);
}

TEST(CdcOpResolver, ExplicitPkIdentitySurvivesDelete) {
    SyncFixture f;
    SyncLoop loop;
    std::string error;
    f.exec("CREATE TABLE node_doc(code TEXT PRIMARY KEY, v INT);");
    f.exec("INSERT INTO node_doc VALUES ('k1', 1), ('k2', 2);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    f.exec("DELETE FROM node_doc WHERE code = 'k1';");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_EQ(1u, loop.shadow["node_doc"].rows.size());
    // The delete must carry the retained TEXT PK value, not just a rowid.
    ASSERT_EQ(1u, loop.allDeletes.size());
    EXPECT_EQ("node_doc", loop.allDeletes[0].table);
    ASSERT_TRUE(loop.allDeletes[0].hasIdentity);
    ASSERT_EQ(SqliteValue::Type::Text, loop.allDeletes[0].identity.type);
    EXPECT_EQ("k1", loop.allDeletes[0].identity.textVal);
    expectShadowMatchesSql(f, loop.shadow);
}

TEST(CdcOpResolver, SkipsWithoutRowidTables) {
    SyncFixture f;
    SyncLoop loop;
    std::string error;
    f.exec("CREATE TABLE w(id INT PRIMARY KEY, v INT) WITHOUT ROWID;");
    f.exec("INSERT INTO w VALUES (1, 1);");
    ASSERT_TRUE(loop.sync(f, error)) << error;
    EXPECT_TRUE(loop.skipped.end() != std::find(loop.skipped.begin(), loop.skipped.end(), "w"));
    EXPECT_TRUE(loop.shadow.find("w") == loop.shadow.end());
}

} // namespace
