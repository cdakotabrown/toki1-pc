#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# fetch.sh -- fetch the upstream SPU core the PC port links, verify it, and
# apply this project's patches.  pc/3rd/SPU-CORE.md is the full procedure;
# this script is that procedure, and reads the source URL and the pinned
# commit from that document (its SPU_CORE_* lines) so they are written once.
#
#   pc/3rd/spu-glue/fetch.sh                  # download into pc/3rd/spu-core/
#   pc/3rd/spu-glue/fetch.sh --from-clone DIR # copy from a local clone instead
#   SPU_CORE_DIR=/elsewhere pc/3rd/spu-glue/fetch.sh
#
# Result (gitignored):
#   $SPU_CORE_DIR/pristine/<file>   the six upstream files, byte for byte
#   $SPU_CORE_DIR/upstream/<file>   the same with patches/*.patch applied
#   $SPU_CORE_DIR/.fetched          stamp: verified and patched
# Needs: sh, curl (or wget), sha256sum (or shasum -a 256), patch.
set -eu

GLUE=$(cd "$(dirname "$0")" && pwd)
DOC="$GLUE/../SPU-CORE.md"
CORE=${SPU_CORE_DIR:-"$GLUE/../spu-core"}
CLONE=
if [ "${1:-}" = "--from-clone" ]; then
    CLONE=${2:?--from-clone needs a directory}
fi

value() {  # value NAME: the NAME=... line of SPU-CORE.md
    v=$(sed -n "s/^$1=\([^ ]*\).*/\1/p" "$DOC" | head -n 1)
    [ -n "$v" ] || { echo "fetch.sh: no $1= line in $DOC" >&2; exit 1; }
    printf '%s\n' "$v"
}
RAW=$(value SPU_CORE_RAW)
COMMIT=$(value SPU_CORE_COMMIT)

# upstream path -> local name (two files are both called types.h upstream)
FILES="src/core/spu.cpp:spu.cpp
src/core/spu.h:spu.h
src/common/types.h:types.h
src/core/types.h:core_types.h
src/common/bitfield.h:bitfield.h
src/common/bitutils.h:bitutils.h"

if command -v sha256sum >/dev/null 2>&1; then SHA="sha256sum"; else SHA="shasum -a 256"; fi

mkdir -p "$CORE"
CORE=$(cd "$CORE" && pwd)
rm -rf "$CORE/pristine.new" "$CORE/stage" "$CORE/.fetched"
mkdir "$CORE/pristine.new"
echo "SPU core: commit $COMMIT -> $CORE"
for pair in $FILES; do
    src=${pair%%:*}; dst=${pair#*:}
    out="$CORE/pristine.new/$dst"
    if [ -n "$CLONE" ]; then
        git -C "$CLONE" show "$COMMIT:$src" > "$out"
    elif command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$out" "$RAW/$COMMIT/$src"
    else
        wget -q -O "$out" "$RAW/$COMMIT/$src"
    fi
    echo "  $src -> pristine/$dst"
done

(cd "$CORE/pristine.new" && $SHA -c "$GLUE/SHA256SUMS") || {
    echo "fetch.sh: checksum mismatch: not the pinned upstream files (see pc/3rd/SPU-CORE.md)" >&2
    exit 1
}

# the patches address upstream/<file> (patch -p1 from the core directory)
rm -rf "$CORE/stage"
mkdir "$CORE/stage"
cp -R "$CORE/pristine.new" "$CORE/stage/upstream"
for p in "$GLUE"/patches/*.patch; do
    echo "  patch $(basename "$p")"
    patch -p1 -s -d "$CORE/stage" -i "$p" || {
        echo "fetch.sh: $(basename "$p") does not apply" >&2; exit 1; }
done
rm -f "$CORE"/stage/upstream/*.orig

rm -rf "$CORE/pristine" "$CORE/upstream" "$CORE/obj" "$CORE/spu_psx.a"
mv "$CORE/pristine.new" "$CORE/pristine"
mv "$CORE/stage/upstream" "$CORE/upstream"
rmdir "$CORE/stage"
date -u +%Y-%m-%dT%H:%M:%SZ > "$CORE/.fetched"
echo "SPU core ready: $CORE (build: make -f pc/Makefile all)"
