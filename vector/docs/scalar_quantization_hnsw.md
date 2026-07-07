# Scalar Quantization For HNSW Indexes

This document describes the scalar quantization design for the vector extension HNSW index. The
implementation supports SQ8 and SQ16 quantized distance evaluation while keeping quantized
embeddings in Ladybug storage instead of a raw sidecar file.

## Goals

- Reduce memory and distance-computation cost for HNSW search by storing quantized vectors.
- Keep quantized embeddings transactionally consistent with the base node table and HNSW graph.
- Support both `cache_embeddings := true` and `cache_embeddings := false`, with the optimized
  query path focused on `cache_embeddings := true`.
- Preserve existing HNSW graph storage: upper and lower graph layers remain internal rel tables.
- Keep full-precision rerank optional through `use_full_precision_rerank := true`.

## Non-Goals

- Quantization is not supported for dot-product metrics. It is available for L2, L2SQ, and cosine.
- The HNSW graph itself is not quantized. Only the embedding representation used for distance
  evaluation is quantized.
- The in-memory quantized cache is not the durable source of truth. It is a query-time accelerator.

## User-Facing Configuration

Scalar quantization is enabled through `CREATE_VECTOR_INDEX`:

```cypher
CALL CREATE_VECTOR_INDEX(
    'embeddings',
    'idx',
    'vec',
    metric := 'l2',
    quantization := 'sq8'
);
```

Supported values are:

- `quantization := 'sq8'`
- `quantization := 'sq16'`

When quantization is enabled, query distance evaluation uses the quantized representation unless
full-precision rerank is enabled. With `use_full_precision_rerank := true`, HNSW candidate
generation uses quantized distances, then final candidate ordering is recomputed against the
original full-precision embedding column.

## Physical Storage

Each quantized HNSW index owns an internal node table in addition to its existing graph tables.

For base table id `T` and index name `idx`, the table name is:

```text
_<T>_<idx>_QEMB
```

The table schema is:

```cypher
CREATE NODE TABLE _<T>_<idx>_QEMB (
    id INT64,
    valid UINT8,
    scale FLOAT,
    norm_sq FLOAT,
    payload INT8[dimension] | INT16[dimension],
    PRIMARY KEY (id)
);
```

Column meaning:

- `id`: source node offset. The quantized table row identity mirrors the base table offset.
- `valid`: `1` when the source embedding is non-null and visible to the writer, `0` for null rows.
- `scale`: per-vector scalar. For L2 and L2SQ this is the reconstruction scale
  `max_abs / max_quantized_value`. For cosine this is the normalized-dot scale
  `1 / sqrt(sum(q_i^2))`.
- `norm_sq`: squared norm of the quantized vector representation. L2 and L2SQ use this in the
  reconstructed distance formula. Cosine keeps it for metadata compatibility but uses the
  normalized-dot scale in the hot path.
- `payload`: SQ8 or SQ16 vector payload.

This table is an internal catalog entry. It is created and dropped with the vector index and is
not part of the public user schema.

## Page Layout

`_QEMB` does not introduce a custom packed disk-page format. It is a normal internal `NodeTable`,
so its columns use the existing node-group, column-chunk, buffer-manager, MVCC, WAL, and checkpoint
machinery. The logical row at offset `N` corresponds to source node offset `N`, but the values are
stored column-wise rather than as one contiguous quantized record:

```text
logical _QEMB row N

id[N] | valid[N] | scale[N] | norm_sq[N] | payload[N][dimension]
  |         |          |           |                 |
  +---------+----------+-----------+-----------------+
       normal NodeTable column chunks and storage pages
```

`TableBackedQuantizedEmbeddings` resolves an offset to its committed or uncommitted node group.
`lookupMultiple` reuses the current scan state while offsets remain in the same source and node
group, and reinitializes it when either changes. The selected `valid`, `scale`, `norm_sq`, and
`payload` values are then read through the normal storage and buffer-manager path. The fixed-size
SQ8/SQ16 `payload` remains an `ARRAY` column; it is not a pointer into a separate `.qemb` file.

