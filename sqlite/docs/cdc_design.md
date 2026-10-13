# SQLite WAL → Ladybug CDC (Debezium-style, no broker)

## Goal

Replicate SQLite writes into Ladybug native tables with Debezium-style
log-based CDC, minus the overhead: no Kafka/JVM, no LTX sidecar, no file
copies, no table scans. The SQLite WAL **is** the commit log; a C++ tailer
inside the `sqlite` extension reads it in place and resolves page images
into row ops applied to Ladybug in commit order.

Non-goals: multi-master, syncing Ladybug → SQLite (one direction only),
DDL beyond table create/drop/add-column in phase 1.

## Logical clock

The WAL frame index is the LSN:

- Frame headers carry page number + "database size after commit". A frame
  with nonzero commit size is a **commit boundary**. Only frames up to the
  last commit frame are visible; the uncommitted tail is never applied.
- The replication cursor is `(walSalt1, walSalt2, appliedFrame)`, stored
  durably in Ladybug and advanced **in the same transaction** as the applied
  row ops (the local equivalent of Debezium offset commits; replay after a
  crash is safe because applies are idempotent PK upserts).
- Crash recovery = resume from the cursor. Uncommitted tail frames are
  re-read, never double-applied.

## Components (`extension/sqlite/src/cdc/`)

```
wal_tailer   mmap the -wal file, verify header salts + per-frame checksums,
             batch frames up to each commit boundary. Output: CommitBatch
             {salt, firstFrame, lastFrame, [(pageNo, pageImage)]}.
page_decoder Decode table-b-tree LEAF pages (0x0D) into (rowid, record)
             rows using sqlite_master column order. Follows overflow chains
             (WAL first, then DB file). Skips interior/index/freelist pages.
op_resolver  Page images -> row ops WITHOUT table scans (see below).
applier      Row ops -> Ladybug native writes + cursor advance, one txn per
             WAL commit.
```

## Op resolution without table scans

Page images are after-images, not logical ops. The resolver keeps bounded
per-page state instead of re-reading tables:

- State: `pageNo -> {PKs}` for every applied leaf page, plus the table/PK
  mapping. Built once at bootstrap (one full scan — Debezium's snapshot
  phase; the only scan, ever).
- Per WAL commit, two phases:
  1. **Upserts**: decode all new images of dirty leaf pages, key by PK.
     A row that moved pages on UPDATE (page A -> page B) reconciles here.
  2. **Deletes**: `(old page PK-set - new page PK-set) - PKs upserted
     elsewhere in the same commit`.
- DDL is a dirty page 1 (`sqlite_master`): re-resolve schema, emit a
  schema-change event, evolve the mapping. No special channel needed.

## Apply path

One WAL commit → one Ladybug transaction: node/rel upserts + deletes by
PK, then cursor advance. Within a commit, PK-keyed last-writer-wins; across
commits, frame order is the total order. `INTEGER PRIMARY KEY` aliases
rowid; other PKs (including `WITHOUT ROWID` tables) use their declared key.

## Edge cases

- **Checkpoint / WAL restart**: salt changes -> reset frame cursor to 0.
  Page->PK sets are logical state and stay valid.
- **Torn writes / races with the writer**: frame checksums (SQLite native
  checksum) + salt re-validation before each batch; on mismatch, re-baseline
  from the DB file's `mxFrame`.
- **Large transactions**: multi-frame commits apply atomically at the
  commit frame.
- **Overflow-heavy rows**: decoder follows chains; missing pages (not yet
  checkpointed, not in WAL window) abort the batch, which retries after the
  next commit.

## Sync mapping (open question)

Target tables: auto-create Ladybug node tables from the SQLite schema, or
map explicitly (SQLite table X -> existing Ladybug table Y)? Default:
auto-create `sqlite_<table>` node tables; explicit mapping via ATTACH
option later.

## Phase plan

1. `wal_tailer` + `page_decoder` with a frame/row-dump unit test,
   cross-checked against Python `sqlite3` ground truth. No engine writes.
2. `op_resolver` (page PK-sets, two-phase commit, bootstrap snapshot).
3. `applier` + cursor store + `SYNC FROM <db>` statement; background
   tailer thread later on the same code path.
4. e2e: write workload on SQLite (WAL mode) -> assert identical Ladybug
   tables, including deletes, page-move UPDATEs, checkpoint + restart.

## References

- SQLite WAL format: https://sqlite.org/wal.html
- SQLite file format (b-tree pages, records, serial types):
  https://sqlite.org/fileformat.html
- LTX (prior art for page-level replication, Go — deliberately not linked):
  https://github.com/superfly/ltx
- Debezium (logical model: snapshot + log, offsets, tombstones):
  https://debezium.io/documentation/reference/stable/architecture.html
