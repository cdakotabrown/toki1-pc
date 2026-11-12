#!/usr/bin/env python3
"""Where the three parts of the project live.

The project is published as three repositories checked out side by side:
toki1-decomp (the decompiled C and its tools), toki1-english (the English
patch) and toki1-pc (the PC port).  A tool that needs a file of another
part takes its root from here and never from its own repository root:

    from repo_paths import DECOMP, ENGLISH, PC
    sym = DECOMP / 'config' / 'symbol_names.json'

Each root is, in order: the environment variable (TOKI_DECOMP_DIR,
TOKI_ENGLISH_DIR, TOKI_PC_DIR); this checkout, when it holds that part
(a checkout with all three parts resolves all three to itself); the
sibling directory.  ENGLISH and PC are None when they are not present;
DECOMP is required and raises if missing (unless the environment sets
TOKI_DECOMP_OPTIONAL=1, for a tool that can run without it).

A sibling checkout is read, never written (contract rule 4): when any
part resolves outside this checkout, Python stops writing bytecode
(__pycache__/*.pyc) for the modules this process imports from it, and the
environment carries PYTHONDONTWRITEBYTECODE=1 to the subprocesses that run
its scripts.  In a checkout holding every part nothing changes.

THE ENGLISH LAYOUT.  toki1-english is published flat (sheets/, guide/,
art/, patch/, config/, tools/) while this monorepo keeps its own paths
(translation/localization/sheets/, src/patch/, config/patch/, ...).
ENGLISH_LAYOUT is the one rename table: tools/release/split/export_split.py
writes the public tree with it, and every reader of an English file maps a
path through it at run time.  Code and data name an English file by its
MONOREPO path (the logical name: the one spelling that is unambiguous across
the three parts, since the public toki1-english's config/ and tools/ would
collide with toki1-decomp's); the layer turns it into the path of the
checkout it runs in:

    english_layout(root)   'public' or 'monorepo' (public: a sheets/
                           directory and no config/patch/)
    english_rel(path, root=ENGLISH)  the path as that checkout spells it
    english_file(path)     ENGLISH / english_rel(path)
    public_path(path)      the public spelling (pure: the export uses it)
    logical_path(path)     the inverse: a public spelling back to the
                           monorepo one (identity on anything else)

This file is identical in the three repositories.
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent

# part -> (environment variable, sibling directory, marker paths: any one)
_PARTS = {
    'DECOMP': ('TOKI_DECOMP_DIR', 'toki1-decomp', ('src/main/matched',)),
    'ENGLISH': ('TOKI_ENGLISH_DIR', 'toki1-english', ('config/patch', 'sheets')),
    'PC': ('TOKI_PC_DIR', 'toki1-pc', ('pc/Makefile',)),
}

# toki1-english's public layout: monorepo prefix -> public prefix.  The
# longest matching prefix wins; a prefix ending in '/' is a directory, any
# other is one file.  Every exported English file must map to a distinct
# public path (export_split.py --check --strict).  The generated, ignored
# files the tools write under translation/ map to work/.
ENGLISH_LAYOUT = (
    ('translation/localization/sheets/', 'sheets/'),
    ('translation/localization/audit/', 'guide/audit/'),
    ('translation/localization/containers.json', 'config/containers.json'),
    ('translation/localization/', 'guide/'),
    ('translation/text_pointers.json', 'config/text_pointers.json'),
    ('translation/reviewer/', 'tools/reviewer/'),
    ('translation/work/', 'work/'),
    ('translation/tokimeki_script.csv', 'work/tokimeki_script.csv'),
    ('translation/tokimeki_script.filtered.csv', 'work/tokimeki_script.filtered.csv'),
    ('translation/tokimeki_script.pot', 'work/tokimeki_script.pot'),
    ('translation/coverage_report.json', 'work/coverage_report.json'),
    ('translation/extract_report.json', 'work/extract_report.json'),
    ('translation/patches/', 'work/patches/'),
    ('translation/fonts/', 'work/fonts/'),
    ('tools/patch/', 'tools/'),
    ('tools/translation/', 'tools/'),
    ('config/patch/', 'config/'),
    ('src/patch/main/', 'patch/'),
    ('src/patch/debug/', 'patch/debug/'),
    ('src/patch/drawlog/drawlog.c', 'patch/debug/drawlog.c'),
    ('src/patch/art/', 'art/'),
    ('src/patch/', 'patch/'),
    ('docs/translation/', 'docs/'),
)


def _swap(path, table):
    s = str(path).replace(os.sep, '/')
    best = None
    for src, dst in table:
        if src.endswith('/'):
            hit = s.startswith(src) or s == src[:-1]
        else:
            hit = s == src
        if hit and (best is None or len(src) > len(best[0])):
            best = (src, dst)
    if best is None:
        return s
    src, dst = best
    if src.endswith('/') and s == src[:-1]:
        return dst[:-1]
    return dst + s[len(src):]


def public_path(path):
    """The public toki1-english spelling of a monorepo path (identity for a
    path the table does not name)."""
    return _swap(path, ENGLISH_LAYOUT)


# public -> monorepo, for the public prefixes no other part uses; config/,
# tools/ and docs/ are also toki1-decomp's, so a public spelling there is
# ambiguous and only the monorepo spelling names an English file
_UNAMBIGUOUS = ('sheets/', 'guide/', 'art/', 'patch/', 'work/')


def logical_path(path):
    """The monorepo spelling of a public toki1-english path under sheets/,
    guide/, art/, patch/ or work/ (identity on anything else)."""
    s = str(path).replace(os.sep, '/')
    if not s.startswith(_UNAMBIGUOUS) and s not in {u[:-1] for u in _UNAMBIGUOUS}:
        return s
    inverse = [(dst, src) for src, dst in ENGLISH_LAYOUT
               if dst.startswith(_UNAMBIGUOUS) and src.endswith('/') == dst.endswith('/')]
    return _swap(s, inverse)


def english_layout(root):
    """'public' for a published toki1-english checkout, else 'monorepo'."""
    root = Path(root)
    if (root / 'sheets').is_dir() and not (root / 'config' / 'patch').is_dir():
        return 'public'
    return 'monorepo'


def _find(part):
    env, sibling, markers = _PARTS[part]
    value = os.environ.get(env)
    if value:
        root = Path(value).expanduser().resolve()
        if not any((root / m).exists() for m in markers):
            raise SystemExit('%s=%s does not hold %s' % (env, value, ' or '.join(markers)))
        return root
    if any((HERE / m).exists() for m in markers):
        return HERE
    root = HERE.parent / sibling
    if any((root / m).exists() for m in markers):
        return root
    return None


DECOMP = _find('DECOMP')
ENGLISH = _find('ENGLISH')
PC = _find('PC')

# TOKI_DECOMP_OPTIONAL=1: a tool that can run without toki1-decomp (the
# English patch's disc-free sheet lint) gets DECOMP = None instead
if DECOMP is None and not os.environ.get('TOKI_DECOMP_OPTIONAL'):
    raise SystemExit(
        'toki1-decomp not found: check it out next to this repository '
        '(%s) or set TOKI_DECOMP_DIR' % (HERE.parent / 'toki1-decomp'))

# Rule 4: no bytecode written into a sibling checkout, by this process or by
# the Python subprocesses it starts (which inherit the environment).
FOREIGN = [root for root in (DECOMP, ENGLISH, PC)
           if root is not None and root != HERE]
if FOREIGN:
    sys.dont_write_bytecode = True
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'

ENGLISH_PUBLIC = ENGLISH is not None and english_layout(ENGLISH) == 'public'


def english_rel(path, root=None):
    """A monorepo-spelled path as the English checkout `root` (default
    ENGLISH) spells it; a public spelling under sheets/, guide/, art/,
    patch/ or work/ is accepted too."""
    root = ENGLISH if root is None else root
    s = logical_path(path)
    if root is not None and english_layout(root) == 'public':
        return public_path(s)
    return s


def english_file(path):
    """The English part's file for a monorepo-spelled path."""
    if ENGLISH is None:
        raise SystemExit('toki1-english not found: check it out next to this '
                         'repository (%s) or set TOKI_ENGLISH_DIR'
                         % (HERE.parent / 'toki1-english'))
    return ENGLISH / english_rel(path)


if __name__ == '__main__':
    # repo_paths.py               the three roots (NAME=path)
    # repo_paths.py --english P..  each monorepo-spelled English path as the
    #                              English checkout spells it, one per line
    if sys.argv[1:2] == ['--english']:
        for arg in sys.argv[2:]:
            print(english_rel(arg))
    else:
        for name in ('DECOMP', 'ENGLISH', 'PC'):
            print('%s=%s' % (name, globals()[name] or ''))
