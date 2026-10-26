#!/usr/bin/env bash
# Headless smoke run of the Windows exe under Wine + Xvfb (cloud dev box only).
# Usage: scripts/run_wine.sh [seconds] [logname]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SECS="${1:-90}"; LOG="$ROOT/logs/${2:-run}.log"; shift $(( $# < 2 ? $# : 2 ))
export HOME=/root WINEDEBUG=-all WINEPREFIX=/root/.wine DISPLAY=:99
pgrep -x Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 &); sleep 1
mkdir -p "$ROOT/run/game"; cp -f "$ROOT/game/default.xbe" "$ROOT/run/game/" 2>/dev/null || true
cp -f "$ROOT/port/build-win/oogie_recomp.exe" "$ROOT/run/"
cd "$ROOT/run" && timeout "$SECS" wine oogie_recomp.exe "$@" > "$LOG" 2>&1; echo "exit=$?"
echo "unresolved icall targets:"; grep -o 'Failed to resolve VA 0x[0-9A-F]*' "$LOG" | awk '{print $5}' | sort -u
wineserver -k 2>/dev/null || true
