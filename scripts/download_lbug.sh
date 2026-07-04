#!/bin/sh
# Download prebuilt liblbug (shared library + headers) into a local cache,
# plus the CLI binary for running extension tests.
#
# Usage:
#   scripts/download_lbug.sh            # default: shared lib + headers
#   LBUG_LIB_KIND=static ./download_lbug.sh   # static lib instead
#
# Environment variables respected:
#   LBUG_LIB_KIND         shared (default) or static
#   LBUG_PRECOMPILED_RUN_ID  Pin to a specific workflow run
#   LBUG_VERSION          Pin to a specific release version
#   LBUG_TARGET_DIR       Output directory (default: .cache/lbug-prebuilt)
#   LBUG_GITHUB_REPOSITORY  Org/repo (default: LadybugDB/ladybug)

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ENV_FILE="${1:-$PROJECT_DIR/.cache/lbug-prebuilt.env}"
CACHE_LIB_DIR="${LBUG_TARGET_DIR:-$PROJECT_DIR/.cache/lbug-prebuilt/lib}"
LIB_KIND="${LBUG_LIB_KIND:-shared}"
UPSTREAM_SCRIPT="$SCRIPT_DIR/download-liblbug.sh"
UPSTREAM_URL="https://raw.githubusercontent.com/LadybugDB/ladybug/refs/heads/main/scripts/download-liblbug.sh"

if [ ! -f "$UPSTREAM_SCRIPT" ]; then
  echo "Fetching $UPSTREAM_URL ..."
  curl -fsSL "$UPSTREAM_URL" -o "$UPSTREAM_SCRIPT"
  chmod +x "$UPSTREAM_SCRIPT"
fi

LBUG_TARGET_DIR="$CACHE_LIB_DIR" LBUG_LIB_KIND="$LIB_KIND" bash "$UPSTREAM_SCRIPT"

OS="$(uname -s)"
case "$OS" in
  Darwin)
    if [ "$LIB_KIND" = "shared" ]; then
      LIB_PATH="$CACHE_LIB_DIR/liblbug.dylib"
    else
      LIB_PATH="$CACHE_LIB_DIR/liblbug.a"
    fi
    ;;
  Linux)
    if [ "$LIB_KIND" = "shared" ]; then
      LIB_PATH="$CACHE_LIB_DIR/liblbug.so"
    else
      LIB_PATH="$CACHE_LIB_DIR/liblbug.a"
    fi
    ;;
  MINGW*|MSYS*|CYGWIN*)
    if [ "$LIB_KIND" = "shared" ]; then
      LIB_PATH="$CACHE_LIB_DIR/lbug_shared.dll"
    else
      LIB_PATH="$CACHE_LIB_DIR/lbug.lib"
    fi
    ;;
  *)
    echo "Unsupported OS: $OS" >&2
    exit 1
    ;;
esac

if [ ! -f "$LIB_PATH" ]; then
  echo "Expected precompiled library not found at $LIB_PATH" >&2
  exit 1
fi

mkdir -p "$(dirname "$ENV_FILE")"
cat > "$ENV_FILE" <<EOF
LBUG_LIBRARY_DIR=$CACHE_LIB_DIR
LBUG_INCLUDE_DIR=$CACHE_LIB_DIR
EOF

echo "Wrote $ENV_FILE"
echo "Resolved precompiled library: $LIB_PATH"
