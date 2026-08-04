# LadybugDB Timeseries Extension

Time-series analysis functions for sequential data: embedding similarity comparison and drift detection on embedding sequences.

## Functions

### EMBEDDING_SIMILARITY

Compute cosine similarity and per-dimension feature similarity between two embedding vectors of any dimension.

```cypher
CALL embedding_similarity(
    CAST([0.1, 0.2, 0.3], 'DOUBLE[]'),
    CAST([0.1, 0.2, 0.4], 'DOUBLE[]')
) RETURN cosine_similarity, feature_similarity, dimension_count;
```

**Parameters:**
- `vec_a LIST<DOUBLE>` — first embedding vector
- `vec_b LIST<DOUBLE>` — second embedding vector

**Returns:**
- `cosine_similarity DOUBLE` — cosine similarity [-1, 1]
- `feature_similarity DOUBLE` — mean per-dimension feature similarity [0, 1]
- `dimension_count INT64` — number of dimensions compared

### DETECT_DRIFT_POINTS

Detect drift points in a sequence of embeddings by computing pairwise cosine distances between consecutive vectors. Returns drift points sorted by significance.

```cypher
CALL detect_drift_points(
    CAST([1.0, 0.0, 0.0, 0.0, 1.0, 0.0], 'DOUBLE[]'),
    2,                    -- num_embeddings
    CAST([100, 200], 'INT64[]'),  -- labels
    3,                    -- embedding_dim
    threshold := 0.1
) RETURN label, drift_magnitude, significance, direction;
```

**Parameters:**
- `flat_embeddings LIST<DOUBLE>` — N×D doubles (row-major: e1_1, e1_2, ..., eN_D)
- `num_embeddings INT64` — N
- `labels LIST<INT64>` — N labels (commit hashes, timestamps, version numbers, etc.)
- `embedding_dim INT64` — D (e.g. 768)
- `threshold:=0.3` — optional DOUBLE detection threshold (default 0.3)

**Returns:**
- `label INT64` — label of the drift point
- `drift_magnitude DOUBLE` — raw cosine distance
- `significance DOUBLE` — normalized significance [0, 1]
- `direction STRING` — "up" or "down" relative to previous distance

## Use Cases

- **Architecture drift**: embeddings of code snapshots per commit → detect when architecture changed significantly
- **Content drift**: embeddings of document revisions over time → detect when content drifted
- **Behavior drift**: embeddings of API response patterns → detect behavioral changes
- **Vector comparison**: compare any two embeddings regardless of dimension

## Building

```bash
cmake -DBUILD_EXTENSIONS="timeseries" ..
cmake --build . --target libtimeseries.lbug_extension
```

## Dependencies

No external dependencies. Pure C++ implementation.
