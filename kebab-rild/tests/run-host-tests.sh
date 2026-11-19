#!/bin/sh
# run-host-tests.sh — build (incremental, parallel) and run the host tests.
# Usage: tests/run-host-tests.sh [--no-asan]
#   default: ASAN build (recommended before a flash)
#   --no-asan: plain build for fast local loops
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
RADIO=$(cd "$HERE/../.." && pwd)
LOS=$(cd "$HERE/../../../../../.." && pwd)

SAN=1
for a in "$@"; do
    case "$a" in
    --no-asan) SAN=0 ;;
    --asan) SAN=1 ;;
    -h | --help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done

JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
make -C "$HERE" -j"$JOBS" SANITIZE=$SAN CLANG="${CC:-clang}" RADIO="$RADIO" LOS="$LOS" >/dev/null

if [ "$SAN" = 1 ]; then
    BIN="$HERE/build/asan/kebab_mock"
else
    BIN="$HERE/build/normal/kebab_mock"
fi

echo "== smoke test"
timeout 60 "$BIN" --smoke

if [ "$SAN" = 1 ]; then
    echo "== mock QMI test (ASAN)"
    ASAN_OPTIONS=detect_leaks=0 timeout 120 "$BIN"
else
    echo "== mock QMI test"
    timeout 120 "$BIN"
fi
