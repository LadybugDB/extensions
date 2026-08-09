#!/usr/bin/env bash
set -euo pipefail

# Verifies that a macOS Mach-O binary's libpq dependency has been relocated
# by scripts/relocate-macos-libpq.sh: it must reference @rpath/libpq.5.dylib,
# must not reference any package-manager-specific absolute libpq path, and
# must carry every known Homebrew libpq location as an LC_RPATH entry.

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <mach-o-binary>" >&2
    exit 2
fi

binary="$1"
if [ ! -f "$binary" ]; then
    echo "Mach-O binary not found: $binary" >&2
    exit 1
fi

dependencies="$(otool -L "$binary")"

if ! grep -Fq "@rpath/libpq.5.dylib" <<<"$dependencies"; then
    echo "missing @rpath dependency for libpq in $binary" >&2
    exit 1
fi

# The leading '/' is matched by the '^[[:space:]]+/' portion of the regex, so the
# alternatives within the group omit it: 'opt/homebrew', 'usr/local', 'opt/local'.
if grep -Eq '^[[:space:]]+/(opt/homebrew|usr/local|opt/local)/.*libpq\.5\.dylib' \
    <<<"$dependencies"; then
    echo "package-manager-specific libpq dependency remains in $binary" >&2
    exit 1
fi

rpaths="$(otool -l "$binary" | awk '/cmd LC_RPATH/{getline; getline; print $2}')"
for required in \
    /opt/homebrew/opt/libpq/lib \
    /usr/local/opt/libpq/lib \
    /opt/homebrew/lib/postgresql@18 \
    /usr/local/lib/postgresql@18 \
    /opt/homebrew/opt/postgresql@18/lib \
    /usr/local/opt/postgresql@18/lib; do
    if ! grep -Fxq "$required" <<<"$rpaths"; then
        echo "missing libpq rpath $required in $binary" >&2
        exit 1
    fi
done