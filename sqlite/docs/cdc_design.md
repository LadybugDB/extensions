# SQLite WAL → Ladybug CDC (Debezium-style, no broker)

Replicate SQLite writes into Ladybug native tables with Debezium-style
log-based CDC, minus the overhead: no Kafka/JVM, no LTX sidecar, no file
copies, no table scans. The SQLite WAL **is** the commit log; a C++ tailer
inside the `sqlite` extension reads it in place and resolves page images
into row ops applied to Ladybug in commit order.

Status: implemented, all green (see Testing). What's left is listed under
Limitations.

## Usage

```sql
LOAD EXTENSION 'sqlite';
-- Optional explicit registration (register_node/register_edge semantics).
-- Unregistered node_*/rel_* tables are inferred (see Mapping).
CALL sqlite_register_node('/data/graph.db', 'node_person') RETURN *;
CALL sqlite_register_edge('/data/graph.db', 'rel_works') RETURN *;
-- One sync tick: tail WAL, resolve, apply, advance cursor.
CALL sqlite_sync('/data/graph.db') RETURN *;
-- tables_synced | rows_upserted | rows_deleted | applied_frames | status
```

Sync identity is the normalized absolute file path. No ATTACH is needed;
the engine opens the file read-only itself. The same file must NOT be
attached into the same graph while synced (attached shadows share bare
table names with synced native tables, so DDL would silently no-op and
writes would bind the foreign shadow). The engine refuses an attached
file with a DETACH directive, and heals stale shadows left by older
DETACHes.

## Logical clock

The WAL frame index is the LSN:

- Frame headers carry page number + "database size after commit". A frame
  with nonzero commit size is a **commit boundary**. Only frames up to the
  last commit frame are visible; the uncommitted tail is never applied.
- The replication cursor is `(walSalt1, walSalt2, appliedFrames)`,
  persisted in the `__sqlite_cdc_cursor` node table. It is written **last**
  (at-least-once semantics); replays are safe because applies are
  idempotent and the cursor only moves forward.
- Crash recovery = resume from the cursor. Uncommitted tail frames are
  re-read, never double-applied. Multi-commit batches apply atomically
  per commit for correctness, but converge identically when coalesced:
  upserts use newest images, deletes diff post-state.

## Components

```
extension/sqlite/src/cdc/          dependency-free (STL only), hermetic tests
  wal_tailer      Parse + verify WAL bytes (magic/format, header + chained
                  per-frame checksums, per-frame salts), drop torn tails,
                  group commit batches. Pure function over bytes.
  btree_decoder   Table-leaf (0x0D) cells -> (rowid, record) rows with
                  serial-type decoding; overflow chains (WAL first, then
                  DB file); interior (0x05) walks for leaf attribution.
                  INTEGER PRIMARY KEY slots hold NULL in records; the cell
                  rowid is authoritative.
  op_resolver     Page images -> row ops without table scans (below).
  sync_mapping    register_node/register_edge semantics + convention
                  inference (below).
  sync_cypher     DDL + UNWIND-batched DML + cursor statements as pure
                  strings (below).

extension/sqlite/src/function/
  sync_engine     One tick: read WAL + DB bytes, load cursor, schemas, and
                  explicit mappings; tail/resolve via cdc/; execute through
                  a dedicated inner ClientContext (reusing the outer CALL's
                  context self-deadlocks on its mutex); cursor last.
                  Resolver memory persists across CALLs (process-lifetime,
                  keyed by path; a zero cursor resets it).
  sync_functions  CALL sqlite_sync / sqlite_register_node /
                  sqlite_register_edge table functions (per-arity overloads).
```

## Op resolution without table scans

Page images are after-images, not logical ops. The resolver keeps bounded
per-page state instead of re-reading tables:

