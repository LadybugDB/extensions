#!/bin/bash
# Regenerates the Lance test datasets under extension/lance/test/lance_data/.
# Uses the DuckDB CLI with its lance core extension (the same engine the lance
# extension reads through), so the files are always readable by the extension.
# Content mirrors dataset/ice-disk-test (the icebug-disk fixtures) row-for-row,
# letting the e2e expectations mirror test/test_files/ice_disk/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/../test/lance_data"
rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

duckdb -unsigned :memory: <<SQL
INSTALL lance;
LOAD lance;

-- Nodes: same rows as nodes_user.parquet / nodes_city.parquet.
COPY (
    SELECT * FROM (VALUES
        (75, 'Noura', 25),
        (100, 'Adam', 30),
        (250, 'Karissa', 40),
        (300, 'Zhang', 50)
    ) AS t(id, name, age)
) TO '${OUT_DIR}/nodes_user.lance' (FORMAT lance, mode 'overwrite');

COPY (
    SELECT * FROM (VALUES
        (500, 'Guelph', 75000),
        (600, 'Kitchener', 200000),
        (700, 'Waterloo', 150000)
    ) AS t(id, name, population)
) TO '${OUT_DIR}/nodes_city.lance' (FORMAT lance, mode 'overwrite');

-- CSR follows: indices (target offsets, CSR-sorted by source) + indptr.
COPY (
    SELECT * FROM (VALUES
        (1, 2023),
        (1, 2023),
        (2, 2020),
        (3, 2020),
        (1, 2022),
        (3, 2021),
        (0, 2022)
    ) AS t(target, since)
) TO '${OUT_DIR}/indices_follows.lance' (FORMAT lance, mode 'overwrite');

COPY (
    SELECT * FROM (VALUES (0), (1), (4), (6), (7)) AS t(ptr)
) TO '${OUT_DIR}/indptr_follows.lance' (FORMAT lance, mode 'overwrite');

-- CSR livesin: indices have no edge properties.
COPY (
    SELECT * FROM (VALUES (0), (2), (2), (1)) AS t(target)
) TO '${OUT_DIR}/indices_livesin.lance' (FORMAT lance, mode 'overwrite');

COPY (
    SELECT * FROM (VALUES (0), (1), (2), (3), (4)) AS t(ptr)
) TO '${OUT_DIR}/indptr_livesin.lance' (FORMAT lance, mode 'overwrite');

-- FLAT follows: (src, dst, since) row per edge, same 7 edges.
COPY (
    SELECT * FROM (VALUES
        (0, 1, 2023),
        (1, 1, 2023),
        (1, 2, 2020),
        (1, 3, 2020),
        (2, 1, 2022),
        (2, 3, 2021),
        (3, 0, 2022)
    ) AS t(src, dst, since)
) TO '${OUT_DIR}/rels_follows_flat.lance' (FORMAT lance, mode 'overwrite');
SQL

echo "Wrote Lance fixtures to ${OUT_DIR}:"
ls "${OUT_DIR}"
