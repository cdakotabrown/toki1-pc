# SPDX-License-Identifier: GPL-3.0-or-later
"""libsnd's score table read 4 bytes per sequence (pc/tools/adapt_ps1_cells.py).

`_ss_score` (0x80131380) holds one 4-byte PS1 pointer per sequence
(SsSetTableSize writes it as u32).  The matched readers declare it
`extern T *_ss_score[]`, so on LP64 `_ss_score[seq]` reads cells 2*seq and
2*seq+1 as one pointer.  pc/Makefile compiles every game source that
declares it through adapt_ps1_cells.py.

Each case compiles the function as pc/Makefile does, links it with
psx_globals.syms and a 2 MB PS1 RAM at 0x80000000, opens three sequences
(score records at 0x80100000 + 0x1000*seq) and calls it for sequence 1:

  fix:              the adapted source touches sequence 1's records;
  negative control: the matched source reads cells 2 and 3 as one
                    pointer (a non-canonical address: the process faults).
"""
from pathlib import Path
import tempfile
import unittest

import adapt_ps1_cells as adapter
import test_ps1_pointer_reads as reads
import test_ps1_pointer_width as width

ROOT = width.ROOT
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import spell  # noqa: E402

PROLOGUE = r"""
#include <stdio.h>
#include <sys/mman.h>
extern unsigned int _ss_score[];
extern int _snd_openflag;
extern short _snd_seq_s_max, _snd_seq_t_max;
extern int _snd_ev_flag;
static int map(void)
{
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    int s;
    if (ram != (unsigned char *)0x80000000UL) return 2;
    for (s = 0; s < 4; s++)
        _ss_score[s] = 0x80100000u + 0x1000u * s;
    _snd_seq_s_max = 3;
    _snd_seq_t_max = 2;
    return 0;
}
#define REC(seq, trk) ((unsigned char *)0x80100000UL + 0x1000 * (seq) + 168 * (trk))
"""

CLOSE = PROLOGUE + r"""
void SpuVmSetSeqVol(short a, int b, int c, int d) {}
void SpuVmSeqKeyOff(short a) {}
void SsSeqClose(short seq);
int main(void)
{
    int r = map();
    if (r) return r;
    REC(1, 1)[0] = 7;
    *(short *)(REC(1, 1) + 114) = 5;
    SsSeqClose(1);
    printf("%d %d %d\n", REC(1, 1)[0], *(short *)(REC(1, 1) + 114), REC(0, 1)[0]);
    return 0;
}
"""

TICK = PROLOGUE + r"""
void SpuVmFlush(void) {}
void Snd_play(int i, int j) { printf("play %d %d\n", i, j); }
void Snd_crescendo(int i, int j) {}
void Snd_decrescendo(int i, int j) {}
void Snd_tempo(int i, int j) {}
void Snd_pause(int i, int j) {}
void Snd_replay(int i, int j) {}
void Snd_stop(int i, int j) {}
void SsSeqCalledTbyT(void);
int main(void)
{
    int r = map();
    if (r) return r;
    _snd_openflag = 1 << 1;
    *(int *)(REC(1, 1) + 144) = 1;
    SsSeqCalledTbyT();
    return 0;
}
"""


def run(func, source, harness, adapt):
    with tempfile.TemporaryDirectory() as d:
        src = ROOT / spell(source)
        if adapt:
            out = Path(d) / src.name
            out.write_text('#line 1 "%s"\n' % source +
                           adapter.adapt(src.read_text(), ['_ss_score']))
            src = out
        return reads.build_and_run(func, src, harness)


class Adapter(unittest.TestCase):
    def test_close_adapted(self):
        ok, out = run('SsSeqClose', 'src/main/matched/SsSeqClose.c', CLOSE, True)
        self.assertTrue(ok, out)
        # record (1, 1) closed (-1, 127); sequence 0 untouched
        self.assertEqual(out.split()[-3:], ['255', '127', '0'])

    def test_close_matched_faults(self):
        ok, out = run('SsSeqClose', 'src/main/matched/SsSeqClose.c', CLOSE, False)
        self.assertFalse(ok, out)

    def test_tick_adapted(self):
        """the sequencer tick (reached every frame music plays) plays
        sequence 1's track 1"""
        ok, out = run('SsSeqCalledTbyT', 'src/main/matched/SsSeqCalledTbyT.c', TICK, True)
        self.assertTrue(ok, out)
        self.assertIn('play 1 1', out)

    def test_tick_matched_faults(self):
        ok, out = run('SsSeqCalledTbyT', 'src/main/matched/SsSeqCalledTbyT.c', TICK, False)
        self.assertFalse(ok, out)

    def test_cell_address_walked_needs_override(self):
        """`&table[i]` that is stepped or handed on cannot be rewritten"""
        src = ('extern u8 *_ss_score[];\nvoid f(int n) { u8 **p = &_ss_score[n]; p++; }\n')
        with self.assertRaises(ValueError):
            adapter.adapt(src, ['_ss_score'])

    def test_record_address_reads_the_cell(self):
        src = ('extern R *_ss_score[];\nR *f(int a, int b) { return &_ss_score[a][b]; }\n')
        out = adapter.adapt(src, ['_ss_score'])
        self.assertIn('&((R *)(unsigned long)(*(u32 *)((u32 *)_ss_score + (a))))[b]', out)


CHAIN_SRC = """
u32 chain_probe(void *a3, int i, int j)
{
    return *((u32 ***)a3)[i][j];
}
"""

CHAIN = r"""
#include <stdio.h>
#include <sys/mman.h>
unsigned int chain_probe(void *a3, int i, int j);
int main(void)
{
    unsigned int *t = (unsigned int *)0x80100000UL;
    unsigned char *ram = mmap((void *)0x80000000UL, 0x200000,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (ram != (unsigned char *)0x80000000UL) return 2;
    /* t: 4 cells -> rows at 0x80100100 + 0x40*r; row cells -> words */
    for (int r = 0; r < 4; r++) {
        t[r] = 0x80100100u + 0x40u * r;
        for (int c = 0; c < 4; c++) {
            ((unsigned int *)(0x80100100UL + 0x40 * r))[c] = 0x80100800u + 0x10u * (4 * r + c);
            *(unsigned int *)(0x80100800UL + 0x10 * (4 * r + c)) = 100 * r + c;
        }
    }
    printf("%u\n", chain_probe((void *)0x80100000UL, 1, 2));
    return 0;
}
"""


class Chains(unittest.TestCase):
    """serifu_cmd_exec's `((u32 ***)a3)[a4][a5]` (--chains)"""

    def run_chain(self, adapt):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'chain_probe.c'
            text = CHAIN_SRC
            if adapt:
                text = adapter.adapt(text, [], chains=True)
            src.write_text(text)
            return reads.build_and_run('chain_probe', src, CHAIN)

    def test_adapted_reads_cells(self):
        ok, out = self.run_chain(True)
        self.assertTrue(ok, out)
        self.assertEqual(out.split()[-1], '102')

    def test_native_reads_joined_cells(self):
        ok, out = self.run_chain(False)
        self.assertFalse(ok and out.split()[-1:] == ['102'], out)

    def test_store_into_a_cell_refuses(self):
        with self.assertRaises(ValueError):
            adapter.adapt('void f(void *p, u8 *q) { ((u8 **)p)[3] = q; }', [], chains=True)


if __name__ == '__main__':
    unittest.main()
