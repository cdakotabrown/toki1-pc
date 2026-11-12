#!/bin/bash
# Run the GitHub Actions workflow (.github/workflows/build.yaml) locally, in a
# throwaway clean checkout of a commit, before pushing it.
#
# A clean checkout is the point: CI clones only tracked files, so an untracked
# header or a gitignored generated file that this checkout happens to have is
# exactly what breaks CI and never breaks here.
#
# usage: tools/ci/run_ci_locally.sh [REV]
#   REV   commit to test (default HEAD; commit first, uncommitted edits are
#         NOT tested)
#   env TOKI_DECOMP_DIR   the toki1-decomp checkout to build against (default:
#         the one tools/repo_paths.py finds, ../toki1-decomp); its HEAD is
#         checked out cleanly beside the throwaway toki1-pc checkout, as CI does
#   env KEEP=1 leaves the checkouts in place (path printed) for debugging
#
# Mirrors the workflow step for step.  If you change build.yaml, change this
# script in the same commit.  On purpose: the host's own Python/venv is used
# (CI pins 3.11).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REV=${1:-HEAD}
SHA=$(git -C "$ROOT" rev-parse --verify "$REV^{commit}") || exit 2
PY=python3
[ -x "$ROOT/.venv/bin/python" ] && PY="$ROOT/.venv/bin/python"
DECOMP=$("$PY" "$ROOT/tools/repo_paths.py" | sed -n 's/^DECOMP=//p')
[ -n "$DECOMP" ] && [ -d "$DECOMP/.git" -o -f "$DECOMP/.git" ] || {
    echo "toki1-decomp checkout not found (../toki1-decomp or TOKI_DECOMP_DIR)"; exit 2; }
DSHA=$(git -C "$DECOMP" rev-parse HEAD) || exit 2
TOP=$(mktemp -d "${TMPDIR:-/tmp}/toki1-pc-ci.XXXXXX")
cleanup() { [ "${KEEP:-0}" = 1 ] && { echo "kept: $TOP"; return; }
            git -C "$ROOT" worktree remove --force "$TOP/toki1-pc" 2>/dev/null
            git -C "$DECOMP" worktree remove --force "$TOP/toki1-decomp" 2>/dev/null
            rm -rf "$TOP"; }
trap cleanup EXIT
git -C "$ROOT" worktree add -q --detach "$TOP/toki1-pc" "$SHA" || exit 2
git -C "$DECOMP" worktree add -q --detach "$TOP/toki1-decomp" "$DSHA" || exit 2
cd "$TOP/toki1-pc" || exit 2
unset TOKI_DECOMP_DIR TOKI_ENGLISH_DIR
echo "== CI on $(git log -1 --oneline) with toki1-decomp $(git -C ../toki1-decomp log -1 --oneline) in $TOP"
FAIL=""
step() { local name=$1; shift; echo "-- $name"; if ! bash -c "$*"; then echo "FAIL: $name"; FAIL="$FAIL $name"; fi; }

step "byte-compile Python" "$PY -m compileall -q -x '(\\.venv|build|__pycache__|\\.git)' tools pc/tools"
step "shell syntax" "find tools pc -name '*.sh' -print0 | xargs -0 -r -n1 bash -n"
step "tracked JSON parses" "$PY - <<'PY'
import json, subprocess
files = subprocess.run(['git', 'ls-files', '*.json'], capture_output=True, text=True, check=True).stdout.split()
bad = []
for p in files:
    try:
        json.load(open(p, encoding='utf-8'))
    except Exception as e:
        bad.append('%s: %s' % (p, e))
print('%d JSON files parsed, %d failed' % (len(files), len(bad)))
for b in bad:
    print('BAD', b)
raise SystemExit(1 if bad else 0)
PY"
step "fetch SPU core" "pc/3rd/spu-glue/fetch.sh"
step "PC build" "PATH=$(dirname "$PY"):\$PATH make -f pc/Makefile all -j$(nproc)"
step "PC unit tests (no disc)" "PC_TESTS_NO_DISC=1 make -f pc/Makefile test"

if [ -n "$FAIL" ]; then echo "== CI WOULD FAIL:$FAIL"; exit 1; fi
echo "== CI would pass"