When `cache_embeddings := true`, `CachedQuantizedColumn` materializes a separate derived layout for
distance evaluation:

```text
cache slot N

nullMask[N]  scales[N]  normSqs[N]  views[N]
                                         |
                                         v
aligned payload base + N * stride
```

The payload buffer base is 32-byte aligned. For dimension `D`, SQ8 stores `D` payload bytes with
`stride = alignUp(D, 32)`, while SQ16 stores `2 * D` payload bytes with `stride = 2 * D`. Scale and
norm metadata live in separate dense arrays, and each `QuantizedEmbeddingView` combines those
values with the payload address for the same offset. This cache layout is not durable: it is rebuilt
from `_QEMB` after invalidation.

## Catalog And Index Metadata

`HNSWStorageInfo` stores the table ids for all durable index-owned structures:

- upper HNSW graph rel table
- lower HNSW graph rel table
- quantized embeddings node table, when quantization is enabled

The quantized table id is appended to the serialized storage info. Deserialization treats it as
optional so older non-quantized or pre-quantized index metadata remains readable.

## In-Memory Types

The implementation uses three embedding providers:

- `QuantizedInMemEmbeddings`
  Used while building a new in-memory HNSW graph during `CREATE_VECTOR_INDEX` when embeddings are
  cached. It quantizes from the input vectors and avoids storage lookups during the hot build loop.

- `TableBackedQuantizedEmbeddings`
  Reads and writes quantized embeddings through the internal `_QEMB` node table. This is the
  durable, transactional representation used when populating or maintaining quantized rows. It is
  also the on-disk query provider when `cache_embeddings := false`. Query-time reads use storage
  `lookup`/`lookupMultiple`, so hot pages are managed by the normal buffer manager.

- `CachedQuantizedEmbeddings`
  A reader over `CachedQuantizedColumn`, which materializes `_QEMB` rows into an aligned dense
  quantized cache for on-disk queries when `cache_embeddings := true`. This mirrors the baseline
  cached embedding path: storage remains the source of truth, but HNSW neighbor distance
  evaluation reads contiguous in-memory views.

The durable source of truth is `_QEMB`. With `cache_embeddings := false`, query-time reads stay in
the storage layer. With `cache_embeddings := true`, `_QEMB` is used to hydrate a shared dense cache
owned by the on-disk index before serving hot HNSW distance calls.

## Create Index Flow

`CREATE_VECTOR_INDEX` creates all internal storage in one rewrite:

1. Create the upper graph rel table.
2. Create the lower graph rel table.
3. If quantization is enabled, create the `_QEMB` node table.
4. Build the HNSW graph.
5. Register the `OnDiskHNSWIndex` with `HNSWStorageInfo`, including the quantized table id.
6. Rebuild quantized rows by scanning the base embedding column and writing one `_QEMB` row per
   source offset.
7. Force checkpoint so the index-owned tables are persisted together.

Null embeddings are written as `valid = 0` rows. This preserves the invariant that source offset
`N` maps to quantized row `N`, even when the source embedding is null.

## Query Flow

When a query uses a quantized index:

1. The query vector is quantized into the same SQ8 or SQ16 format.
2. `OnDiskHNSWIndex` chooses an embedding provider:
   - `cache_embeddings := true` uses `CachedQuantizedEmbeddings`, backed by a dense
     `CachedQuantizedColumn` populated from `_QEMB`.
   - `cache_embeddings := false` uses `TableBackedQuantizedEmbeddings`.
   - `TableBackedQuantizedEmbeddings::getEmbeddings` batches neighbor vector reads with
     `_QEMB` `lookupMultiple`, matching the baseline storage-backed access pattern more closely
     than one point lookup per neighbor.
