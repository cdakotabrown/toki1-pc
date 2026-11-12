#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Catch the PC-port tree up with config/symbol_names.json.

The renames of 2026-10-07..13 landed their renames with
`apply_debug_symbols.py / apply_data_symbols.py --exclude pc/ --exclude
src/main/pc-port/` ("the PC port is out of scope"), so every landed name left
the port behind: HAL definitions and prototypes, the BIOS trampolines,
pc/src/globals.c, the Makefile HAL_EXCLUDES lists, override file names.  The
game C then calls `SetSp` while the port still defines `func_800B56D0`, and
the link fails.

This tool replays every LANDED main-scope rename onto the PC-port tree, and
only there, with the same whole-word, hex-case-insensitive token replacement
the landing tools use.  It additionally:

  * moves src/main/pc-port/func_XXXXXXXX.c to <name>.c, because pc/Makefile
    shadows src/main/matched/<name>.c by BASENAME -- an override left under
    the address name silently stops applying;
  * drops self-trampolines (`void name(void) { __asm__("jmp name"); }` in
    lowercase_aliases.c) and self-aliases that a case-insensitive rename of a
    lowercase/uppercase pair produces -- the same cycle removal the landing
    tool does in revert mode;
  * SKIPS the names that collide with the host libc (memset, strncpy, printf,
    open, read, ...): those stay address-named in pc/ and the game TUs reach
    them through pc/include/game/sony_bios_alias.h, which
    pc/tools/gen_sony_bios_alias.py generates from the same collision rule;
  * leaves GENERATED files alone: pc/src/overlay_*.c and
    pc/include/overlay_entries.h (pc/tools/generate_overlay_modules.py) and
    pc/src/missing_globals_gen.c (pc/tools/gen_missing_globals.py) are
    regenerated instead.

Usage:
    python3 pc/tools/sync_symbol_names.py --dry-run
    python3 pc/tools/sync_symbol_names.py --apply
    python3 pc/tools/sync_symbol_names.py --apply --only SetSp --only 0x800B56E0

Re-run after any rename that excluded pc/.  Idempotent: a tree that is
already in sync changes nothing.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'pc', 'tools'))
from pc_paths import DECOMP, MONOREPO_DECOMP, spell  # noqa: E402
sys.path.insert(1, str(DECOMP / 'tools' / 'analysis'))
import symbol_names as SN                       # noqa: E402
import apply_debug_symbols as ADS               # noqa: E402  (SELF_TRAMPOLINE / SELF_ALIAS)
import gen_sony_bios_alias as ALIAS             # noqa: E402  (the libc collision rule)

# where the PC port lives (prefixes; relative to ROOT)
PC_ROOTS = ('pc/include/', 'pc/hal/', 'pc/src/', 'pc/tools/', 'pc/tests/',
            'pc/Makefile', 'pc/CMakeLists.txt', 'pc/build.sh',
            'src/main/pc-port/')
# generated or vendored: never edited here
SKIP_PREFIXES = ('pc/src/overlay_', 'pc/include/overlay_entries.h', 'pc/tools/',
                 'pc/src/missing_funcs_gen.c', 'pc/src/missing_aliases.ld',
                 'pc/src/missing_globals_gen.c', 'pc/include/game/sony_bios_alias.h',
                 'pc/include/game/ps1_alias_refs.h', 'pc/src/ps1_alias_refs.c',
                 # DEFINES address spellings on purpose: matched TmdG3/G4/TF4 still
                 # call func_800A7218 etc. by address (link fails if renamed)
                 'pc/hal/gte_address_entries.c',
                 'pc/3rd/', 'pc/tools/.venv', 'pc/build/', 'pc/assets/', 'pc/docs/',
                 'pc/screenshots/')
