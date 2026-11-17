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
    # A test lists the app sources it needs on a "/* deps: a.c b.c */" line.
    deps=$(sed -n 's|^/\* deps: \(.*\) \*/$|\1|p' "$t" | head -n1)
    srcs=""
    for d in $deps; do srcs="$srcs $ROOT/$d"; done
    $CC -std=c99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
        -I"$ROOT/include" "$t" $srcs -o "$OUT/$name" && "$OUT/$name" || fail=1
done

[ "$fail" -eq 0 ] && echo "all host tests passed" || { echo "host tests FAILED"; exit 1; }
