#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ps1_residue.py -- host-stack residue in a 2 MB PS1 RAM dump.

Matched code copies two buffers whose tails it never writes, so those bytes
hold whatever the host stack held there (the twin of pc/hal/parity.c
parity_mask_host_residue, which PC_FRAME_HASH_RAM=game applies before
hashing; test_ps1_residue.py checks that the two agree):

  * the draw environment libgpu_cur_drawenv (DRAWENV, 92 bytes): the words of the
    DR_ENV at +0x1C past its packet length (tag bits 24-31);
  * cdq_que (48 records x 24 bytes at 0x800E8DA8): cdq_command_enqueue
    copies 16 bytes of the caller's data into each record; the reader uses
    type, arg, data[0..3] and the flag, never data[4..14] (record bytes
    +12..+22: a read command copies its destination buffer's old contents,
    host memory or not).  A set-mode (type 14) also copies
    cd_command_issue's uninitialised local, of which only data[0] is set:
    +9..+11 as well.

The game never reads them back; their values depend on which host code ran
on the stack before (on Windows, whether a feature is on).  Compare RAM
dumps of runs whose host code differs with mask() applied to both.

    python3 pc/tools/ps1_residue.py A.ram B.ram    # compare, residue masked
"""
import sys

RAM_BASE = 0x80000000
DRAWENV = 0x800EBD70 - RAM_BASE
CDQ_QUE = 0x800E8DA8 - RAM_BASE


def mask(ram):
    """A copy of `ram` (bytes, the 2 MB block) with the residue cleared."""
    out = bytearray(ram)
    tag = DRAWENV + 0x1C
    length = out[tag + 3]
    for i in range(1 + length, (92 - 0x1C) // 4):
        out[tag + 4 * i:tag + 4 * i + 4] = bytes(4)
    for r in range(48):
        rec = CDQ_QUE + 24 * r
        out[rec + 12:rec + 23] = bytes(11)            # data[4..14]: never read
        if out[rec] == 14:                            # set-mode reads data[0] only
            out[rec + 9:rec + 12] = bytes(3)
    return bytes(out)


def differences(a, b):
    """Word addresses where the masked dumps differ."""
    a, b = mask(a), mask(b)
    return [RAM_BASE + i for i in range(0, min(len(a), len(b)), 4) if a[i:i + 4] != b[i:i + 4]]


if __name__ == '__main__':
    d = differences(open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read())
    for a in d[:50]:
        print(hex(a))
    print(f'{len(d)} words differ (host residue masked)')
    sys.exit(1 if d else 0)