TEXT_EXT = {'.c', '.h', '.cpp', '.py', '.sh', '.txt', '.json', '.mk', ''}
CODE_EXT = {'.c', '.h', '.cpp'}
MOVE_DIR = 'src/main/pc-port'
ARCHIVE_DIR = 'src/main/archive'      # <name>_painted.c; PC-only tier, moved here too


def git(*args):
    return subprocess.run(['git'] + list(args), capture_output=True, text=True, cwd=ROOT).stdout


def pc_files():
    """tracked + untracked-not-ignored text files under the PC-port roots."""
    out = []
    for f in git('ls-files', '--cached', '--others', '--exclude-standard', '--', *PC_ROOTS).splitlines():
        if not f or f.startswith(SKIP_PREFIXES) or f.endswith('.md'):
            continue
        if os.path.splitext(f)[1] not in TEXT_EXT:
            continue
        if os.path.isfile(os.path.join(ROOT, f)):
            out.append(f)
    return out


def rename_map(doc, only):
    """{OLD_ADDRESS_NAME (upper hex): new_name} for every landed main-scope entry
    whose name is not an address name and is not a libc collision.  Also the
    set of names an earlier revert REMOVED from the tree (record `reverted.from`)
    that are not a live name of any entry: a port file still defining one of
    those is reported for review, never rewritten -- the refuted name may since
    have landed on a different function."""
    libc = ALIAS.host_libc_names()
    fwd, skipped, refuted = {}, [], set()
    live = {e['name'] for e in doc['symbols'].values() if e.get('landed')}
    for key, e in doc['symbols'].items():
        if ':' in key or not e.get('landed'):
            continue
        name = e['name']
        if SN.is_address_name(name):
            continue
        addr = int(key, 16)
        prefix = 'func_' if e.get('kind') == 'func' else 'D_'
        old = f'{prefix}{addr:08X}'
        if only and not ({name, old, key, f'{addr:#010x}'} & only):
            continue
        if name in libc:
            skipped.append((old, name))
            continue
        fwd[old.upper()] = name
    for key, e in doc['symbols'].items():
        rv = e.get('reverted')
        if ':' not in key and isinstance(rv, dict) and rv.get('from') and rv['from'] not in live \
                and not SN.is_address_name(rv['from']):
            refuted.add(rv['from'])
    return fwd, skipped, refuted


TOKEN = re.compile(r'\b(func_[0-9A-Fa-f]{8}|D_[0-9A-Fa-f]{8})\b')


COMMENT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
# a one-line forwarder `T NAME(args) { [return] NAME(args); }`: after a rename
# it would call itself
SELF_CALL = re.compile(r'\b(\w+)\s*\([^;{)]*\)\s*\{\s*(?:return\s+)?\1\s*\(')
STEM_DIRS = ('src/main/matched', 'src/main/semi-matched')


def protected_stems():
    """File basenames under src/main/ that stay address-named (not moved by
    this tool); a Makefile list of them is a list of FILES, not symbols."""
    out = set()
    for d in STEM_DIRS:
        dp = os.path.join(ROOT, spell(d))
        if os.path.isdir(dp):
            out.update(os.path.splitext(fn)[0] for fn in os.listdir(dp))
    return out


def apply_to_text(text, fwd, is_code=True, keep=frozenset()):
    """Replace address names outside comments (Makefile: outside `#` lines).
    Comments narrate history (`func_X->name`, `func_X jumps here`) and are
    never rewritten; tokens naming a file in `keep` are file names; a line
    that would become a self-calling forwarder is left as it was."""
    hits = {}

    def sub(m):
        new = fwd.get(m.group(1).upper())
        if new is None or m.group(1) in keep:
            return m.group(0)
        hits[m.group(1)] = new
        return new

    def sub_code(chunk):
        return TOKEN.sub(sub, chunk)
    if is_code:
        out, pos = [], 0
        for c in COMMENT.finditer(text):
            out.append(sub_code(text[pos:c.start()]))
            out.append(c.group(0))
            pos = c.end()
        out.append(sub_code(text[pos:]))
        text2 = ''.join(out)
    else:
        text2 = ''.join(l if l.lstrip().startswith('#') else sub_code(l)
                        for l in text.splitlines(True))
    if text2 != text:
        a, b = text.split('\n'), text2.split('\n')
        if len(a) == len(b):
            for i, (x, y) in enumerate(zip(a, b)):
                if x != y and SELF_CALL.search(y) and not SELF_CALL.search(x):
                    b[i] = x
            text2 = '\n'.join(b)
    return text2, hits


