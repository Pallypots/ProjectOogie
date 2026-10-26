#!/usr/bin/env bash
# Project Oogie: regenerate C from default.xbe and rebuild the Windows exe.
# Seeds (measured indirect-call targets) live in scripts/icall_seeds.json.
# Usage: scripts/regen.sh   (env XBOXRECOMP=/path/to/xboxrecomp)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
XR="${XBOXRECOMP:-/home/claude/ext/xboxrecomp}"
XBE="$ROOT/game/default.xbe"
O="$ROOT/build_out"
SEEDS="$ROOT/scripts/icall_seeds.json"
cd "$XR"
[ -f "$ROOT/game/default_analysis.json" ] || python3 -m tools.xbe_parser "$XBE" --json "$ROOT/game/default_analysis.json"
SEEDARG=(); [ -s "$SEEDS" ] && SEEDARG=(--seed-functions "$SEEDS")
[ -n "${SKIP_DISASM:-}" ] || python3 -m tools.disasm "$XBE" --analysis-json "$ROOT/game/default_analysis.json" -o "$O/disasm" --force "${SEEDARG[@]}" > "$ROOT/logs/disasm.log" 2>&1
[ -n "${SKIP_DISASM:-}" ] || python3 -m tools.func_id "$XBE" --functions "$O/disasm/functions.json" --strings "$O/disasm/strings.json" --xrefs "$O/disasm/xrefs.json" -o "$O/func_id" > "$ROOT/logs/func_id.log" 2>&1
[ -n "${SKIP_DISASM:-}" ] || python3 -m tools.abi_analysis "$XBE" --disasm-dir "$O/disasm" --func-id-dir "$O/func_id" --output-dir "$O/abi" > "$ROOT/logs/abi.log" 2>&1
rm -f "$ROOT"/port/src/recomp/gen/recomp_*.c
python3 -m tools.recomp "$XBE" --all --split 1000 --game-name OogiesRevenge \
  --disasm-dir "$O/disasm" --func-id-dir "$O/func_id" --abi-dir "$O/abi" \
  --exclude-manual "$ROOT/port/src/recomp_manual.c" \
  --gen-dir "$ROOT/port/src/recomp/gen" -o "$O/recomp" > "$ROOT/logs/recomp.log" 2>&1
tail -25 "$ROOT/logs/recomp.log" | grep -E 'functions|unresolved' || true
cmake --build "$ROOT/port/build-win" -j2 > "$ROOT/logs/build-win.log" 2>&1
ls -la "$ROOT/port/build-win/oogie_recomp.exe"
