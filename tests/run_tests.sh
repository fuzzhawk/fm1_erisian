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

# The uploader page's protocol code generates frames for the C tests.
if command -v node >/dev/null 2>&1; then
    echo "== js/gen_frames.mjs"
    node "$ROOT/tests/js/gen_frames.mjs" "$OUT/js_frames.bin" || fail=1
else
    echo "(node not found: JS uploader frames not tested)"
fi

for t in "$ROOT"/tests/test_*.c; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .c)
    echo "== $name"
    # A test lists the app sources it needs on a "/* deps: a.c b.c */" line.
    deps=$(sed -n 's|^/\* deps: \(.*\) \*/$|\1|p' "$t" | head -n1)
    srcs=""
    for d in $deps; do srcs="$srcs $ROOT/$d"; done
    $CC -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -DFM1_HOST_TEST \
        -I"$ROOT/include" -I"$ROOT/shared" -I"$ROOT/mavis" "$t" $srcs -lm -o "$OUT/$name" && \
        (cd "$ROOT" && "$OUT/$name" "$OUT/js_frames.bin") || fail=1
done

[ "$fail" -eq 0 ] && echo "all host tests passed" || { echo "host tests FAILED"; exit 1; }