DEF_RE = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;{)]*\)\s*\{', re.M)


def absorbed_head_shim(path, before, after, fwd):
    """A src/main/pc-port/func_<HEAD>.c that reproduces a cross-prologue head
    fragment and calls the host.  When the host was ABSORBED (its record now
    sits at the head address, so func_<HEAD> IS the host's old name), the
    rename turns `func_HEAD(...) { ...; return host(...); }` into a function
    that calls itself.  The host's C (window widened over the head) already
    carries the fragment, so the shim is obsolete.  Returns (host, where) when
    `path` is such a shim and the host is defined by a matched/override file,
    else None."""
    if not path.startswith(MOVE_DIR + '/'):
        return None
    stem = os.path.splitext(os.path.basename(path))[0]
    host = fwd.get(stem.upper())
    if not host:
        return None
    body = ADS.strip_comments(after)
    defs = DEF_RE.findall(body)
    if host not in defs:
        return None
    # does the renamed body CALL the host (not just declare it) from inside its own definition?
    h = re.escape(host)
    code = re.sub(r'^[ \t]*(?!return\b)(extern[ \t]+)?[A-Za-z_][\w \t\*]*?\b' + h + r'\s*\([^;{]*\)\s*;[^\n]*$', '', body, flags=re.M)
    code = re.sub(DEF_RE, '{', code)
    if not re.search(r'(?<![\w.>])' + h + r'\s*\(', code):
        return None
    for d in ('src/main/matched', 'src/main/semi-matched', 'src/main/archive', MOVE_DIR):
        for cand in (host + '.c', host + '_painted.c'):
            cp = os.path.join(d, cand)
            if cp != path and os.path.exists(os.path.join(ROOT, spell(cp))):
                return host, cp
    return None


def superseded_override(path, fwd):
    """src/main/pc-port/func_<ADDR>.c whose function already has an override
    under its landed name (src/main/pc-port/<name>.c).  Two overrides for one
    symbol cannot both stay: the one written under the real name was written
    for that function knowingly; the address-named one is a stale stub or a
    head shim.  Returns the keeper's path, else None."""
    if not path.startswith(MOVE_DIR + '/'):
        return None
    stem = os.path.splitext(os.path.basename(path))[0]
    host = fwd.get(stem.upper())
    if not host:
        return None
    keeper = os.path.join(MOVE_DIR, host + '.c')
    return keeper if os.path.exists(os.path.join(ROOT, keeper)) else None


def drop_self_cycles(text):
    dropped = []

    def keep(m):
        if m.group(1) == m.group(2):
            dropped.append(m.group(0).strip())
            return ''
        return m.group(0)
    text2 = ADS.SELF_ALIAS.sub(keep, ADS.SELF_TRAMPOLINE.sub(keep, text))
    return text2, dropped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--dry-run', action='store_true')
    g.add_argument('--apply', action='store_true')
    ap.add_argument('--only', action='append', default=[], help='name, func_/D_ address name or 0x key (repeatable)')
    ap.add_argument('--verbose', '-v', action='store_true', help='print every token hit')
    args = ap.parse_args()
    only = set(args.only)

    doc = SN.load_names()
    fwd, skipped, refuted = rename_map(doc, only)
    print(f'{len(fwd)} landed renames to replay, '
          f'{len(skipped)} libc collisions left address-named (sony_bios_alias.h bridges them)')
    if args.verbose:
        for old, name in skipped:
            print(f'   keep {old} (= {name})')

    files = pc_files()
    KEEP = protected_stems()
    touched, total_hits, moves, removed = [], 0, [], []
    for f in files:
        p = os.path.join(ROOT, f)
        try:
            text = open(p, encoding='utf-8').read()
        except UnicodeDecodeError:
            continue
        text2, hits = apply_to_text(text, fwd, os.path.splitext(f)[1] in CODE_EXT,
                                    KEEP if f == 'pc/Makefile' or f.endswith('.mk') else frozenset())
        dropped = []
        if os.path.splitext(f)[1] in CODE_EXT:
            text2, dropped = drop_self_cycles(text2)
        if text2 == text:
            continue
        dup = superseded_override(f, fwd)
        if dup:
            removed.append(f)
            print(f'{"RM   " if args.apply else "WOULD rm"} {f}: {dup} already overrides this function under its name')
            if args.apply:
                os.remove(p)
            continue
        shim = absorbed_head_shim(f, text, text2, fwd)
        if shim:
            host, where = shim
            removed.append(f)
            print(f'{"RM   " if args.apply else "WOULD rm"} {f}: absorbed-head shim of {host} '
                  f'(the rename would make it call itself; {where} defines the host)')
            if args.apply:
                os.remove(p)
            continue
        touched.append(f)
        total_hits += len(hits)
        print(f'{"EDIT " if args.apply else "WOULD"} {f}: {len(hits)} names' + (f', {len(dropped)} self-cycles dropped' if dropped else ''))
        if args.verbose:
            for o, n in sorted(hits.items()):
                print(f'          {o} -> {n}')
            for d in dropped:
                print(f'          dropped: {d[:100]}')
        if args.apply:
            open(p, 'w', encoding='utf-8').write(text2)
    # file moves: an override named after the address must follow the function;
    # so must the archive tier (src/main/archive/<name>_painted.c, PC-only C the
    # landing tool does not move): pc/Makefile filters HAL_EXCLUDES by basename
    # (the archive tier is toki1-decomp's when that is a separate checkout: this
    # tool never writes there; rename it with the decomp's landing tools)
    for mdir, suffix in ((MOVE_DIR, ''), (ARCHIVE_DIR, '_painted'))[:2 if MONOREPO_DECOMP else 1]:
      for fn in sorted(os.listdir(os.path.join(ROOT, mdir))):
        stem, ext = os.path.splitext(fn)
        if ext != '.c' or os.path.join(mdir, fn) in removed or not stem.endswith(suffix):
            continue
        new = fwd.get(stem[:len(stem) - len(suffix)].upper() if suffix else stem.upper())
        if not new:
            continue
        src = os.path.join(mdir, fn)
        dst = os.path.join(mdir, new + suffix + '.c')
        if os.path.exists(os.path.join(ROOT, dst)):
            print(f'CONFLICT {src} -> {dst}: target exists (two overrides for one function; merge by hand)')
            continue
        moves.append((src, dst))
        print(f'{"MOVE " if args.apply else "WOULD"} {src} -> {dst}')
        if args.apply:
            shutil.move(os.path.join(ROOT, src), os.path.join(ROOT, dst))
    # overrides still named after a REFUTED name: the function they override
    # went back to its address name (or on to another name) while pc/ was
    # excluded; the file now defines a symbol nothing calls -- or worse, the
    # name of a different function.  Reported only: resolve by hand.
    for fn in sorted(os.listdir(os.path.join(ROOT, MOVE_DIR))):
        stem, ext = os.path.splitext(fn)
        if ext == '.c' and stem in refuted:
            print(f'REVIEW {MOVE_DIR}/{fn}: "{stem}" was reverted and is no live name (see symbol_names.json reverted.from)')
    print(f'{len(touched)} files, {total_hits} token renames, {len(moves)} file moves'
          + ('' if args.apply else ' (dry run)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
