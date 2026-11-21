#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-quick}"
BUILD="${2:-build}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ "$MODE" != "quick" && "$MODE" != "full" ]]; then
  echo "usage: $0 [quick|full] [build-dir]" >&2
  exit 1
fi

if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"; else JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"; fi

echo "== config + build"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --config Release -j "$JOBS"

BIN="$BUILD"
[[ -x "$BUILD/Release/crdt_bench" ]] && BIN="$BUILD/Release"   # multi-config generators (for macos)

OUT="results/$MODE"
mkdir -p "$OUT"

echo "== env"
"$BIN/crdt_bench" env | tee "$OUT/environment.txt"

echo "== correctness tests"
"$BIN/crdt_tests" --json "$OUT/tests_summary.json" | tee "$OUT/tests.log"

echo "== experiments ($MODE)"
for cfg in micro merge memory workloads network sync_intervals scale_replicas scale_ops intensity; do
  "$BIN/crdt_bench" run "configs/$MODE/$cfg.ini" --out "$OUT"
done

echo "== analysis (graphs + tables)"
PY="$(command -v python3 || command -v python)"
"$PY" scripts/analyze.py "$OUT"

echo "== done: results in $OUT, figures in $OUT/figures, tables in $OUT/tables.md"
