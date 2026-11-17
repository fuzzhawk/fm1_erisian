#!/bin/sh
# Host tests: no JieLi toolchain needed. Runs every tests/test_*.py and
# compiles/runs every tests/test_*.c with the host C compiler.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build-host"
mkdir -p "$OUT"
CC="${CC:-cc}"
fail=0

for t in "$ROOT"/tests/test_*.py; do
    [ -e "$t" ] || continue
    echo "== $(basename "$t")"
    python3 "$t" || fail=1
done

for t in "$ROOT"/tests/test_*.c; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .c)
    echo "== $name"
    $CC -std=c99 -Wall -Wextra -Werror -O1 -g -I"$ROOT/include" \
        "$t" -o "$OUT/$name" && "$OUT/$name" || fail=1
done

[ "$fail" -eq 0 ] && echo "all host tests passed" || { echo "host tests FAILED"; exit 1; }
