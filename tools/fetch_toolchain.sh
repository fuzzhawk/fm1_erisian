#!/bin/sh
# Download the JieLi PI32v2 Linux toolchain into ~/.jieli (never into the repo).
# Verifies the archive against tools/toolchain.sha256 once a hash is pinned.
set -eu

URL="${JIELI_TOOLCHAIN_URL:-https://pkgman.jieliapp.com/s/linux-toolchain}"
DEST="${JIELI_HOME:-$HOME/.jieli}"
HERE=$(cd "$(dirname "$0")" && pwd)
PIN_FILE="$HERE/toolchain.sha256"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
ARCHIVE="$TMP/toolchain.tar.xz"

echo "fetching $URL"
curl -fL --retry 3 -o "$ARCHIVE" "$URL"

ACTUAL=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
echo "toolchain archive sha256: $ACTUAL"

EXPECTED=$(grep -Eo '^[0-9a-f]{64}' "$PIN_FILE" 2>/dev/null || true)
if [ -n "$EXPECTED" ]; then
    if [ "$ACTUAL" != "$EXPECTED" ]; then
        echo "error: hash mismatch (expected $EXPECTED)" >&2
        exit 1
    fi
    echo "hash matches pinned value"
else
    echo "warning: no pinned hash yet; commit the value above to tools/toolchain.sha256" >&2
fi

mkdir -p "$DEST"
tar -xJf "$ARCHIVE" -C "$DEST"
echo "extracted into $DEST:"
find "$DEST" -path '*/bin/clang' -type f
