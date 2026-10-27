# Lance Extension

Lance-backed read-only graph storage for Ladybug: node tables and CSR (or flat)
relationship tables whose data lives in [Lance](https://lancedb.github.io/lance/)
datasets instead of Ladybug's native storage engine.

It is the Lance counterpart of the **icebug-disk** format (`docs/icebug-disk.md`):
same table layout and Cypher semantics, but every backing file is a Lance
dataset instead of a Parquet file.

## Quick start

```cypher
LOAD EXTENSION 'extension/lance/build/liblance.lbug_extension';

CREATE NODE TABLE user(id INT32 PRIMARY KEY, name STRING, age INT64)
  WITH (storage = 'graph/', format = 'lance');
CREATE NODE TABLE city(id INT32 PRIMARY KEY, name STRING, population INT64)
  WITH (storage = 'graph/', format = 'lance');
CREATE REL TABLE follows(FROM user TO user, since INT32)
  WITH (storage = 'graph/', format = 'lance');
CREATE REL TABLE livesin(FROM user TO city)
  WITH (storage = 'graph/', format = 'lance');

MATCH (a:user {id: 100})-[:follows]->(b)-[:livesin]->(c:city)
RETURN b.name, c.name ORDER BY b.name;
```

Expected files (see `docs/lance.md` for the full layout):

```
graph/nodes_user.lance      graph/nodes_city.lance
graph/indices_follows.lance graph/indptr_follows.lance
graph/indices_livesin.lance graph/indptr_livesin.lance
```

A relationship table whose `storage` points directly at a `<name>.lance`
dataset uses the FLAT layout (`src, dst, <props...>` rows) instead of CSR.

Ad-hoc reads without creating tables:

```cypher
CALL LANCE_SCAN('graph/nodes_user.lance') RETURN *;
LOAD FROM 'graph/nodes_user.lance' (file_format = 'lance') RETURN *;
COPY person FROM 'graph/nodes_user.lance' (file_format = 'lance');
```

## How it works

**Lance I/O is native.** The extension links the Lance Rust crates directly
(same versions as [lance-duckdb](https://github.com/lance-duckdb) `main`:
Lance 13, Arrow 58) through a small C FFI (`rust/`: open/count/schema/scan
to Arrow batches), with no DuckDB layer in between. Reads are dataset scans
with projection + limit/offset pushed into Lance, so morsel and CSR range
scans stay efficient; batches cross the FFI via the Arrow C data interface
and decode with core's `ArrowConverter` (the same machinery backing
`arrow://` tables). Building needs a Rust toolchain, protoc, and crates.io
access (see `rust/`); the CMake build drives cargo automatically.

**Storage layout mirrors icebug-disk** (`IceDiskNodeTable`/`IceDiskRelTable`
are the templates for `LanceNodeTable`/`LanceRelTable`): positional node
offsets, `indices` (edge targets + properties, CSR-sorted by source) plus a
single-column `indptr`, indptr cached in memory and range-limiting each
bound-node batch from O(edges) to O(matching edges) on forward scans — the
same optimization icebug-disk applies to Parquet row groups. Like icebug-disk
v1, tables are **immutable** (inserts/updates/deletes are rejected) and
edge properties match **by name** (column order is insignificant; missing
properties read as NULL).

**Plugging into core.** Storage table types cannot live behind a plain table
function (graph traversal needs CSR extend, degree summaries, PK lookup), so
core provides a small extension hook surface (see
`src/include/storage/table/external_table_hooks.h`, LadybugDB/ladybug#1445):

- `StorageFormat::LANCE` + `ExternalTableHookRegistry` (factories + scan hooks,
  keyed by format and by table);
- the scan operators (`ScanNodeTable`, `ScanRelTable`, `ScanMultiRelTable`,
  `OnDiskGraph` for recursive traversal) create scan states and count morsels
  through the hooks, falling back to the legacy paths otherwise;
- `LanceRelTableScanState` extends `IceDiskRelTableScanState` so one shared
  state can scan lance, icebug-disk, Arrow, and native tables in multi-rel
  scans (rank 3 in the coverage lattice).

No core query path changed behavior: all pre-existing icebug-disk/Arrow/native
tests pass unmodified.

## Relationship to lance-csr

[lance-csr](https://github.com/hyperdimensionalcomputingai/lance-csr) is a
Cypher engine over Lance datasets with an **in-memory** CSR index built from
edge tables at query time. This extension takes the complementary,
storage-first approach: the CSR (`indices` + `indptr`) is **materialized as
Lance datasets on disk** (the icebug-disk layout translated to Lance), so
graphs are portable files that any Lance reader can open, while traversal
still resolves hops through CSR range lookups instead of joins. Reads go
through the extension's own native reader (`rust/` + `LanceReader`); a
Rust-free build is not a goal.

## Regenerating test fixtures

```bash
./extension/lance/scripts/gen_test_data.sh
```

regenerates `extension/lance/test/lance_data/` with the extension's own Rust
generator (`rust/examples/gen_fixtures.rs`, same Lance version as the reader).
Content mirrors `dataset/ice-disk-test` row-for-row; every dataset is stamped
with `lance_version = v1` schema metadata (plus a `v999` dataset for the
version-mismatch error test).

## Limitations (v1)

- Read-only tables; no `COPY ... TO ... (FORMAT lance)` write path yet (write
  Lance datasets with pylance/DuckDB, or vote for the native writer).
- Declared property types must match the dataset exactly (no silent coercion;
  endpoint offset columns must be `INT64`). Whatever core's Arrow converter
  rejects (float16, time32/64, maps, ...) fails fast at `CREATE`; timezone-aware
  timestamps and durations are rejected in the FFI for the same reason.
- Object-store datasets skip validation at `CREATE` (credentials may be
  configured later) and fail at first scan, mirroring icebug-disk.
- Semi-mask morsel skipping is not implemented (masks still filter); GDS
  vertex scans (`scanVertices`) over Lance tables are not supported.
- Scan batches are sized for the default engine vector capacity (2048 rows);
  custom `LBUG_VECTOR_CAPACITY_LOG2` builds below 11 are not supported.
- `LOAD` the extension before creating Lance tables; the "load the extension"
  error otherwise points at the fix.
