#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PC-build-only exact edits of matched sources: the arguments the console
passes in registers, and PS1-width record members.

The defect classes the matched C reproduces on the console but not on
x86-64 LP64, each with its plan:

  pc/tools/call_args_plan.json     IDO passes the first four arguments in
      $a0-$a3 and a register keeps its value until something writes it, so
      matched C can call a function with fewer arguments than the callee
      reads and still reproduce the console's bytes (`AddPrim(ot)` with the
      packet already in $a1).  On x86-64 the missing argument is whatever
      the register last held.  pc/tools/audit_ps1_call_args.py finds these
      calls (SHORT-READ); each entry writes out the argument the console's
      listing passes.

  pc/tools/record_width_plan.json  a record that lives in PS1 memory (the
      libgs ordering-table / coordinate / matrix-stack records, the GTE
      divide work area in the scratchpad) and declares a member as a
      native pointer: 4 bytes on the console, 8 (8-aligned) on the host,
      so the member and everything after it move.
      pc/tools/audit_ps1_pointer_casts.py finds them (RECORD-PTR); each
      entry retypes the member to a 4-byte cell (`u32`) and reads it back
      with `(T *)(unsigned long)` (zero-extended, as the console's address
      is 32 bits).  The same plan carries the PS1 pointer tables walked
      through a host `T **` (audit_ps1_pointer_cells.py PS1-CELL /
      NARROW-CELL: the variable becomes a `u32 *` cell pointer) and frame
      buffers the console sizes larger than the C declares
      (msg_str_disp, ContDataEntry's 8-byte program record).

  pc/tools/varargs_plan.json       a K&R variadic function whose body
      reads its arguments as consecutive words above its last named
      parameter (`ap = &va_alist`), contiguous on the console only; the
      edit gives it an ANSI `...` prototype and collects the arguments
      through stdarg (pc/src/ps1_varargs.c).

  pc/tools/return_plan.json        a void function whose every path ends
      in a call leaves that callee's $v0, and a caller declaring it as
      returning a value reads it on the console (GsSortObject4's packet
      cursor from TmdTG3); on x86-64 %rax after a void function is
      whatever it last held.  audit_ps1_call_args.py reports VOID-READ;
      each entry returns the callee's value.

  pc/tools/stack_width_plan.json  a call that passes an argument from the
      fifth on as a 4-byte integer where the definition reads an 8-byte
      pointer: on the stack (Windows from the fifth argument, Linux from
      the seventh) the callee reads the slot's stale upper half.
      audit_ps1_call_args.py --width reports WIN64-STACK / STACK-WIDTH;
      each entry passes the zero-extended 64-bit address.

A plan:

    {"functions": {
        "<OV>/<func>":  {"why": "...", "edits": [[old, new(, count)], ...]},
        "main/<func>":  {"source": "src/main/.../<file>.c", "why": "...",
                         "edits": [...]}}}

Each edit is an exact text replacement in the matched source; `old` must
occur `count` times (default 1) or the entry is stale and the build stops
(the matched file changed: re-read the listing).  A replacement keeps the
line count, so diagnostics and `#line` stay on the matched file's lines.
`why` names the evidence.  Entries of both plans for one key apply in plan
order.

Users: generate_overlay_modules.py applies the overlay entries to the
matched files it reads (`apply_edits`); pc/Makefile compiles each `main/`
entry's source through this file as a filter (`--sources` lists them),
before adapt_ps1_cells.py / adapt_ps1_addresses.py.  The matched sources
are not edited.

usage: ps1_source_edits.py --sources
       ps1_source_edits.py SRC OUT      (filter: SRC must be a plan `source`)
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import spell  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PLANS = (os.path.join(ROOT, 'pc/tools/call_args_plan.json'),
         os.path.join(ROOT, 'pc/tools/record_width_plan.json'),
         os.path.join(ROOT, 'pc/tools/varargs_plan.json'),
         os.path.join(ROOT, 'pc/tools/function_cells_plan.json'),
         os.path.join(ROOT, 'pc/tools/return_plan.json'),
         os.path.join(ROOT, 'pc/tools/stack_width_plan.json'))
_cache = {}


def load_plan(path):
    if path not in _cache:
        with open(path) as fh:
            _cache[path] = json.load(fh)
    return _cache[path]


def load_plans(paths=PLANS):
    return [(os.path.basename(p), load_plan(p)) for p in paths if os.path.exists(p)]


def apply_entry(text, entry):
    """`text` with the entry's edits applied; ValueError when one no longer
    fits (wrong occurrence count, or a replacement that changes the number
    of lines)."""
    for edit in entry['edits']:
        old, new = edit[0], edit[1]
        want = edit[2] if len(edit) > 2 else 1
        n = text.count(old)
        if n != want:
            raise ValueError('%r occurs %d times, the plan expects %d' % (old, n, want))
        if old.count('\n') != new.count('\n'):
            raise ValueError('%r -> %r changes the line count' % (old, new))
        text = text.replace(old, new)
    return text


def apply_edits(key, text, plans=None):
    """`text` with every plan's edits for `key` (OV/func or main/name)
    applied, or unchanged when no plan has the key.  An entry that no
    longer fits the source raises."""
    for name, plan in (plans if plans is not None else load_plans()):
        entry = plan.get('functions', {}).get(key)
        if entry is None:
            continue
        try:
            text = apply_entry(text, entry)
        except ValueError as ex:
            raise ValueError('%s: stale pc/tools/%s entry (%s)' % (key, name, ex))
    return text


def plan_keys(overlay, plans=None):
    """Every plan's keys for one overlay (`OV/func`)."""
    keys = set()
    for _name, plan in (plans if plans is not None else load_plans()):
        keys |= {k for k in plan.get('functions', {}) if k.split('/', 1)[0] == overlay}
    return sorted(keys)


def main_entries(plans=None):
    """{source path: [key, ...]} of the `main/` entries.  The path is the
    one pc/Makefile compiles (pc_paths.spell: a decomp file in its
    toki1-decomp checkout)."""
    out = {}
    for _name, plan in (plans if plans is not None else load_plans()):
        for k, e in plan.get('functions', {}).items():
            if k.startswith('main/'):
                keys = out.setdefault(os.path.normpath(spell(e['source'])), [])
                if k not in keys:
                    keys.append(k)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--sources', action='store_true',
                    help='print the main-binary sources the plans cover')
    ap.add_argument('src', nargs='?')
    ap.add_argument('out', nargs='?')
    a = ap.parse_args(argv)
    if a.sources:
        print(' '.join(sorted(main_entries())))
        return 0
    if not a.src or not a.out:
        ap.error('SRC and OUT are required')
    keys = main_entries().get(os.path.normpath(a.src))
    if not keys:
        print('ps1_source_edits: %s: no plan entry' % a.src, file=sys.stderr)
        return 1
    out = open(a.src, encoding='utf-8', errors='replace').read()
    try:
        for key in keys:
            out = apply_edits(key, out)
    except ValueError as ex:
        print('ps1_source_edits: %s' % ex, file=sys.stderr)
        return 1
    if not re.match(r'#line 1 "[^"]+"\n', out):
        out = '#line 1 "%s"\n' % a.src + out
    tmp = a.out + '.tmp'
    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(out)
    os.replace(tmp, a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