3. HNSW graph traversal reads neighbors from the existing graph rel tables.
4. Distance evaluation reads quantized embedding views from the selected provider.
5. If full-precision rerank is enabled, the final candidates are rescored using `OnDiskEmbeddings`
   over the original embedding column.

For quantized on-disk query, `cache_embeddings := true` does not cache full-precision embeddings.
It caches the quantized representation in memory after hydrating it from durable `_QEMB` rows. If
full-precision rerank is enabled, the original embedding column is read separately for reranking.

## Distance Evaluation

L2 and L2SQ use scalar reconstruction from the quantized dot product:

```text
l2sq = scale_l^2 * norm_l + scale_r^2 * norm_r - 2 * scale_l * scale_r * dot(q_l, q_r)
```

`metric := 'l2'` returns `sqrt(max(0, l2sq))`; `metric := 'l2sq'` returns `max(0, l2sq)`.

Cosine uses a MariaDB-style normalized quantized dot product. During quantization for cosine
indexes, `scale` is stored as:

```text
scale = 1 / sqrt(sum(q_i^2))
```

The hot path then evaluates cosine distance as:

```text
distance = 1 - clamp(scale_l * scale_r * dot(q_l, q_r), -1, 1)
```

This avoids computing a denominator from both vector norms for every candidate comparison. Query
vectors are quantized with the same cosine-specific scale semantics as `_QEMB` rows, so cached,
table-backed, and in-memory build paths use the same formula.

## Insert, Update, And Finalize

Committed inserts write both graph state and quantized state in the same transaction:

1. Read the inserted embedding from the insert vector.
2. Insert the vector into the HNSW graph if it is non-null.
3. Write the matching `_QEMB` row through `TableBackedQuantizedEmbeddings`.
4. Write `valid = 0` for null embeddings.
5. Mark the shared derived qemb cache dirty for the writer transaction.

Updates use the existing HNSW update model: the old table row version becomes invisible and the new
vector is inserted as a new version. The quantized table follows the inserted version.

`finalize()` catches up rows that were not part of the checkpointed graph. For quantized indexes it
writes quantized rows only for the offsets it is finalizing. It does not rebuild the full `_QEMB`
table, which avoids duplicate primary-key writes for rows already maintained by insert.

## Checkpoint And Recovery

Checkpoint includes:

- upper graph rel table
- lower graph rel table
- quantized embeddings node table, when present

Because `_QEMB` is a normal internal node table, it uses the same storage, WAL, MVCC, checkpoint,
and recovery paths as other node tables. This is the main reason the design uses an internal table
instead of a raw `.qemb` sidecar file.

After reload, `OnDiskHNSWIndex::load` reconstructs the index from serialized storage info and
loads the quantized table pointer from `quantizedEmbeddingsTableID`.

## Drop Index Flow

`DROP_VECTOR_INDEX` drops all index-owned internal structures:

1. upper graph rel table
2. lower graph rel table
3. `_QEMB` node table, when the index config has quantization enabled
4. index catalog entry

There is no filesystem sidecar cleanup because quantized embeddings are stored through Ladybug
storage.

## Cache And Buffer Semantics

`cache_embeddings := true` does not mean cache full-precision embeddings for quantized indexes.
For on-disk quantized query, it creates a dense qemb cache from `_QEMB` and uses that cache for
HNSW distance evaluation. This is analogous to the baseline cached-embedding path, but the cached
payload is SQ8/SQ16 rather than full-precision floats.

The durable source of truth remains the `_QEMB` table. `TableBackedQuantizedEmbeddings` uses
transactional `NodeTable` lookup APIs, including `lookupMultiple` for batched neighbor reads, when
`cache_embeddings := false`. `CachedQuantizedEmbeddings` still checks source-table visibility while
serving dense cached views when `cache_embeddings := true`.

The persistent scan state retains a `const Transaction*` because quantized lookup only reads
transaction visibility and local-storage metadata. `NodeTable::lookupMultiple` currently accepts a
non-const `Transaction*` as part of its scan-state initialization API, so this call site uses a
contained `const_cast`. This remains safe only while `lookupMultiple` and its callees treat the
transaction as read-only. If that call chain gains transaction mutations, the scan state must
instead retain and pass a non-const transaction rather than casting away constness.

