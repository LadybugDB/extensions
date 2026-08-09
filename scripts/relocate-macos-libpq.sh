#!/usr/bin/env bash
set -euo pipefail

# Relocates the libpq dependency of a macOS Mach-O binary from the
# package-manager-specific absolute path recorded at build time
# (e.g. /opt/homebrew/opt/libpq/lib/libpq.5.dylib) to @rpath/libpq.5.dylib,
# then adds the common Homebrew locations that provide libpq as LC_RPATH
# fallbacks.
#
# This mirrors scripts/relocate-macos-openssl.sh in the ladybug repo and
# fixes LadybugDB/extensions#48: the pg_client extension is linked against
# the keg-only standalone `libpq` Homebrew formula during CI, but end users
# often have libpq only via `brew install postgresql@18` (which installs it
# to a different location). Without relocation the released binary cannot be
# loaded on such machines.
#
# Note: LC_RPATH entries pointing at directories that do not exist on the
# target machine are silently skipped by dyld, so adding every known
# Homebrew layout is safe. When a new PostgreSQL major version becomes a
# common Homebrew formula, append its locations to the list below.

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <mach-o-binary>" >&2
    exit 2
fi

binary="$1"
if [ ! -f "$binary" ]; then
    echo "Mach-O binary not found: $binary" >&2
    exit 1
fi

LIBPQ="libpq.5.dylib"

dependency_for() {
    local library="$1"
    otool -L "$binary" | awk -v library="$library" \
        'index($1, library) && substr($1, length($1) - length(library) + 1) == library { print $1; exit }'
}

dependency="$(dependency_for "$LIBPQ")"
if [ -z "$dependency" ]; then
    echo "libpq dependency not found in $binary" >&2
    exit 1
fi
if [ "$dependency" != "@rpath/$LIBPQ" ]; then
    install_name_tool -change "$dependency" "@rpath/$LIBPQ" "$binary"
fi

for rpath in \
    /opt/homebrew/opt/libpq/lib \
    /usr/local/opt/libpq/lib \
    /opt/homebrew/lib/postgresql@18 \
    /usr/local/lib/postgresql@18 \
    /opt/homebrew/opt/postgresql@18/lib \
    /usr/local/opt/postgresql@18/lib; do
    existing_rpaths="$(otool -l "$binary" | awk '/cmd LC_RPATH/{getline; getline; print $2}')"
    if ! grep -Fxq "$rpath" <<<"$existing_rpaths"; then
        install_name_tool -add_rpath "$rpath" "$binary"
    fi
done

# install_name_tool invalidates the existing signature.
# Ad-hoc signing ("-") is sufficient for development and CI. If this binary is
# distributed to end-users (e.g., via npm), a proper Developer ID certificate
# should be used instead for notarization compatibility.
codesign --force --sign - "$binary"