# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a clean 2 MiB PS1 RAM image from the player's extracted executable.

The pointer-width regression tests need static retail tables, not a live,
mid-game memory snapshot.  Keeping the source explicit lets the tests run
without redistributing RAM dumps.
"""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
PSX_EXE = ROOT / 'pc/assets/raw/PSX.EXE'
RAM_BASE = 0x80000000
RAM_SIZE = 0x200000


def retail_ram():
    """Return PS1 RAM containing the PS-X EXE's initialised data segment."""
    data = PSX_EXE.read_bytes()
    if len(data) < 0x800 or data[:8] != b'PS-X EXE':
        raise ValueError('%s is not a PS-X EXE' % PSX_EXE)
    load, size = struct.unpack_from('<II', data, 0x18)
    start = load - RAM_BASE
    end = start + size
    if start < 0 or end > RAM_SIZE or len(data) < 0x800 + size:
        raise ValueError('%s has an invalid load segment' % PSX_EXE)
    ram = bytearray(RAM_SIZE)
    ram[start:end] = data[0x800:0x800 + size]
    return bytes(ram)
