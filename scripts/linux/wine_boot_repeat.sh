#!/bin/bash
# Run the release zip under Wine N times (linked game files), and summarise
# each run's play.log: crashes, frames shown, films reached.
# Usage: wine_boot_repeat.sh <zip> <tag> <runs> <seconds>
ZIP="$1"; TAG="$2"; RUNS="${3:-3}"; SECS="${4:-90}"
for i in $(seq 1 "$RUNS"); do
    "$HOME/wine_test_release.sh" "$ZIP" "${TAG}_$i" "$SECS" linkgame >/dev/null 2>&1
    L="$HOME/oogie_wine/${TAG}_$i/ProjectOogie/logs/play.log"
    crashes=$(grep -ac '\[CRASH\]' "$L" 2>/dev/null)
    stalls=$(grep -ac '^\[STALL\] #' "$L" 2>/dev/null)
    presents=$(grep -aoE '\[PRESENT\] f=[0-9]+' "$L" 2>/dev/null | tail -1)
    films=$(grep -aoE 'Movie\\\\mv_[a-z0-9_]+\.bik' "$L" 2>/dev/null | sort -u | tr '\n' ' ')
    compiler=$(grep -a 'shader compiler:' "$L" 2>/dev/null | head -1 | sed 's/.*compiler: //')
    shfail=$(grep -ac 'shader failed' "$L" 2>/dev/null)
    echo "run $i: crashes=$crashes stalls=$stalls last=$presents shader_failures=$shfail films=$films compiler=$compiler"
done
