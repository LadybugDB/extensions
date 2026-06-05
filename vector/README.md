# Ladybug Vector Extension

This extension is the Ladybug target for Navix vector-index compatibility. Vector-specific behavior
belongs here; core changes should be limited to parser/planner or storage hooks that are useful
beyond vector search.

## API

Use the Ladybug table functions:

```cypher
CALL CREATE_VECTOR_INDEX('embeddings', 'idx', 'vec', metric := 'cosine');
CALL QUERY_VECTOR_INDEX('embeddings', 'idx', [0.1, 0.2, 0.3], 10);
CALL DROP_VECTOR_INDEX('embeddings', 'idx');
```

`ANN_SEARCH` is registered as a compatibility alias for the Ladybug query function signature:

```cypher
CALL ANN_SEARCH('embeddings', 'idx', [0.1, 0.2, 0.3], 10);
```

Native Navix DDL syntax such as `CREATE VECTOR INDEX ON ...` and `UPDATE VECTOR INDEX ON ...` is
not implemented. Supporting that exact syntax is core parser/planner work that should bind to the
extension functions instead of moving vector-index execution into core.

## Navix Parameter Mapping

| Navix parameter | Ladybug parameter | Notes |
|---|---|---|
| `maxNbrsAtUpperLevel` | `mu` | Upper-layer max degree. |
| `maxNbrsAtLowerLevel` | `ml` | Lower-layer max degree. |
| `samplingProbability` | `pu` | Probability of membership in the upper layer. |
| `efConstruction` | `efc` | Construction search width. |
| `efSearch` | `efs` | Query search width. |
| `alpha` | `alpha` | Neighbor pruning expansion factor. |
| `L2` | `metric := 'l2'` | Also supports `l2sq`. |
| `COSINE` | `metric := 'cosine'` | Default metric. |
| `IP` | `metric := 'ip'` | Alias for Ladybug dot-product distance. |
| `bool_enable_brute_force_knn` | `use_knn := true` | Exact brute-force fallback over visible rows. |
| `searchtype` | `search_type` | `searchtype` remains accepted as a legacy alias. |
| `useknn` | `use_knn` | `useknn` remains accepted as a legacy alias. |

## Search Modes

`search_type` accepts:

```text
auto, navix, adaptive_l, adaptive_g, blind, directed, one_hop, naive, random
```

`auto` is the default. Unfiltered queries use regular HNSW search; filtered queries automatically
use the NaviX adaptive-local heuristic. `navix` and `adaptive_l` explicitly select the same mode,
while `adaptive_g` uses global selectivity thresholds to choose between blind, directed, and one-hop
filtered search. `naive` and `use_knn := true` run exact brute-force search. The other named modes
route filtered search through the extension HNSW implementation. `random` exposes Navix random-mode
behavior using Ladybug graph scans; the old Navix random-fast storage scan is intentionally not
ported without benchmark evidence.

## HNSW Updates And Maintenance

Updates to an indexed vector property are maintained as delete plus insert semantics: the table MVCC
state makes the old vector version invisible, and the new vector is inserted into the existing HNSW
graph. Searches skip invisible and deleted table rows, but graph edges that point at old or deleted
versions can accumulate after many updates or deletes.

For write-heavy workloads, periodically rebuild the vector index to compact away dead HNSW edges:

```cypher
CALL DROP_VECTOR_INDEX('embeddings', 'idx');
CALL CREATE_VECTOR_INDEX('embeddings', 'idx', 'vec', metric := 'cosine');
```

## Deferred Navix Features

The following Navix features are benchmark or research slices, not required for functional parity:

- Scalar quantization.
- Parallel vector search.
- ACORN-specific search behavior.
- Buffer-manager prefetch/pass-through changes.
- Fast neighbor/random-scan storage APIs.
- FVec/FBin benchmark dataset readers.
- Vector-search profiling counters.
