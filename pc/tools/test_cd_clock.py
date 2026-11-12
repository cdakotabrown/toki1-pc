# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the HAL drive with busy polling and ordinary frame polling."""
from pathlib import Path
import os
import tempfile
import unittest

from safe_process import run

ROOT = Path(__file__).resolve().parents[2]


class CdClockTests(unittest.TestCase):
    def test_busy_polling_completes_without_vsync(self):
        fixture = '''#define _GNU_SOURCE
#include <sys/mman.h>
#include "pc/hal/cd.c"
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
void hal_vblank_interrupt(void) {}
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) {}
void hal_overlay_set_loaded(const char *p) {}
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
int main(void) {
    u8 loc[4] = {0,2,0,0};
    u32 *p;
    u64 before;
    int i;
    p = mmap((void *)0x80134000UL, 4096, PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return 1;
    CdControl(2, loc, 0);
    if (!CdRead(1, p, 128)) return 2;
    before = hal_clock_ticks();
    if (CdReadSync(1, 0) <= 0 || hal_clock_ticks() != before) return 3;
    for (i=0;i<1000000 && CdReadSync(1,0)>0;i++) {}
    if (i == 1000000 || hal_clock_ticks() == before || !hal_clock_vblanks()) return 4;
    /* A regular poll after a VBlank costs libcd's measured CPU work only
     * (377 ticks while the command runs), no artificial poll quantum. */
    CdControlF(9, 0);
    cd_hal_vblank_tick();
    before = hal_clock_ticks();
    CdSync(1, 0);
    if (hal_clock_ticks() != before + 377u) return 5;
    for (i=0;i<1000000 && CdSync(1,0)!=2;i++) {}
    return i == 1000000 ? 6 : 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'test.c').write_text(fixture)
            run(['gcc', '-std=gnu17', '-fpermissive', '-w', '-O2', '-fno-pie', '-no-pie', '-ffunction-sections',
                 '-fdata-sections', '-I', str(ROOT), '-I', str(ROOT / 'pc/include'),
                 '-I', str(ROOT / 'pc/include/game'), str(p / 'test.c'),
                 '-Wl,--gc-sections', '-lm', '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()



CPU_FIXTURE = r'''#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include "pc/hal/cd.c"
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
void hal_vblank_interrupt(void) {}
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) {}
void hal_overlay_set_loaded(const char *p) {}
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
int main(void) {
    u8 loc[4] = {0,2,0,0}, mode = 0x80;
    u32 *p;
    u64 before, sector;
    p = mmap((void *)0x80134000UL, 4096, PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    assert(p != MAP_FAILED);
    /* The CD queue worker's calls, retail entry-to-return (CdControlB
     * 30,764..30,967, CdControl Getstat 30,643, CdSync(1) 493). */
    before = hal_clock_ticks();
    CdControlB(0x0e, &mode, 0);
    assert(hal_clock_ticks() - before == 113u + 440u + 25000u + 3800u + 960u + 449u);
    before = hal_clock_ticks();
    CdControl(1, 0, 0);
    assert(hal_clock_ticks() - before == 90u + 440u + 25000u + 3800u + 960u + 92u);
    before = hal_clock_ticks();
    assert(CdSync(1, 0) == 2);
    assert(hal_clock_ticks() - before == 493u);
    /* CdRead's own Setmode takes CD_init/CdRead's short path. */
    CdControlB(2, loc, 0);
    before = hal_clock_ticks();
    assert(CdRead(1, p, 0x80));
    assert(hal_clock_ticks() - before == 1275u + 2u * (440u + 25000u + 3800u));
    /* The last sector's interrupt issues CdlPause and waits for its
     * acknowledge inside the handler: the interrupted CPU work loses
     * 4,845 + 25,000 + 2,661 ticks, and the Pause is acknowledged
     * 4,845 + 25,000 after the sector. */
    while (g_drive != DRIVE_READING || !g_xfer_active) advance_to(g_drive_at);
    sector = g_drive_at;
    before = hal_clock_ticks();
    hal_cpu_spend((u32)(sector - before) + 10u);
    assert(!g_xfer_active);
    assert(hal_clock_ticks() - before == (sector - before) + 10u + 4845u + 25000u + 2661u);
    assert(!g_cmd_pending && g_second_cmd == CMD_PAUSE);
    return 0;
}
'''


class CdCpuTests(unittest.TestCase):
    def test_libcd_wrappers_and_read_end_pause_spend_retail_cpu_time(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'test.c').write_text(CPU_FIXTURE)
            run(['gcc', '-std=gnu17', '-fpermissive', '-w', '-O2', '-fno-pie', '-no-pie', '-ffunction-sections',
                 '-fdata-sections', '-I', str(ROOT), '-I', str(ROOT / 'pc/include'),
                 '-I', str(ROOT / 'pc/include/game'), str(p / 'test.c'),
                 '-Wl,--gc-sections', '-lm', '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()


IRQ_FIXTURE = r'''#define _GNU_SOURCE
#include <assert.h>
#include "pc/hal/cd.c"
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
static int normal, absorbed, accepts = 1;
/* as pc/hal/system.c: the libetc chain ran, its exception is charged */
void hal_vblank_interrupt(void) { normal++; hal_cpu_interrupt_occupancy(24449u); }
int hal_vblank_absorbed(void) { absorbed++; return 1; }
int hal_irq_accepts(u16 bits) { return accepts && (bits & 0x000c); }
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) {}
void hal_overlay_set_loaded(const char *p) {}
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
/* an exception entered `lead` ticks before the next edge */
static u64 enter_before_edge(u32 lead, u32 exit_at) {
    u64 entry = g_next_vblank - lead;
    advance_to(entry);
    assert(hal_irq_exception(0x0004, 0, 0, exit_at));
    return entry;
}
int main(void) {
    u64 entry, before;
    u8 mode = 0x80;
    /* After the kernel's verifier (entry + 409) and before trapIntr's last
     * mask read (return - 389): trapIntr takes the VBlank, the kernel's
     * level never runs, and the exception lasts 2,800 longer. */
    entry = enter_before_edge(2000u, 4559u);
    assert(absorbed == 1 && normal == 0);
    assert(g_now == entry + 4559u + 2800u);
    /* VSync(0) waiting on that edge resumes 129 after the return. */
    cd_hal_vblank_tick();
    assert(g_now == entry + 4559u + 2800u + 129u);
    /* ...and the next VSync(0) waits for the next edge. */
    cd_hal_vblank_tick();
    assert(normal == 1 && g_now == g_next_vblank - TICKS_PER_FRAME + 24291u);
    /* Pending at the verifier: both levels, inside the exception. */
    entry = enter_before_edge(300u, 4559u);
    assert(absorbed == 1 && normal == 2);
    assert(g_now == entry + 4559u + 22516u);
    /* Raised after the last mask read: a new exception runs both levels. */
    entry = enter_before_edge(4300u, 4559u);
    assert(absorbed == 1 && normal == 3 && g_now == entry + 4559u + 22516u);
    /* Interrupts off: no exception, no time. */
    accepts = 0;
    before = g_now;
    assert(!hal_irq_exception(0x0004, 0, 0, 4559u) && g_now == before);
    accepts = 1;
    /* A two-response command waits out its completion interrupt:
     * CdControlB(Pause) on an idle drive, 7,400 after the acknowledge,
     * returns 4,616 + 449 after that. */
    advance_to(g_next_vblank + 100000u);
    before = hal_clock_ticks();
    CdControlB(9, 0, 0);
    assert(hal_clock_ticks() - before == 113u + 440u + 25000u + 7400u + 4616u + 449u);
    return 0;
}
'''


class IrqExceptionTests(unittest.TestCase):
    def test_other_interrupts_absorb_a_vblank_after_the_kernel_verifier(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'test.c').write_text(IRQ_FIXTURE)
            src = os.environ.get('CD_CLOCK_ROOT', str(ROOT))
            run(['gcc', '-std=gnu17', '-fpermissive', '-w', '-O2', '-fno-pie', '-no-pie', '-ffunction-sections',
                 '-fdata-sections', '-I', src, '-I', str(ROOT / 'pc/include'),
                 '-I', str(ROOT / 'pc/hal'),
                 '-I', str(ROOT / 'pc/include/game'), str(p / 'test.c'),
                 '-Wl,--gc-sections', '-lm', '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()


CLOCK_FIXTURE = r'''#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include "pc/hal/cd.c"
const FileLbaEntry g_file_lba_table[1] = {{0, 0, 0}};
const u32 g_file_lba_table_count = 0;
const FileLbaEntry *file_id_lookup_lba(u32 n) { return 0; }
void hal_vblank_interrupt(void) {}
u64 memcard_next_event(void) { return (u64)-1; }
void memcard_complete_event(void) {}
void hal_overlay_set_loaded(const char *p) {}
u64 spu_transfer_next_event(void) { return (u64)-1; }
void spu_transfer_complete_event(void) {}
void nbn_trace_register_load(const char *p, const void *d, u32 a, u32 n) {}
int main(int argc, char **argv) {
    if (argv[1][0] == 'j') {
        /* the seek's settling term is the midpoint of the 0.5..1 ms a
         * drive varies by, the same for every seek */
        assert(seek_jitter() == SEEK_JITTER_TICKS);
        assert(seek_jitter() == SEEK_JITTER_TICKS);
        assert(seek_jitter() == SEEK_JITTER_TICKS);
    } else if (argv[1][0] == 'm') {
        assert(seek_jitter() == SEEK_JITTER_TICKS);
        assert(seek_jitter() == SEEK_JITTER_TICKS);
    } else if (argv[1][0] == 'p') {
        /* 263 x 3,413 dot clocks a frame, 566,203.775 CPU ticks */
        assert(vbl_edge(0) == TICKS_PER_FRAME);
        assert(vbl_edge(1000) - vbl_edge(0) == 566203775u);
        boot_vblank_phase();
        assert(g_next_vblank == 163773u);
        assert(hal_clock_edge_at_or_before(163772u) == 0);
        assert(hal_clock_edge_at_or_before(163773u + 566202u) == 163773u);
        assert(hal_clock_edge_at_or_before(163773u + 566203u) == 163773u + 566203u);
        assert(hal_clock_edge_at_or_before(163773u + 566203775u) ==
               163773u + 566203775u);
    } else {
        assert(vbl_edge(1000) - vbl_edge(0) == 1000u * TICKS_PER_FRAME);
    }
    return 0;
}
'''


class ClockModelTests(unittest.TestCase):
    """The seek's settling term is a deterministic midpoint; VBlank edges
    fall at the exact NTSC period (negative controls too)."""
    def exercise(self, mode, env=None):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'test.c').write_text(CLOCK_FIXTURE)
            run(['gcc', '-std=gnu17', '-fpermissive', '-w', '-O2', '-fno-pie', '-no-pie',
                 '-ffunction-sections', '-fdata-sections',
                 '-I', str(ROOT), '-I', str(ROOT / 'pc/include'), '-I', str(ROOT / 'pc/hal'),
                 '-I', str(ROOT / 'pc/include/game'), str(p / 'test.c'),
                 '-Wl,--gc-sections', '-lm', '-o', str(p / 'test')]).require_ok()
            e = {k: v for k, v in os.environ.items()
                 if k not in ('CD_SEEK_JITTER', 'VBLANK_EXACT_PERIOD', 'BOOT_VBLANK_PHASE')}
            e.update(env or {})
            run([str(p / 'test'), mode], env=e).require_ok()

    def test_seek_jitter_is_the_deterministic_midpoint(self):
        self.exercise('j')

    def test_seek_jitter_ignores_the_retired_switch(self):
        self.exercise('m', {'CD_SEEK_JITTER': 'mid'})

    def test_vblank_period_and_boot_phase(self):
        self.exercise('p')

    def test_vblank_period_negative_control(self):
        self.exercise('w', {'VBLANK_EXACT_PERIOD': '0'})


if __name__ == '__main__':
    unittest.main()
