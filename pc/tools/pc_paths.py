# SPDX-License-Identifier: GPL-3.0-or-later
"""Where pc/tools finds the other parts of the project (docs/REPO-SPLIT-PLAN.md).

The PC port is published as toki1-pc next to toki1-decomp (the decompiled C,
include/, config/, the decomp tools) and, optionally, toki1-english (the
English patch).  tools/repo_paths.py resolves the three roots; this module
is the pc/tools view of it:

    from pc_paths import ROOT, DECOMP, ENGLISH, decomp, english

    ROOT     this checkout (the PC port; pc/build stays here)
    DECOMP   the toki1-decomp root (a Path; required)
    ENGLISH  the toki1-english root (a Path), or None when it is absent
    decomp('src/main/matched/x.c')   a decomp file, spelled for the
             compiler and for pc/Makefile: in the monorepo (DECOMP == ROOT)
             the relative path unchanged, else the absolute path
    english('tools/patch/fontgen.py')   the same for the English part
             (raises when toki1-english is absent: check ENGLISH first);
             the path is the monorepo spelling, mapped to the layout of the
             English checkout (tools/repo_paths.py ENGLISH_LAYOUT: the
             public toki1-english is flat, tools/fontgen.py)
    english_path('src/patch/art')   the same as an absolute Path
    decomp_key(path)   the inverse of decomp(): a decomp file's path
             relative to DECOMP ('src/main/matched/x.c'), the spelling the
             plan JSONs and allow-lists key on, from either spelling

In this repository all three roots are the repository root, so every path
a tool builds is the one it built before the split.
"""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / 'tools') not in sys.path:
    sys.path.insert(0, str(ROOT / 'tools'))
from repo_paths import DECOMP, ENGLISH, english_rel  # noqa: E402

MONOREPO_DECOMP = DECOMP == ROOT
MONOREPO_ENGLISH = ENGLISH == ROOT
# the spelling pc/Makefile uses too ($(realpath) of DECOMP_DIR / ENGLISH_DIR)
_DECOMP_REAL = Path(os.path.realpath(DECOMP))
_ENGLISH_REAL = Path(os.path.realpath(ENGLISH)) if ENGLISH is not None else None


def decomp(rel):
    rel = str(rel)
    return rel if MONOREPO_DECOMP else str(_DECOMP_REAL / rel)


def english(rel):
    if ENGLISH is None:
        raise SystemExit('toki1-english not found: check it out next to this '
                         'repository (%s) or set TOKI_ENGLISH_DIR'
                         % (ROOT.parent / 'toki1-english'))
    rel = english_rel(str(rel))
    return rel if MONOREPO_ENGLISH else str(_ENGLISH_REAL / rel)


def english_path(rel):
    """An English file as an absolute Path (monorepo spelling, mapped)."""
    if ENGLISH is None:
        english(rel)                    # raises, saying why
    return ENGLISH / english_rel(str(rel))


def decomp_key(path):
    """'src/main/matched/x.c' for any spelling of a decomp file's path."""
    p = str(path)
    if not os.path.isabs(p):
        if MONOREPO_DECOMP:
            return os.path.normpath(p)
        p = os.path.join(os.getcwd(), p)
    try:
        return Path(os.path.realpath(p)).relative_to(_DECOMP_REAL).as_posix()
    except ValueError:
        return os.path.normpath(str(path))


# Paths this checkout owns (the PC port); everything else a pc/tools plan,
# allow-list or test names by a repository-relative path is a decomp file.
PC_OWNED = ('pc/', 'src/main/pc-port/', 'docs/pc-port/', 'run_game.sh')


def spell(rel):
    """A repository-relative path as the PC build spells it: a PC file
    unchanged, a decomp file through decomp()."""
    rel = str(rel)
    return rel if rel.startswith(PC_OWNED) else decomp(rel)


def disc_input(rel):
    """A disc-derived input that the developer supplies (disks/<cue,bin>,
    disks/SLPS_000.65, assets/overlays/<OV>.EXN, ...): this checkout's copy,
    else the one in the toki1-decomp checkout (its build extracts the same
    files), else this checkout's path (which does not exist: the caller's
    'missing' message names it)."""
    mine = ROOT / rel
    if mine.exists() or MONOREPO_DECOMP:
        return mine
    theirs = DECOMP / rel
    return theirs if theirs.exists() else mine


def have_commit(rev):
    """True when this checkout's git history holds `rev`.  Negative controls
    that build an older revision of a file (`git show <rev>:pc/hal/x.c`)
    need the monorepo's history; a published single-commit checkout lacks
    it, and those tests skip, by name, saying so."""
    import subprocess
    return subprocess.run(['git', '-C', str(ROOT), 'cat-file', '-e', f'{rev}^{{commit}}'],
                          capture_output=True).returncode == 0


NO_HISTORY = ('needs commit %s of the project history (a negative control built from '
              'an older file); this checkout does not have it')