- State per table: `pageNo -> {rowids}` + page-image hashes, plus retained
  PK identities (explicit-PK tables) and retained records (edge tables).
  Identity is the **rowid** for rowid tables. Built once at bootstrap (one
  full scan — Debezium's snapshot phase; the only scan, ever).
- Table ownership without scans: per batch, walk each table's interior
  pages (interior reads only, never leaf row data) for the current leaf
  set; dirty pages from the commit frames select which leaves to
  (re-)decode. Splits, frees, and rebuilds fall out of leaf-set diffs.
- Per WAL commit, two phases:
  1. **Upserts**: decode new images of dirty leaf pages, key by rowid. A
     row that moved pages reconciles here.
  2. **Deletes**: `(old page rowid-set - new page rowid-set) - rowids
     upserted elsewhere in the same commit`.
- `reconcile()` re-diffs after WAL restarts, skipping hash-identical
  pages without decoding, so no-op restarts converge nearly free.
- The sync loop applies incrementally while the schema is stable; any
  batch touching page 1 (`sqlite_master`, i.e. containing DDL) falls back
  to `reconcile`, which converges without era-accurate schema tracking.

## Mapping (register_node / register_edge semantics)

Like pg_ladybug's `register_node` / `register_edge`: label <-> table, id
column, endpoint columns, props, with upsert behavior, persisted in the
`__sqlite_cdc_map` node table. The `node_*` / `rel_*` / `csr_rel_*`
naming convention auto-populates it:

- `node_*` -> node table (label keeps the full table name, matching
  pg_client catalog behavior). Identity is the single-column PK:
  rowid-sourced for INTEGER PRIMARY KEY, record-sourced otherwise. No
  single PK -> skipped, not guessed.
- `rel_*` / `csr_rel_*` -> edge table. Endpoints require FK-backed
  src/dst columns resolving to mapped node tables; without them the edge
  cannot be oriented safely, so it is skipped. `csr_rel_*` sets a
  contract flag for later bulk-load; both sync as edges today.
- Anything else -> skipped (keeps today's foreign-scan behavior).

Explicit rows win per table; nodes resolve before edges regardless of
iteration order; inferred edges rebind to final node labels.

## Apply path

Resolved batches become `CREATE ... IF NOT EXISTS` DDL (nodes before
edges) and UNWIND-batched DML, executed statement-by-statement through
the inner context:

- Node upserts: `MERGE` by PK (`SET` non-key props). Deletes: PK match +
  `DETACH DELETE` (explicit-PK deletes use resolver-retained values).
- Edge upserts: delete-by-`_sqlite_rowid`-slot + `CREATE`. The slot is
  required because SQLite reuses rowids after deletes (max+1
  allocation): an endpoint-keyed MERGE cannot replace a moved edge,
  while a reused PK value IS the same node identity (Debezium semantics).
- Value rendering is strict like the scan (structural mismatches are
  builder errors); unparseable date strings pass through to fail at
  execution with engine context. Doubles render shortest-round-trip;
  blobs render as `BLOB('\x..')` literals.

## Required core fixes (parent repo)

Built as part of this work; sync depends on each:

- DETACH removes its shadow entries (node + rel-group) from the main
  catalog; previously they leaked and crashed planning with a dangling
  database reference.
- DETACH clears the default database when it points at the detached
  alias (the first ATTACH becomes the default; every node-pattern bind
  resolves it).
- `isLbugDatabase` returns false for unknown databases instead of
  throwing from a predicate.
- ATTACH/DETACH are write-classified so catalog changes commit (they
  mutated state inside read transactions before).

## Background daemon (no polling loop required)

Manual `CALL sqlite_sync(path)` suits schedulers; for keep-it-fresh
operation the extension runs one daemon thread per file:

```sql
CALL sqlite_sync_start('/data/graph.db', 1000);  -- tick every 1s
CALL sqlite_sync_status() RETURN *;              -- running, applied, errors
CALL sqlite_sync_stop('/data/graph.db');         -- joins the thread
```

- Each tick is the same `SyncEngine::sync` a manual CALL runs (WAL
  parse -> resolve -> apply -> cursor), on a persistent per-daemon
  session. Ticks and manual CALLs mutually exclude per path via a tick
  mutex, so they never resolve/apply concurrently.
- Sessions are never shared across threads: the CALL's context mutex is
  held during execution, so nested use self-deadlocks; the daemon owns
  its `ClientContext`, and the engine's fan-out statements each run on
  short-lived inner contexts.
- Shutdown: the thread exits itself once the database closes (probed via
  a trivial query that throws post-close) and joins promptly on stop
  (interruptible sleep). Restart the daemon after a process restart;
  the cursor resumes it with no redundant work.
- Errors are recorded per daemon with backoff (up to 30s) instead of
  spinning; `status` surfaces `last_error` and consecutive-error counts.

## Limitations (future work)

- Resolver memory is process-lifetime: a restart converges upserts but
  may miss deletes that landed while down (the cursor itself is durable).
  Durable page-set state would close the gap.
- One sync (manual or daemon) per file per process; state is keyed by path.
- No WITHOUT ROWID or composite-PK materialization (skipped with
  reasons); no CSR bulk-load for `csr_rel_*` yet.
- No background tailer thread yet; sync is per-CALL (same code path will
  drive it).
- One direction only (SQLite -> Ladybug); no multi-master.

## Testing

- `sqlite_cdc_wal_test` (6): tailer + decoder against real WALs built by
  the vendored sqlite3 (commits, corruption rejection, torn tails,
  ints/text/real/null/blob, 100KB overflow chains, update/delete).
- `sqlite_cdc_op_resolver_test` (9): bootstrap, incremental
  insert/update/delete, mixed multi-commit batching, drop/create,
  checkpoint restart with a missed commit, 5000-row split stress,
  WITHOUT ROWID skip, explicit-PK and retained-record delete identity —
  all validated against SQL ground truth through a shadow applier.
- `sqlite_cdc_sync_test` (13): mapping inference + exact Cypher strings
  (DDL/DML/chunking/escaping/strictness/cursor).
- `sqlite_sync_api_test` (2): in-process ApiTest proving
  write->sync->write->sync convergence (updates, edge deletes across
  rowid reuse, mid-stream DDL, quiet syncs, attach guard).
- Existing suites stay green: 4/4 sqlite e2e, attach/detach, 524
  transaction, 289 ddl tests.

## References

- SQLite WAL format: https://sqlite.org/wal.html
- SQLite file format (b-tree pages, records, serial types):
  https://sqlite.org/fileformat.html
- LTX (prior art for page-level replication, Go — deliberately not linked
  or needed: the WAL itself is the log): https://github.com/superfly/ltx
- Debezium (logical model: snapshot + log, offsets, tombstones):
  https://debezium.io/documentation/reference/stable/architecture.html
