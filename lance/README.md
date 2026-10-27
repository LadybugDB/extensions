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

**Lance I/O goes through DuckDB.** The extension embeds an in-memory DuckDB
that loads the `lance` core extension — i.e. the exact engine built by
[lance-duckdb](https://github.com/lance-duckdb) (this extension tracks what
DuckDB `v2.0-cyanoptera` ships, the same line lance-duckdb `main` targets).
Reads are range queries (`SELECT <casts> FROM '<path>.lance' LIMIT/OFFSET`)
which lance-duckdb pushes into the Lance scan, so morsel and CSR range scans
stay efficient. This mirrors how the `delta`/`iceberg` extensions reuse
DuckDB's Delta/Iceberg extensions, and it means no Rust toolchain or Lance
linkage is required to build or run this extension.

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
`src/include/storage/table/external_table_hooks.h`):

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
still resolves hops through CSR range lookups instead of joins. A native
(Rust, zero-DuckDB) reader reusing lance-duckdb's `rust/` crates is the
natural next step; the table layer is already decoupled behind `LanceReader`.

## Regenerating test fixtures

```bash
./extension/lance/scripts/gen_test_data.sh
```

regenerates `extension/lance/test/lance_data/` with the DuckDB CLI. Content
mirrors `dataset/ice-disk-test` row-for-row.

## Limitations (v1)

- Read-only tables; no `COPY ... TO ... (FORMAT lance)` write path yet (write
  Lance datasets with DuckDB/pylance, or vote for the native writer).
- `STRUCT`/`MAP`/`UNION` property columns are rejected at `CREATE` with a
  clear error; scalars, `LIST`/`ARRAY` (incl. fixed-size vectors), temporal,
  `UUID`, `DECIMAL`, and `BLOB` are supported.
- Object-store datasets skip validation at `CREATE` (credentials may be
  configured later) and fail at first scan, mirroring icebug-disk.
- Semi-mask morsel skipping is not implemented (masks still filter); GDS
  vertex scans (`scanVertices`) over Lance tables are not supported.
- `LOAD` the extension before creating Lance tables; the "load the extension"
  error otherwise points at the fix.
