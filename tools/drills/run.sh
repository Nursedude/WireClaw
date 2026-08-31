#!/usr/bin/env bash
# Host-side drills for the +dudeclaw.20 fixes (F1 truncation witness, F2 hop
# arithmetic). Neither needs hardware, so they run anywhere with g++.
#
# WHY these exist: F1's witness and F2's guard are both code that is supposed
# to FIRE on inputs the fleet does not produce today — a 12-id watch list, an
# originator predating Meshtastic 2.3. A guard that has never refused anything
# is not evidence it refuses correctly, and neither condition can be staged on
# the bench. So they are drilled here, against the REAL function.
#
# F1's body is EXTRACTED FROM src/lora_ears.cpp at run time (by marker, not by
# line number) so this drill can never quietly test a stale copy: if the
# function is edited, the drill compiles the edit.
#
# Not a substitute for the bench drills: F1 still wants a real 12-id list on a
# claw, and F3 (wifi_pass refusal) is hardware-only. See FORK.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=src/lora_ears.cpp
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# Extract WATCH_CUT_RESERVE .. end of appendWatch by MARKER.
awk '
  /^static const int WATCH_CUT_RESERVE/ { on = 1 }
  on { print }
  on && /^static void appendWatch\(char \*out, int out_len\) \{/ { infn = 1 }
  infn && /^\}$/ { exit }
' "$SRC" > "$OUT/body.cpp"

lines=$(wc -l < "$OUT/body.cpp")
if [ "$lines" -lt 40 ]; then
    echo "FAIL: extracted only $lines lines from $SRC — the markers moved."
    echo "      Fix the awk above; a silently-empty extract would drill nothing."
    exit 1
fi
grep -q 'appendWatchCut' "$OUT/body.cpp" || { echo "FAIL: extract lacks appendWatchCut"; exit 1; }
echo "extracted $lines lines of appendWatch from $SRC"

CXXFLAGS="-O1 -Wall -Wextra -fsanitize=address,undefined"
cat tools/drills/f1_appendwatch_head.cpp "$OUT/body.cpp" \
    tools/drills/f1_appendwatch_tail.cpp > "$OUT/f1.cpp"

rc=0
echo "=== F1 — truncation witness ==="
# shellcheck disable=SC2086
g++ $CXXFLAGS -o "$OUT/f1" "$OUT/f1.cpp"
"$OUT/f1" || rc=1
echo
echo "=== F2 — hop arithmetic ==="
# shellcheck disable=SC2086
g++ $CXXFLAGS -o "$OUT/f2" tools/drills/f2_hop_arithmetic.cpp
"$OUT/f2" || rc=1
echo
[ "$rc" -eq 0 ] && echo "ALL DRILLS PASSED" || echo "DRILLS FAILED"
exit "$rc"
