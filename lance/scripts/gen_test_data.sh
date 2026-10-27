#!/bin/bash
# Regenerates the Lance test datasets under extension/lance/test/lance_data/.
# Uses the extension's own Rust fixture generator (same Lance version as the
# reader), so files are always in a readable format. Content mirrors
# dataset/ice-disk-test (the icebug-disk fixtures) row-for-row, letting the
# e2e expectations mirror test/test_files/ice_disk/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/../test/lance_data"
rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

cargo run --quiet --manifest-path "${SCRIPT_DIR}/../rust/Cargo.toml" \
    --example gen_fixtures -- "${OUT_DIR}"

echo "Wrote Lance fixtures to ${OUT_DIR}:"
ls "${OUT_DIR}"