The dense cache is stored in `OnDiskHNSWIndex` as shared cache state. It is protected by a mutex,
has a commit timestamp version, and tracks writer transactions that dirtied `_QEMB`. A query can
reuse the cache only when the cache is not dirty and the query transaction start timestamp is at or
after the cache version. If the cache is absent or too small, the first eligible query rebuilds it
from `_QEMB`.

When a transaction writes `_QEMB`, the index registers commit and rollback callbacks on the
transaction:

- On commit, the cache is reset and the cache version advances to the maximum of its current value
  and the commit timestamp.
- On rollback, the dirty writer is removed without advancing the cache version.
- The cache remains dirty while any tracked writer transaction is still unresolved.

Commit callbacks take the cache-state mutex, so their updates are serialized even when multiple
transactions finish concurrently. The callbacks can still acquire the mutex out of commit timestamp
order; taking the maximum keeps the version watermark monotonic and prevents an older callback from
moving it backward. Tracking every dirty transaction separately keeps the cache unavailable until
the last concurrent writer has committed or rolled back.

This avoids reusing a dense cache across committed qemb changes while allowing rollback-only writes
to clear the dirty state without forcing a new committed version.

This gives the cached quantized path the same hot-query shape as the regular cached baseline while
retaining a smaller quantized representation.

![Quantized HNSW cache read and concurrent writer flow](images/quantized_hnsw_cache_transactions.svg)

[Editable Excalidraw source](images/quantized_hnsw_cache_transactions.excalidraw)

## Benchmark Results

The following run uses `cache_embeddings := true`, `efs = 64`, `k = 10`, 200 speed queries repeated
3 times, and 100 accuracy queries on `openai_small_50k`:


| Variant | Load | Index Build | Ingest | Query Latency | DB Size | Bytes/Vec | Query RSS | Recall@10 | Precision@10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| base | 5.862s | 51.000s | 56.862s | 17.812 ms/query | 606.5 MiB | 12718.7 | 746.6 MiB | 0.9960 | 0.9960 |
| sq8 | 5.765s | 13.111s | 18.875s | 16.366 ms/query | 723.4 MiB | 15170.2 | 327.1 MiB | 0.9460 | 0.9460 |
| sq16 | 5.421s | 23.337s | 28.758s | 16.444 ms/query | 908.0 MiB | 19041.5 | 489.4 MiB | 0.9950 | 0.9950 |

Speedup vs baseline:

| Variant | Load | Index Build | Ingest | Query Latency | Query RSS Reduction |
|---|---:|---:|---:|---:|---:|
| sq8 | 1.02x | 3.89x | 3.01x | 1.09x | 2.28x lower |
| sq16 | 1.08x | 2.19x | 1.98x | 1.08x | 1.53x lower |

Storage overhead vs baseline:

| Variant | DB Size | Bytes/Vec |
|---|---:|---:|
| sq8 | 1.19x | 1.19x |
| sq16 | 1.50x | 1.50x |

## Tradeoffs And Follow-Ups

- `TableBackedQuantizedEmbeddings` uses storage lookups during search. This is correct,
  transactional, page-aware, and buffer-pool-backed, but it still pays lookup/list-decoding overhead
  compared with the dense cache used by `cache_embeddings := true`.
- A dense `CachedQuantizedColumn` avoids storage lookup in the hot path, but it adds cache
  population cost and shared transaction-aware invalidation requirements.
- `norm_sq` is currently stored as `FLOAT`, matching the table schema. SQ16 with very high
  dimensionality may benefit from wider norm storage if precision becomes measurable in recall or
  ranking tests.
- Dead HNSW graph edges can still accumulate under the existing delete/update model. Quantization
  follows existing visibility rules but does not compact the graph.
