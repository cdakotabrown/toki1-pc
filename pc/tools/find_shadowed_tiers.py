#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""find_shadowed_tiers.py -- address-named lower-tier files that shadow
matched C under a landed name.

The naming passes renamed functions in config/symbol_names.json and in the
matched tree, but a src/main/semi-matched/func_XXXXXXXX.c (or archive /
pc-port file) with the old address name still compiles into the PC build and
defines `func_XXXXXXXX` -- and every caller that still spells the address
reaches that copy instead of the matched function.  This lists the linked
ones (pc/build/obj/.srcmap) whose address has a landed name with
src/main/matched/<name>.c, for pc/Makefile's SEMI_SHADOWED.

    python3 pc/tools/find_shadowed_tiers.py
"""
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import decomp_key, spell  # noqa: E402


def main():
    table = json.load(open(os.path.join(ROOT, spell('config/symbol_names.json'))))['symbols']
    srcmap = os.path.join(ROOT, 'pc/build/obj/.srcmap')
    linked = set(decomp_key(l.split()[1]) for l in open(srcmap)) if os.path.exists(srcmap) else None
    for tier in ('src/main/semi-matched', 'src/main/archive', 'src/main/pc-port'):
        for f in sorted(os.listdir(os.path.join(ROOT, spell(tier)))):
            m = re.fullmatch(r'func_([0-9A-Fa-f]{8})(_painted)?\.c', f)
            if not m:
                continue
            path = f'{tier}/{f}'
            if linked is not None and path not in linked:
                continue
            v = table.get('0x' + m.group(1).upper())
            if v and v.get('landed') and os.path.exists(os.path.join(ROOT, spell('src/main/matched'), v['name'] + '.c')):
                print(f'{path} -> {v["name"]}')


if __name__ == '__main__':
    main()
