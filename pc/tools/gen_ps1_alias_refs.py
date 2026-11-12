#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Name the PS1 globals that overlap another C name, and make game code
reach them through an opaque address (PC build input generator).

The port links PS1 globals at their PS1 addresses, and two linker scripts
give some bytes a second C name:

  pc/src/layout_aliases.ld   members inside a containing block
                             (`icon_num = screen_w + 0xF6D`): screen_w,
                             class_w, gSDInfo, cdq_head, cdq_result;
  pc/src/missing_aliases.ld  typed views at the same address
                             (`PROVIDE(D_8012B1E8_v = now_date_spot)`).

GCC assumes that two distinct declared objects never overlap (its alias
oracle's base-decl rule; -fno-strict-aliasing does not change it), so in a
function that writes `p_birthday` and then reads the same halfword as
`*(u16 *)(screen_w + 248)` it may move the read above the write.  It did:
NAME_ENT name_ent_record_set lost the player's birth month that way (the
volatile halfword read-modify-write of screen_w + 248 was scheduled before
the store to p_birthday, and wrote the old month back).  The console has
one flat RAM; any access may alias any other.

The fix: game code never names such a symbol directly.  The forced header
this writes, pc/include/game/ps1_alias_refs.h, defines

    #define screen_w (*ps1_ref_screen_w())

for every container (for cdq_head / cdq_result, which C defines, their
members instead) and every typed-view alias, so a translation unit's
own `extern u8 screen_w[];` declares `ps1_ref_screen_w` as a function
returning `u8 (*)[]` -- the file's own type -- and each access goes
through the address that function returns, which GCC cannot pin to an
object.  pc/src/ps1_alias_refs.c (also written here, compiled without the
header) returns the linker symbol's address.  The members (icon_num,
now_step, ...) and the base symbols of the views are left alone: one
opaque side of each pair is enough to keep every pair ordered.

pc/Makefile force-includes the header into the game objects and the pc-port
overrides.  Regenerate after a change to either .ld file:

    python3 pc/tools/gen_ps1_alias_refs.py          # writes both files
    python3 pc/tools/gen_ps1_alias_refs.py --check  # exit 1 if stale
    python3 pc/tools/gen_ps1_alias_refs.py --audit  # a routed name used as a
                                                    # local/parameter (exit 1)
A name a game file also uses for a local or a parameter stays direct (the
macro would turn the local into a block-scope function declaration whose
`x = ...` writes the global, and a parameter into a function pointer the
body calls: libsnd GetMetaEvent's `seq_no`, a gSDInfo member's name).
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import decomp, decomp_key, spell  # noqa: E402
LAYOUT = os.path.join(ROOT, 'pc/src/layout_aliases.ld')
MISSING = os.path.join(ROOT, 'pc/src/missing_aliases.ld')
HEADER = os.path.join(ROOT, 'pc/include/game/ps1_alias_refs.h')
SOURCE = os.path.join(ROOT, 'pc/src/ps1_alias_refs.c')


def strip_comments(text):
    return re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)


# the C compiled with the header (pc/Makefile COST_OBJS: game sources, the
# overlay modules, the pc-port overrides) that may DEFINE a container -- the
# macro cannot stand in a definition.  pc/src/globals.c, which defines
# screen_w / class_w / gSDInfo, is compiled without it.
DEFINERS = ('src/main/matched', 'src/main/semi-matched', 'src/main/archive',
            'src/main/pc-port', 'pc/src')


_DEFINED = None
DEF_RE = re.compile(r'^(?!extern\b)(?!typedef\b)(?!return\b)[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*'
                    r'(?:\[[^\]]*\])*\s*(?:=|;)', re.M)


# a local or a parameter of the same name: the macro would turn
# `s16 seq_no;` into a block-scope declaration of the function
# ps1_ref_seq_no (every later `seq_no = x` then writes the GLOBAL), and a
# parameter `s16 seq_no` into a function-pointer parameter that the body
# calls (libsnd GetMetaEvent did exactly that).  Such names stay direct.
TYPE_WORDS = r'(?:(?:const|volatile|unsigned|signed|struct|union|enum|static|register)\s+)*'
LOCAL_RE = re.compile(r'^[ \t]+(?!return\b|goto\b|case\b|else\b|extern\b)' + TYPE_WORDS +
                      r'[A-Za-z_]\w*[ \t\*]+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?=[=;,)]|$)', re.M)
PARAM_RE = re.compile(r'[(,]\s*' + TYPE_WORDS +
                      r'[A-Za-z_]\w*[\s\*]+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?=[,)])')
# later declarators of a block-scope declaration (`s16 a, seq_no;`)
MULTI_RE = re.compile(r'^[ \t]+' + TYPE_WORDS + r'[A-Za-z_]\w*[ \t\*]+[^;(){}=]*,[ \t\*]*'
                      r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?=[;,=])', re.M)
_LOCAL = None


def scan_sources():
    """(names a compiled-with-the-header C file defines at file scope,
        names it uses for a local, a parameter or a later declarator)"""
    global _DEFINED, _LOCAL
    if _DEFINED is None:
        _DEFINED, _LOCAL = set(), set()
        for d in DEFINERS:
            base = os.path.join(ROOT, spell(d))
            if not os.path.isdir(base):
                continue
            for f in sorted(os.listdir(base)):
                if f.endswith('.c') and (d != 'pc/src' or f.startswith('overlay_')):
                    with open(os.path.join(base, f), errors='replace') as fh:
                        text = strip_comments(fh.read())
                    _DEFINED |= set(DEF_RE.findall(text))
                    _LOCAL |= set(LOCAL_RE.findall(text)) | set(PARAM_RE.findall(text))
                    _LOCAL |= set(MULTI_RE.findall(text))
    return _DEFINED, _LOCAL


