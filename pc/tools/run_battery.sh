#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# run_battery.sh -- the PC-port verification battery (docs/progress/REMAINING-WORK.md),
# with the independent stages run in parallel.
#
#   pc/tools/run_battery.sh [OUT_DIR]        (default build/battery)
#
# Run it from a checkout that is NOT the player's (the main checkout's
# pc/build holds prepared game data and saves): e.g. a detached worktree.
# Stage 1 (serial): regenerate the overlay modules, rebuild the English
# translation pack (when the English patch is present: toki1-english beside
# this checkout or TOKI_ENGLISH_DIR, and its patch build staged), build,
# native tests.  Without the English patch the build is the Japanese game
# and the English-only tests skip (each one says so in the unittest log).
# Stage 2 (parallel): the pc/tools unit tests, the seven audits, the scene
# matrix and the three playthroughs.  Every stage writes OUT/<stage>.log and
# one "<stage> <exit>" line to OUT/done; the script exits non-zero if any
# stage failed and prints a one-screen summary.
set -u
OUT=${1:-build/battery}
PY=${PY:-$(pwd)/.venv/bin/python3}
rm -rf "$OUT"; mkdir -p "$OUT"
stage() {  # stage NAME CMD...
    local name=$1; shift
    "$@" > "$OUT/$name.log" 2>&1
    echo "$name $?" >> "$OUT/done"
}
export PATH="$(dirname "$PY"):$PATH"
# the three playthroughs run at once: give each a third of the cores for its shot re-runs
export PLAYTHROUGH_SHOT_JOBS=${PLAYTHROUGH_SHOT_JOBS:-$(( $(nproc) / 3 > 1 ? $(nproc) / 3 : 1 ))}

# The parts (tools/repo_paths.py): toki1-decomp is required, toki1-english not.
EN=$("$PY" tools/repo_paths.py | sed -n 's/^ENGLISH=//p')
DECOMP=$("$PY" tools/repo_paths.py | sed -n 's/^DECOMP=//p')
[ -n "$DECOMP" ] || { echo "toki1-decomp not found (see tools/repo_paths.py)"; exit 1; }
STAGING=${EN:+$EN/build/patch/iso/staging}
stage gen  "$PY" pc/tools/generate_overlay_modules.py
# A stale pack makes the English tests compare against an older patch build:
# rebuild it from the current staging before the build embeds it.
if [ -z "$EN" ]; then
    EN_NOTE="English: toki1-english not found -- Japanese-only build; English-only tests skip"
elif [ -d "$STAGING" ] && [ -d pc/assets/raw ]; then
    stage pack "$PY" pc/tools/translation_pack.py build --quiet
    EN_NOTE="English: pack rebuilt from $STAGING"
else
    EN_NOTE="English: NO patch staging at $STAGING (build it in toki1-english: tools/patch/build_patched.py config/patch/english.json) -- pack NOT rebuilt"
    [ -f pc/build/translation/en.tktp ] && EN_NOTE="$EN_NOTE; the existing pc/build/translation/en.tktp may be stale"
fi
echo "$EN_NOTE"
echo "$EN_NOTE" > "$OUT/english.txt"
stage build make -f pc/Makefile all
stage test make -f pc/Makefile test
if grep -qE '^(gen|pack|build) [^0]' "$OUT/done"; then
    echo "build failed: see $OUT/build.log"; cat "$OUT/done"; exit 1
fi

pids=()
stage unittest "$PY" pc/tools/run_unittests.py --log "$OUT/unittests" & pids+=($!)
for a in audit_ps1_pointer_casts audit_ps1_wide_access audit_ps1_pointer_records \
         audit_pc_stubs audit_switch_tables audit_ps1_call_args audit_ps1_pointer_cells; do
    stage "$a" "$PY" "pc/tools/$a.py" & pids+=($!)
done
stage matrix "$PY" pc/tools/scene_matrix.py --frames 300 --jobs "$(nproc)" \
      --out "$OUT/matrix.txt" --logdir "$OUT/matrix-logs" & pids+=($!)
for s in new_game date_save spring_events; do
    stage "$s" "$PY" pc/tools/playthrough.py "pc/tools/playthrough/$s.txt" --out "$OUT/pt-$s" & pids+=($!)
done
wait "${pids[@]}"

echo "== $(git log --oneline -1)"
[ "$DECOMP" = "$(pwd -P)" ] || echo "== toki1-decomp: $DECOMP ($(git -C "$DECOMP" log --oneline -1 2>/dev/null))"
[ -z "$EN" ] || [ "$EN" = "$(pwd -P)" ] || echo "== toki1-english: $EN ($(git -C "$EN" log --oneline -1 2>/dev/null))"
echo "== $EN_NOTE"
sort "$OUT/done" | tr '\n' ' '; echo
grep -h 'summary:' "$OUT/matrix.log" | tail -1
grep -h 'fault folds' "$OUT/matrix.log" | tail -1
grep -E '^(Ran|OK|FAILED|slowest)|: EXCLUDED ' "$OUT/unittest.log"
grep -E '^(----|FAIL:|ERROR:)' "$OUT/unittest.log" | head -20
tail -1 "$OUT/test.log"
grep -h '^PASS\|^FAIL' "$OUT"/new_game.log "$OUT"/date_save.log "$OUT"/spring_events.log
! grep -qE ' [^0]$' "$OUT/done"
