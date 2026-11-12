#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""obj_srcmap.py OBJ_DIR SRC... -- keep pc/Makefile's incremental build honest.

pc/Makefile names objects by source basename, and the source tiers shadow
each other by basename (src/main/pc-port > matched > semi-matched > archive).
When the winning source for a basename changes -- an override deleted, a
matched file landed, an exclusion added -- the existing object is NEWER than
the new winner, so make would keep linking the old file's code.  Stale
objects masked fixes more than once.

This records `<object> <source>` in OBJ_DIR/.srcmap and deletes every object
whose recorded source differs from the current one (or that no source maps
to any more), so the next make rebuilds it.  Called from pc/Makefile at
parse time; prints nothing unless it removes something.
"""
import os
import sys


def main():
    obj_dir, srcs = sys.argv[1], sys.argv[2:]
    os.makedirs(obj_dir, exist_ok=True)
    path = os.path.join(obj_dir, '.srcmap')
    cur = {}
    for s in srcs:
        cur[os.path.splitext(os.path.basename(s))[0] + '.o'] = s
    old = {}
    if os.path.exists(path):
        for line in open(path):
            parts = line.split()
            if len(parts) == 2:
                old[parts[0]] = parts[1]
    removed = []
    for obj, src in old.items():
        if cur.get(obj) != src:
            p = os.path.join(obj_dir, obj)
            if os.path.exists(p):
                os.remove(p)
                removed.append(f'{obj} ({src} -> {cur.get(obj, "gone")})')
    # an object with no record (built before this tool existed) is trusted only
    # if it is not shadowed: record it and let make's timestamp rule decide
    if old:
        for obj in cur:
            if obj not in old:
                p = os.path.join(obj_dir, obj)
                if os.path.exists(p):
                    os.remove(p)
                    removed.append(f'{obj} (new source {cur[obj]})')
    text = ''.join(f'{obj} {cur[obj]}\n' for obj in sorted(cur))
    # rewritten only when the set changes: pc/Makefile relinks on its mtime,
    # so a source that leaves the build (no object newer than the binary)
    # still drops out of the link
    old_text = open(path).read() if os.path.exists(path) else None
    if text != old_text:
        tmp = path + '.tmp'
        with open(tmp, 'w') as fh:
            fh.write(text)
        os.replace(tmp, path)
    if removed:
        sys.stderr.write(f'obj_srcmap: {len(removed)} object(s) changed source, rebuilding: '
                         + ', '.join(removed[:8]) + (' ...' if len(removed) > 8 else '') + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