def defined_in_c():
    return scan_sources()[0]


def names():
    """-> (laundered, kept): every name on either side of
    layout_aliases.ld (`member = base + off`) and missing_aliases.ld
    (`PROVIDE(alias = base)`), minus the ones a compiled-with-the-header C
    file defines (CDQ_init.c defines cdq_head / cdq_result: the macro cannot
    stand in a definition; their members are still laundered, which keeps
    each pair ordered) or uses as a local or parameter name (LOCAL_RE)"""
    with open(LAYOUT) as fh:
        pairs = re.findall(r'^\s*(\w+)\s*=\s*(\w+)\b', strip_comments(fh.read()), re.M)
    with open(MISSING) as fh:
        pairs += re.findall(r'PROVIDE\(\s*(\w+)\s*=\s*(\w+)\s*\)', strip_comments(fh.read()))
    every = {n for pair in pairs for n in pair}
    defined, local = scan_sources()
    kept = every & (defined | local)
    return sorted(every - kept), sorted(kept)


def render():
    allnames, kept = names()
    h = ['/* Generated by pc/tools/gen_ps1_alias_refs.py -- do not edit.',
         ' * PS1 globals that overlap another C name (both sides of',
         ' * pc/src/layout_aliases.ld and pc/src/missing_aliases.ld):',
         ' * game code reaches them through an address GCC cannot pin to one',
         ' * object, so it keeps their accesses ordered against the other name\'s',
         ' * (see the generator\'s docstring).  Forced into the game objects and',
         ' * the pc-port overrides by pc/Makefile.',
         ' * Defined in game C, or a local/parameter name there, so named',
         ' * directly: %s */' % (' '.join(kept) or '-'),
         '#ifndef PS1_ALIAS_REFS_H',
         '#define PS1_ALIAS_REFS_H',
         '']
    for n in allnames:
        h.append('#define %s (*ps1_ref_%s())' % (n, n))
    h += ['', '#endif', '']
    c = ['/* Generated by pc/tools/gen_ps1_alias_refs.py -- do not edit.',
         ' * The addresses behind pc/include/game/ps1_alias_refs.h: each',
         ' * function returns its linker symbol\'s address (PS1 RAM).  Not',
         ' * compiled with that header. */',
         '']
    for n in allnames:
        c.append('extern char %s[];' % n)
        c.append('void *ps1_ref_%s(void) { return %s; }' % (n, n))
    c.append('')
    return '\n'.join(h), '\n'.join(c)


DECL_USE = re.compile(r'(?<![\w>.])((?!return\b|extern\b|sizeof\b|case\b|else\b)[A-Za-z_]\w*)\s+'
                      r'\(\*(ps1_ref_\w+)\(\)\)')
GAME_FLAGS = ['-std=gnu17', '-w', '-D_GNU_SOURCE', '-DPC_PORT', '-include', 'pc/include/game/types.h',
              '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include'),
              '-include', 'pc/include/game/ps1_alias_refs.h']


def audit_declarations(jobs=16, header=None, files=None):
    """Preprocess every compiled-with-the-header source and return the
    places where a routed name stands in a DECLARATION other than an extern
    (a local, a parameter): [(file, type, function, statement)].  The
    LOCAL_RE scan above keeps such names direct; this is its check."""
    import subprocess
    from concurrent.futures import ThreadPoolExecutor
    flags = list(GAME_FLAGS)
    if header:
        flags[-1] = header
    if files is None:
        files = []
        for d in DEFINERS:
            base = os.path.join(ROOT, spell(d))
            if os.path.isdir(base):
                files += [os.path.join(spell(d), f) for f in sorted(os.listdir(base))
                          if f.endswith('.c') and (d != 'pc/src' or f.startswith('overlay_'))]

    def one(f):
        r = subprocess.run(['gcc', '-E', '-P'] + flags + ['-iquote', os.path.dirname(f), f],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            return [(decomp_key(f), '-', '-', 'preprocessing failed')]
        text = '\n'.join(l for l in r.stdout.split('\n') if not l.lstrip().startswith('#'))
        out = []
        for m in DECL_USE.finditer(text):
            start = max(text.rfind(c, 0, m.start()) for c in ';{}')
            stmt = ' '.join(text[start + 1:m.end()].split())
            if not stmt.startswith('extern '):
                out.append((decomp_key(f), m.group(1), m.group(2), stmt[:120]))
        return out
    with ThreadPoolExecutor(jobs) as ex:
        return [h for hits in ex.map(one, files) for h in hits]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true', help='exit 1 if the files are stale')
    ap.add_argument('--audit', action='store_true',
                    help='list routed names used as a local or parameter (exit 1 if any)')
    a = ap.parse_args(argv)
    if a.audit:
        hits = audit_declarations()
        for h in hits:
            print('%s: %s %s in `%s`' % h)
        return 1 if hits else 0
    header, source = render()
    stale = []
    for path, text in ((HEADER, header), (SOURCE, source)):
        old = None
        if os.path.exists(path):
            with open(path) as fh:
                old = fh.read()
        if old != text:
            stale.append(os.path.relpath(path, ROOT))
            if not a.check:
                with open(path, 'w') as fh:
                    fh.write(text)
    if a.check:
        if stale:
            print('stale: %s (run pc/tools/gen_ps1_alias_refs.py)' % ' '.join(stale))
            return 1
        return 0
    print('wrote %s' % (' '.join(stale) or 'nothing (up to date)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
