# SPDX-License-Identifier: GPL-3.0-or-later
"""The reference emulator: where the public tools find it.

The PC port is checked against an existing PS1 emulator run headless (its
"regtest" host, patched for scripted input, traces and pokes). Neither that
emulator nor the tooling around it is distributed with this repository: it
lives in a gitignored local directory, by default `local/refemu/`, and
docs/pc-port/REFERENCE-EMULATOR.md describes the interface the tools expect.

  REFEMU_DIR      the local tooling directory (default <repo>/local/refemu)
  REFEMU_REGTEST  the regtest host to run (default $REFEMU_DIR/regtest, a
                  wrapper that sets up its libraries and environment)
  REFEMU_CPU      the CPU mode the trace tools ask for (default Interpreter)

  regtest()          path of the regtest host (str), present or not
  available()        True when that host exists and is executable
  require(what)      exit with the explanation below when it is absent
  local_module(name) import a module from the local tooling directory, or
                     raise RefEmuUnavailable with the same explanation
"""
import importlib
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
DIR = Path(os.environ.get('REFEMU_DIR') or ROOT / 'local/refemu')
DOC = 'docs/pc-port/REFERENCE-EMULATOR.md'
CPU = os.environ.get('REFEMU_CPU', 'Interpreter')


class RefEmuUnavailable(RuntimeError):
    pass


def regtest():
    return os.environ.get('REFEMU_REGTEST') or str(DIR / 'regtest')


def available():
    path = regtest()
    return os.path.isfile(path) and os.access(path, os.X_OK)


def explain(what='this'):
    return (f'{what} needs the reference emulator, which is not part of this '
            f'repository: no executable regtest host at {regtest()}. Set '
            f'REFEMU_REGTEST to one, or install the local tooling in {DIR} '
            f'(see {DOC}).')


def require(what='this tool'):
    if not available():
        sys.exit(explain(what))
    return regtest()


def local_module(name, what=None):
    """the module `name` from the local tooling directory"""
    if not (DIR / f'{name}.py').is_file():
        raise RefEmuUnavailable(
            f'{what or name} needs the local reference-emulator tooling '
            f'({DIR / (name + ".py")} is absent; see {DOC}).')
    if str(DIR) not in sys.path:
        sys.path.insert(0, str(DIR))
    return importlib.import_module(name)
