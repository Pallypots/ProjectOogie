#!/bin/bash
# First-start setup under Wine: the release zip, no game folder, the disc image
# given by OOGIE_SETUP_FROM (path read from the file named in $1, so paths with
# quotes and brackets need no escaping). Usage: wine_setup_test.sh <file with iso path> <zip> <seconds>
ISO="$(cat "$1")"; ZIP="$2"; SECS="${3:-420}"
df -h "$HOME" | tail -1
"$HOME/wine_test_release.sh" "$ZIP" setup1 "$SECS" setup "$ISO" >/dev/null 2>&1
D="$HOME/oogie_wine/setup1/ProjectOogie"
echo "game files: $(find "$D/game" -type f 2>/dev/null | wc -l)"
du -sh "$D/game" 2>/dev/null
grep -a -m2 "started on its own" "$D/logs/play.log"
echo "crash lines: $(grep -ac CRASH "$D/logs/play.log")"
grep -a -o -m1 'Movie.mv_00.bik' "$D/logs/play.log"
