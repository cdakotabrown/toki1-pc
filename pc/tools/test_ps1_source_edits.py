# SPDX-License-Identifier: GPL-3.0-or-later
"""pc/tools/ps1_source_edits.py: the call-argument and record-width plans.

Plans: every entry still fits its matched source (a stale entry would stop
the build), keeps the line count, and names an existing file.

CallArgs (negative control for every call_args_plan.json entry): the
matched source, audited against the callee's definition, is SHORT-READ
(audit_ps1_call_args.py); the edited source is clean.

RecordWidth (runtime negative controls): the function is compiled as
pc/Makefile compiles it, linked with a 2 MB PS1 RAM at 0x80000000, and
run on PS1-layout records:
  GsTMDdivF4L     the OT bucket address lands at +20 of the divide work
                  area, read from org at +4 of a 20-byte GsOT;
  get_dstack2     the coordinate cells of the 40-byte matrix-stack records
                  at GsDSTACK (+4, stride 40), the chain walked through
                  the 4-byte super link at +32;
  bustup_sprite_init's frame-table cell is read 4 bytes wide (the word
                  after it is not part of the pointer).
The matched source gets each one wrong.

MsgStrDisp (record_width_plan.json main/msg_str_disp): a 530-byte message
with Rei's epilogue's line lengths runs 19 bytes past buf[512] into tmpBuf,
as in the console's frame, and its last line is drawn from there; the
matched source's GCC frame aborts (stack protector).  The frame is
poisoned first, at 16 alignments, so the result does not depend on what
the stack held.

ReturnPassThrough (runtime negative control for return_plan.json): each
TmdXX dispatcher, compiled as pc/Makefile compiles it, takes each of its
six GsNDIV / lighting paths to a stub builder that returns its own
cursor; the result must be that cursor.  The matched source is void and
gcc pops a stack argument into %rax after the call, so the console's
pass-through is lost on some path (GsSortObject4's packet cursor became
the OT header address in TACO, ev_final_taco98).

VarArgs (runtime negative control for varargs_plan.json): the in-image
sprintf, compiled as pc/Makefile compiles it with pc/src/ps1_varargs.c,
formats two %s arguments in PS1 RAM, an int, a hex word and a host-stack
string; the matched source faults or prints garbage after the first.
"""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps1_source_edits as pe  # noqa: E402
import audit_ps1_call_args as ca  # noqa: E402
import audit_ps1_pointer_casts as apc  # noqa: E402

ROOT = Path(pe.ROOT)
from pc_paths import DECOMP, decomp, spell  # noqa: E402
HAVE_CLANG = shutil.which(apc.CLANG)


def source_of(key, entry):
    """the entry's source, spelled for this checkout (pc_paths.spell)"""
    if 'source' in entry:
        return spell(entry['source'])
    ov, fn = key.split('/', 1)
    return decomp('src/overlay/%s/%s.c' % (ov, fn))



def landed_spelling(ov, text):
    """TEXT with every D_XXXXXXXX that names a landed data symbol of overlay
    OV (or of main) respelled with that name: the fixtures quote address
    names, the tree carries the landed names (config/symbol_names.json)."""
    import json as _j
    import re
    doc = _j.loads((DECOMP / 'config' /
                    'symbol_names.json').read_text())['symbols']
    names = {}
    for key, e in doc.items():
        scope, _, a = key.rpartition(':')
        if e.get('kind') == 'data' and e.get('landed') and scope in ('', ov):
            names['D_%08X' % int(a, 16)] = e['name']
    return re.sub(r'\bD_[0-9A-Fa-f]{8}\b',
                  lambda m: names.get('D_' + m.group(0)[2:].upper(), m.group(0)), text)

class Plans(unittest.TestCase):
    def test_entries_fit(self):
        for name, plan in pe.load_plans():
            for key, entry in plan['functions'].items():
                with self.subTest(plan=name, key=key):
                    src = ROOT / source_of(key, entry)
                    self.assertTrue(src.exists())
                    text = src.read_text(errors='replace')
                    out = pe.apply_entry(text, entry)
                    self.assertNotEqual(out, text)
                    self.assertEqual(out.count('\n'), text.count('\n'))

    def test_stale_entry_raises(self):
        entry = {'edits': [['no such text', 'x']]}
        with self.assertRaises(ValueError):
            pe.apply_entry('int a;\n', entry)
        with self.assertRaises(ValueError):
            pe.apply_entry('a a\n', {'edits': [['a', 'b']]})       # count 2, plan 1
        with self.assertRaises(ValueError):
            pe.apply_entry('a\n', {'edits': [['a', 'b\n']]})       # line count

    def test_main_sources_listed(self):
        srcs = pe.main_entries()
        self.assertIn(decomp('src/main/matched/SsSepStop.c'), srcs)
        self.assertIn(decomp('src/main/matched/GsTMDdivF4L.c'), srcs)


@unittest.skipUnless(HAVE_CLANG, 'needs clang')
class CallArgs(unittest.TestCase):
    def short(self, caller, callee_srcs):
        defs, calls = {}, []
        for path in list(callee_srcs) + [caller]:
            d, cs, err = ca.scan_source(['gcc'] + apc.GAME_FLAGS, str(path), str(path))
            self.assertIsNone(err, path)
            defs.update(d)
            if path == caller:
                calls += cs
        return [r for r in ca.judge(calls, defs.get) if r['shape'] == 'SHORT-READ']

    def test_every_entry(self):
        plan = dict(pe.load_plans())['call_args_plan.json']
        with tempfile.TemporaryDirectory() as d:
            for key, entry in plan['functions'].items():
                with self.subTest(key=key):
                    src = source_of(key, entry)
                    callees = [spell(c) for c in entry['callee_src']]
                    self.assertTrue(self.short(src, callees), 'matched source not SHORT-READ')
                    edited = Path(d) / os.path.basename(src)
                    edited.write_text(pe.apply_entry((ROOT / src).read_text(errors='replace'),
                                                     entry))
                    rows = self.short(str(edited), callees)
                    self.assertEqual(rows, [], rows)


def edited_copy(key, d):
    entry = None
    for _name, plan in pe.load_plans():
        entry = plan['functions'].get(key, entry)
    src = ROOT / spell(entry['source'])
    out = Path(d) / src.name
    text = src.read_text(errors='replace')
    for _name, plan in pe.load_plans():
        if key in plan['functions']:
            text = pe.apply_entry(text, plan['functions'][key])
    out.write_text('#line 1 "%s"\n' % entry['source'] + text)
    return out


RAM_HEAD = r"""
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned int u32;
#define W(a) (*(volatile u32 *)(unsigned long)(a))
static int map_ram(void)
{
    void *r = mmap((void *)0x80000000UL, 0x200000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return r == (void *)0x80000000UL;
}
"""

TMD_HARNESS = RAM_HEAD + r"""
int RotAverageNclip4(void *p0, void *p1, void *p2, void *p3, int *a0, int *a1,
                     int *a2, int *a3, int *x1, int *x2, int *x0)
{ *x1 = 0; *x2 = 40; *x0 = 0; return 1; }
void ReadSZfifo4() {}
void NormalColorCol() {}
void *RCpolyF4A(void *out) { return out; }
void *GsTMDdivF4L();
int main(void)
{
    if (!map_ram()) return 2;
    W(0x80100000) = 1;            /* GsOT: length */
    W(0x80100004) = 0x80110000;   /*       org    */
    W(0x80100008) = 0;            /*       offset */
    W(0x8010000C) = 0;            /*       point  */
    W(0x80100010) = 0x80112345;   /*       tag (a following word) */
    memset((void *)0x80100100UL, 0, 32);   /* one GsPOLY, vertex 0 x4 */
    memset((void *)0x80100200UL, 0, 64);   /* vertices */
    memset((void *)0x80100300UL, 0, 16);   /* normal table */
    GsTMDdivF4L((void *)0x80100100UL, (void *)0x80100200UL, (void *)0x80100300UL,
                (void *)0x80130000UL, 1, 2, (void *)0x80100000UL, (void *)0x80120000UL);
    printf("%08x %08x\n", W(0x80120014), W(0x80120018));
    return 0;
}
"""

DSTACK_HARNESS = RAM_HEAD + r"""
int get_dstack2();
int main(void)
{
    int n;
    if (!map_ram()) return 2;
    W(0x80100020) = 0x80100040;   /* A.super = B (+32) */
    W(0x80100024) = 0x80100080;   /* A.sub (+36) */
    W(0x80100060) = 0;            /* B.super = 0 */
    W(0x80100064) = 0x801000C0;   /* B.sub */
    n = get_dstack2((void *)0x80100000UL);
    printf("%d %08x %08x\n", n, W(0x801317A4), W(0x801317CC));
    return 0;
}
"""

BUSTUP_HARNESS = RAM_HEAD + r"""
STUBS
void bustup_sprite_init();
int main(void)
{
    if (!map_ram()) return 2;
    memset((void *)0x80100000UL, 0, 0x1000);
    W(0x80100100) = 0x80100200;   /* the frame-table cell (arg2[0]) */
    W(0x80100104) = 0x8010FFFF;   /* the next word: not part of it */
    *(short *)0x80100200UL = 0;   /* p[0] < 6: no sprite set-up */
    bustup_sprite_init(0, (void *)0x80100000UL, (void *)0x80100100UL, 0, 0);
    printf("ok\n");
    return 0;
}
"""


SHUGAKU_FIXTURE = r"""
typedef unsigned int u32;
u32 D_8013CA98[68];
unsigned char date_place = 1, now_sub_sub_step = 3, D_8013CA94;
unsigned char row[1] = {8}, *rows[1] = {row}, **ss_serifu_shugaku_kind = rows;
short now_serifu_shugaku, now_serifu_shugaku_in;
static int called, sent;
void menu2_main(int a) { called = 2; }
void menu3_main(int a) { called = 3; }
void func_801385EC(int a) { called = 0x85EC; }
void device_cmd_send(int a) { sent = a; }
void *overlay_resolve(u32 a)
{
    return a == 0x8008337Cu ? (void *)menu2_main : a == 0x80083178u ? (void *)menu3_main
         : a == 0x801385ECu ? (void *)func_801385EC : (void *)0;
}
void shugaku_dispatch4_menu(void);
int main(void)
{
    int i;
    for (i = 0; i < 68; i++) D_8013CA98[i] = 0x801385ECu;
    D_8013CA98[3] = 0x8008337Cu;           /* menu2_main: swapped to menu3 */
    shugaku_dispatch4_menu();
    if (called != 3) return 1;
    date_place = 0; now_sub_sub_step = 5;  /* func_801385EC: the script byte 8 */
    shugaku_dispatch4_menu();
    return called == 0x85EC && sent == 519 && D_8013CA94 == 1 ? 0 : 1;
}
"""


class FunctionCells(unittest.TestCase):
    def test_shugaku_dispatch4_menu(self):
        """SHUGAKU shugaku_dispatch4_menu calls one 4-byte cell of its copied word
        table and compares cells with functions.  Negative control: the
        matched source calls two cells as one pointer (or never matches)."""
        def build(src, d):
            p = Path(d)
            (p / 'f.c').write_text(src)
            (p / 'x.c').write_text(landed_spelling('SHUGAKU', SHUGAKU_FIXTURE))
            r = run_sh(['gcc', '-std=gnu89', '-w', '-no-pie', '-I', str(DECOMP / 'include'),
                        str(p / 'f.c'), str(p / 'x.c'), '-o', str(p / 'run')])
            self.assertTrue(r.ok, r.stdout)
            return run_sh([str(p / 'run')]).ok
        text = (DECOMP / 'src/overlay/SHUGAKU/shugaku_dispatch4_menu.c').read_text(errors='replace')
        entry = dict(pe.load_plans())['function_cells_plan.json']['functions']['SHUGAKU/shugaku_dispatch4_menu']
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(build(pe.apply_entry(text, entry), d))
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(build(text, d))


def run_sh(cmd):
    from safe_process import run
    return run(cmd, timeout=120)


def run_case(func, source, harness_text):
    import test_ps1_pointer_reads as reads
    return reads.build_and_run(func, source, harness_text)


RETURN_HARNESS = RAM_HEAD + r"""
extern int GsNDIV, GsLIOFF, GsLIGNR, GsLIGHT_MODE, GsLMODE;
STUBS
int DISPATCH(int, int, int, int, int, int, int, int);
int main(void)
{
    static const int paths[6][5] = {   /* GsNDIV LIOFF LIGNR LIGHT_MODE LMODE */
        {1, 1, 0, 0, 0}, {1, 0, 0, 1, 0}, {1, 0, 0, 0, 0},
        {0, 1, 0, 0, 0}, {0, 0, 1, 0, 1}, {0, 0, 0, 0, 0}};
    int i;
    if (!map_ram()) return 2;
    for (i = 0; i < 6; i++) {
        GsNDIV = paths[i][0]; GsLIOFF = paths[i][1]; GsLIGNR = paths[i][2];
        GsLIGHT_MODE = paths[i][3]; GsLMODE = paths[i][4];
        printf("%08x\n", (u32)DISPATCH(0x80100000, 0x80101000, 0x80102000,
                                       0x80120000, 3, 6, 0x800F1150, 0x1F800000));
    }
    return 0;
}
"""

TMD_DISPATCHERS = ['TmdF3', 'TmdTF3', 'TmdG3', 'TmdTG3', 'TmdF4', 'TmdTF4', 'TmdG4', 'TmdTG4']


class ReturnPassThrough(unittest.TestCase):
    def results(self, func, source):
        import re
        callees = re.findall(r'^void (\w+)\(\);', (DECOMP / ('src/main/matched/%s.c' % func))
                             .read_text(errors='replace'), re.M)
        self.assertEqual(len(callees), 6, callees)
        # builder k returns the cursor (4th argument) + 4 * (k + 1): the
        # path that was taken, and that its result came back
        stubs = ''.join('long %s(long a, long b, long c, long out) { return out + %d; }\n'
                        % (c, 4 * (k + 1)) for k, c in enumerate(callees))
        ok, out = run_case(func, source, RETURN_HARNESS.replace('STUBS', stubs)
                           .replace('DISPATCH', func))
        self.assertTrue(ok, out)
        return [int(x, 16) for x in out.split()[-6:]]

    def test_dispatchers_return_the_builder_cursor(self):
        want = [0x80120000 + 4 * (k + 1) for k in range(6)]
        for func in TMD_DISPATCHERS:
            with self.subTest(func=func):
                with tempfile.TemporaryDirectory() as d:
                    self.assertEqual(self.results(func, edited_copy('main/' + func, d)), want)

    def test_matched_source_loses_the_cursor(self):
        """negative control: the void dispatchers, compiled as they were,
        hand back some other register value on at least one path"""
        want = [0x80120000 + 4 * (k + 1) for k in range(6)]
        lost = [f for f in TMD_DISPATCHERS
                if self.results(f, 'src/main/matched/%s.c' % f) != want]
        self.assertIn('TmdTG3', lost)


CHAIN_HARNESS = RAM_HEAD + r"""
typedef int s32;
static s32 *pick_edited(s32 tbl, int row, int col) { return EDITED; }
static s32 *pick_matched(s32 ***tbl, int row, int col) { return tbl[row][col]; }
int main(int argc, char **argv)
{
    if (!map_ram()) return 2;
    W(0x80100000) = 0x80100100;   /* row 0 */
    W(0x80100004) = 0x80100200;   /* row 1: a 4-byte PS1 table of rows */
    W(0x80100200) = 0x80100300;   /* row 1, col 0 */
    W(0x80100204) = 0x80100310;   /* row 1, col 1: the entry */
    W(0x80100310) = 7;
    if (argc > 1)                 /* the matched walk (may fault) */
        printf("%lx\n", (unsigned long)pick_matched((s32 ***)0x80100000UL, 1, 1));
    else
        printf("%lx\n", (unsigned long)pick_edited((s32)0x80100000, 1, 1));
    return 0;
}
"""


class PointerChain(unittest.TestCase):
    def test_ending_serifu_cmd_table(self):
        """ENDING ending_serifu_cmd's tbl[row][col]: a PS1 table of 4-byte
        row pointers to 4-byte entry pointers.  The plan's expression picks
        0x80100310; negative control: the matched `s32 ***` walk reads 8
        bytes a level and does not."""
        import re
        entry = dict(pe.load_plans())['record_width_plan.json']['functions']['ENDING/ending_serifu_cmd']
        expr = [e[1] for e in entry['edits'] if e[0].startswith('ent = ')][0]
        expr = re.match(r'ent = (.*);$', expr).group(1)
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'chain.c'
            src.write_text(CHAIN_HARNESS.replace('EDITED', expr))
            exe = Path(d) / 'chain'
            r = run_sh(['gcc', '-O1', '-w', '-no-pie', str(src), '-o', str(exe)])
            self.assertTrue(r.ok, r.stdout)
            r = run_sh([str(exe)])
            self.assertTrue(r.ok, r.stdout)
            self.assertEqual(r.stdout.split()[-1], '80100310')
            r = run_sh([str(exe), 'matched'])
            self.assertFalse(r.ok and r.stdout.split()[-1:] == ['80100310'], r.stdout)

    def test_date2_serifu_prefix_cell(self):
        """DATE2 date2_serifu_prefix_set's `p = *pp`: pp is a PS1 cell whose
        neighbour is another pointer.  Negative control: the matched 8-byte
        read takes the neighbour as the high half."""
        entry = dict(pe.load_plans())['record_width_plan.json']['functions']['DATE2/date2_serifu_prefix_set']
        new = entry['edits'][0][1].strip()            # p = (u8 *)...pp;
        prog = (RAM_HEAD + 'typedef unsigned char u8;\n'
                'int main(int argc, char **argv)\n{\n    u8 **pp = (u8 **)0x80100000UL; u8 *p;\n'
                '    if (!map_ram()) return 2;\n'
                '    W(0x80100000) = 0x80100100; W(0x80100004) = 0x80100200;\n'
                '    if (argc > 1) p = *pp; else ' + new + '\n'
                '    printf("%lx\\n", (unsigned long)p);\n    return 0;\n}\n')
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'cell.c'
            src.write_text(prog)
            exe = Path(d) / 'cell'
            r = run_sh(['gcc', '-O1', '-w', '-no-pie', str(src), '-o', str(exe)])
            self.assertTrue(r.ok, r.stdout)
            self.assertEqual(run_sh([str(exe)]).stdout.split()[-1], '80100100')
            self.assertNotEqual(run_sh([str(exe), 'matched']).stdout.split()[-1], '80100100')


SPU_AREA_HARNESS = r"""
int spu_mem_mode = 1;
int _SpuIsInAllocateArea(int addr) { return addr == 0x2000; }
/* the reach binary's exit hook: returns with its own %eax */
int __cyg_profile_func_enter(void *f, void *c) { (void)f; (void)c; return 0x5a5a; }
int __cyg_profile_func_exit(void *f, void *c) { (void)f; (void)c; return 0x5a5a; }
int _SpuIsInAllocateArea_(int a0);
#include <stdio.h>
int main(void) { printf("%d %d\n", _SpuIsInAllocateArea_(0x1000), _SpuIsInAllocateArea_(0x800)); return 0; }
"""


class ReturnPassThroughSpu(unittest.TestCase):
    def results(self, text):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'f.c'
            f.write_text(text)
            h = Path(d) / 'h.c'
            h.write_text(SPU_AREA_HARNESS)
            exe = Path(d) / 'run'
            r = run_sh(['gcc', '-std=gnu17', '-O2', '-w', '-no-pie', '-include', 'pc/include/game/types.h',
                        '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include'),
                        '-finstrument-functions', '-c', str(f), '-o', str(Path(d) / 'f.o')])
            self.assertTrue(r.ok, r.stdout)
            r = run_sh(['gcc', '-O0', '-w', '-no-pie', str(h), str(Path(d) / 'f.o'), '-o', str(exe)])
            self.assertTrue(r.ok, r.stdout)
            return run_sh([str(exe)]).stdout.split()

    def test_spu_is_in_allocate_area_returns_the_callee_value(self):
        """_SpuIsInAllocateArea_ (void on the console, its v0 is the
        callee's) returns _SpuIsInAllocateArea's 1 / 0 once edited, also in
        an instrumented (reach) compile.  Negative control: the matched void
        wrapper hands back the exit hook's %eax there."""
        entry = dict(pe.load_plans())['return_plan.json']['functions']['main/_SpuIsInAllocateArea_']
        text = (DECOMP / 'src/main/matched/_SpuIsInAllocateArea_.c').read_text(errors='replace')
        self.assertEqual(self.results(pe.apply_entry(text, entry)), ['1', '0'])
        self.assertNotEqual(self.results(text), ['1', '0'])


FAN_HARNESS = RAM_HEAD + r"""
int activeBuff;
u32 ot_buff0[2][256];
static u32 next = 0x80100000;
void *prim_alloc(int n, int buf) { void *p = (void *)(unsigned long)next; (void)buf; next += 28; return p; }
int rsin(int a) { (void)a; return 0; }
int rcos(int a) { (void)a; return 4096; }
void AddPrim(void *ot, void *p) { (void)ot; (void)p; }
void SetSemiTrans(void *p, int f) { (void)p; (void)f; }
void SetShadeTex(void *p, int f) { (void)p; (void)f; }
void SetPolyG3(void *p) { (void)p; }
void taco_fan_disp();
int main(void)
{
    /* taco_burst_disp's `s32 buf[40]` and the word after it */
    static struct { int buf[40]; int after; } f = {{0}, 0x5a5a5a5a};
    int unused;
    if (!map_ram()) return 2;
    taco_fan_disp(f.buf, &unused, 0x808080, 16);
    printf("%x %x %x\n", f.buf[0], f.buf[39], f.after);
    return 0;
}
"""


class FanCells(unittest.TestCase):
    def test_taco_fan_disp_writes_forty_cells(self):
        """TACO taco_fan_disp fills taco_burst_disp's s32 buf[40] with
        forty 4-byte primitive handles.  Negative control: the matched
        `Prim **out` stores 8-byte pointers over the word after the
        buffer (on the stack: the canary)."""
        entry = dict(pe.load_plans())['record_width_plan.json']['functions']['TACO/taco_fan_disp']
        text = (DECOMP / 'src/overlay/TACO/taco_fan_disp.c').read_text(errors='replace')

        def run_with(src):
            with tempfile.TemporaryDirectory() as d:
                f = Path(d) / 'f.c'
                f.write_text(src)
                h = Path(d) / 'h.c'
                h.write_text(FAN_HARNESS)
                exe = Path(d) / 'run'
                r = run_sh(['gcc', '-std=gnu17', '-O1', '-w', '-no-pie', '-I', str(DECOMP / 'include'),
                            str(h), str(f), '-o', str(exe)])
                self.assertTrue(r.ok, r.stdout)
                return run_sh([str(exe)]).stdout.split()
        self.assertEqual(run_with(pe.apply_entry(text, entry)),
                         ['80100000', '%x' % (0x80100000 + 39 * 28), '5a5a5a5a'])
        self.assertNotEqual(run_with(text)[-1:], ['5a5a5a5a'])


ETC_DISPATCH_HARNESS = RAM_HEAD + r"""
/* D_8014EA34.. D_8014EA6C: one PS1 table of 4-byte cells (the link
   aliases them into cells[]); cell k (from D_8014EA38) points at a value
   table in the mapped PS1 RAM, word j = 1000 + 10 k + j */
u32 cells[16];
int D_8014E0E0, D_80150074;
static int last = -1, calls = 0;
int value_cache_find_or_load(int v, int f) { (void)f; last = v; calls++; return 0; }
void etc_menu_dispatch(int a0, int a1, int a2, int a3);
int main(int argc, char **argv)
{
    int k, j;
    if (!map_ram()) return 2;
    for (k = 0; k < 15; k++) {
        cells[1 + k] = 0x80100000u + 0x100 * k;
        for (j = 0; j < 8; j++)
            W(0x80100000u + 0x100 * k + 4 * j) = 1000 + 10 * k + j;
    }
    etc_menu_dispatch(3, 2, 0, 0);       /* cell 3, word 2 */
    printf("%d\n", last);
    calls = 0;
    etc_menu_dispatch(1, 0, 0, 0);       /* slot2 == &D_8014EA3C, idx4 0: no call */
    printf("%d\n", calls);
    return 0;
}
"""
ETC_DISPATCH_SYMS = dict(D_8014EA34=0, D_8014EA38=4, D_8014EA3C=8, D_8014EA60=0x2c,
                         D_8014EA64=0x30, D_8014EA6C=0x38)

OPTION_MENU1_HARNESS = RAM_HEAD + r"""
/* D_8013E7D4: a PS1 table of sixteen 4-byte label pointers (aliased onto
   labels[]); label i is the string "Li" in the mapped PS1 RAM */
u32 labels[16];
short cursor_cw_x, cursor_cw_y, para_tai, para_bun, para_ri, para_gei, para_un,
      para_zat, para_you, para_kon, para_str;
unsigned char screen_w[256], byouki_kind, byouki_state, now_icon;
u32 pad_data;
void menu_check() {}
void menu_cursor_move() {}
void menu_bar_show() {}
void debug_number_show() {}
int option_value_step() { return 0; }
void debug_string_show(const char *s) { printf("%s ", s); }
void option_menu1_main1(void);
int main(void)
{
    int i;
    if (!map_ram()) return 2;
    for (i = 0; i < 16; i++) {
        labels[i] = 0x80100000u + 0x20 * i;
        sprintf((char *)(unsigned long)labels[i], "L%d", i);
    }
    option_menu1_main1();
    printf("\n");
    return 0;
}
"""


class PointerCellTables(unittest.TestCase):
    """audit_ps1_pointer_cells.py PS1-CELL rows fixed through
    record_width_plan.json: each function walks a PS1 table of 4-byte
    pointer cells.  Compiled as a plain C unit with a 2 MB PS1 RAM at
    0x80000000 and the table at its own symbol; the edited source picks
    the console's cells, the matched one (8-byte cells) does not."""

    def run_with(self, src, harness, syms):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'f.c'
            f.write_text(src)
            h = Path(d) / 'h.c'
            h.write_text(harness)
            exe = Path(d) / 'run'
            ld = ['-Wl,--defsym=%s=%s+%d' % (name, base, off) for name, (base, off) in syms.items()]
            r = run_sh(['gcc', '-std=gnu17', '-O1', '-w', '-no-pie', '-I', str(DECOMP / 'include'),
                        str(h), str(f), '-o', str(exe)] + ld)
            self.assertTrue(r.ok, r.stdout)
            return run_sh([str(exe)]).stdout.split()

    def test_etc_menu_dispatch_cells(self):
        """ETC etc_menu_dispatch's general_path: `table = *slot2` with
        slot2 = &D_8014EA38 + a2 picks cell a2 (value 1032 for a2 3, word
        2), and the compare chain recognises the D_8014EA3C cell (a2 1,
        a1 0: no lookup).  Negative control: the matched `s32 **slot2`
        strides 8 and reads two cells as one pointer."""
        entry = dict(pe.load_plans())['record_width_plan.json']['functions']['ETC/etc_menu_dispatch']
        text = (DECOMP / 'src/overlay/ETC/etc_menu_dispatch.c').read_text(errors='replace')
        syms = {landed_spelling('ETC', k): ('cells', v) for k, v in ETC_DISPATCH_SYMS.items()}
        harness = landed_spelling('ETC', ETC_DISPATCH_HARNESS)
        self.assertEqual(self.run_with(pe.apply_entry(text, entry), harness, syms),
                         ['1032', '0'])
        self.assertNotEqual(self.run_with(text, harness, syms)[:1], ['1032'])

    def test_option_menu1_main1_labels(self):
        """OPTION option_menu1_main1 shows the sixteen labels of
        D_8013E7D4.  Negative control: the matched `u8 **p` reads two
        cells as one pointer and strides 8."""
        entry = dict(pe.load_plans())['record_width_plan.json']['functions']['OPTION/option_menu1_main1']
        text = (DECOMP / 'src/overlay/OPTION/option_menu1_main1.c').read_text(errors='replace')
        syms = {'option_menu1_lbl_tbl': ('labels', 0)}
        want = ['L%d' % i for i in range(16)]
        self.assertEqual(self.run_with(pe.apply_entry(text, entry), OPTION_MENU1_HARNESS, syms), want)
        self.assertNotEqual(self.run_with(text, OPTION_MENU1_HARNESS, syms), want)


CONT_DATA_HARNESS = r"""
#include <stdio.h>
#include <string.h>
typedef short s16;
/* the voice manager SsUtGetProgAtr reads: VAB 0, program 2 has 5 tones */
unsigned char _svm_vab_used[16] = {1};
void *hal_svm_pg;
static unsigned char prog_table[16 * 16];
int SpuVmVSetUp(s16 slot, s16 n) { (void)slot; (void)n; hal_svm_pg = prog_table; return 0; }
static int gets;
void SsUtGetVagAtr(s16 a, unsigned char b, int i, unsigned char *s) { (void)a; (void)b; (void)i; memset(s, 0, 24); gets++; }
void SsUtSetVagAtr(s16 a, unsigned char b, int i, unsigned char *s) { (void)a; (void)b; (void)i; (void)s; }
int ReadDeltaValue(s16 ch, s16 seq) { (void)ch; (void)seq; return 0; }
void Snd_setVabAttr() {}
unsigned char *_ss_score[16];
static unsigned char score[168];
void ContDataEntry(int ch, int seq, int val);   /* K&R: promoted */
int main(void)
{
    prog_table[2 * 16 + 0] = 5;          /* tones */
    prog_table[2 * 16 + 6] = 0x34;       /* attr: the halfword at +6 */
    _ss_score[0] = score;
    score[18] = 0;                       /* sub = rec */
    score[44] = 2;                       /* program 2 */
    score[22] = 20; score[41] = 2;       /* the per-tone arm, */
    score[19] = 0; score[20] = 0;        /* loop 1: `count` tones */
    ContDataEntry(0, 0, 0x40);
    printf("%d\n", gets);
    return 0;
}
"""


class ContDataEntryCount(unittest.TestCase):
    def test_program_record_fits_the_count_slot(self):
        """ContDataEntry hands `&count` to SsUtGetProgAtr, which fills an
        8-byte program record (the console's sp+188..195).  Edited: count
        is the first byte of that record and the arm runs its 5 tones,
        clean under ASan and UBSan (alignment).  Negative control: the
        matched `u8 count` takes a 7-byte overflow (ASan stack-buffer-
        overflow) or a misaligned halfword store."""
        if not shutil.which('gcc'):
            self.skipTest('needs gcc')
        entry = dict(pe.load_plans())['record_width_plan.json']['functions']['main/ContDataEntry']
        text = (DECOMP / 'src/main/matched/ContDataEntry.c').read_text(errors='replace')

        def run_with(src):
            with tempfile.TemporaryDirectory() as d:
                f = Path(d) / 'f.c'
                f.write_text(src)
                h = Path(d) / 'h.c'
                h.write_text(CONT_DATA_HARNESS)
                exe = Path(d) / 'run'
                san = ['-fsanitize=address,alignment', '-fno-sanitize-recover=all']
                flags = ['-std=gnu17', '-O2', '-w', '-funsigned-char', '-fno-strict-aliasing',
                         '-D_GNU_SOURCE', '-DPC_PORT', '-include', 'pc/include/game/types.h',
                         '-I', 'pc/include/game', '-I', 'pc/include', '-I', decomp('include')]
                r = run_sh(['gcc'] + flags + san + ['-D_svm_pg=hal_svm_pg', '-c',
                            str(ROOT / 'src/main/pc-port/SsUtGetProgAtr.c'), '-o',
                            str(Path(d) / 'p.o')])
                self.assertTrue(r.ok, r.stdout)
                r = run_sh(['gcc'] + flags + san + ['-c', str(f), '-o', str(Path(d) / 'f.o')])
                self.assertTrue(r.ok, r.stdout)
                r = run_sh(['gcc', '-O1', '-w'] + san + [str(h), str(Path(d) / 'f.o'),
                            str(Path(d) / 'p.o'), '-o', str(exe)])
                self.assertTrue(r.ok, r.stdout)
                return run_sh([str(exe)])
        old = os.getcwd()
        os.chdir(ROOT)
        try:
            r = run_with(pe.apply_entry(text, entry))
            self.assertTrue(r.ok, r.stdout)
            self.assertEqual(r.stdout.split()[-1:], ['5'])
            r = run_with(text)
            self.assertFalse(r.ok and r.stdout.split()[-1:] == ['5'], r.stdout)
            self.assertRegex(r.stdout, 'stack-buffer-overflow|misaligned')
        finally:
            os.chdir(old)


class RecordWidth(unittest.TestCase):
    def test_tmd_div_ot_bucket(self):
        with tempfile.TemporaryDirectory() as d:
            ok, out = run_case('GsTMDdivF4L', edited_copy('main/GsTMDdivF4L', d), TMD_HARNESS)
            self.assertTrue(ok, out)
            tag, after = [int(x, 16) for x in out.split()[-2:]]
            self.assertEqual(tag, 0x80110000 + (40 >> 2) * 4)
        ok, out = run_case('GsTMDdivF4L', 'src/main/matched/GsTMDdivF4L.c', TMD_HARNESS)
        if ok:
            tag = int(out.split()[-2], 16)
            self.assertNotEqual(tag, 0x80110000 + (40 >> 2) * 4)

    def test_dstack_cells(self):
        with tempfile.TemporaryDirectory() as d:
            ok, out = run_case('get_dstack2', edited_copy('main/get_dstack2', d), DSTACK_HARNESS)
            self.assertTrue(ok, out)
            self.assertEqual(out.split()[-3:], ['1', '80100000', '80100040'])
        ok, out = run_case('get_dstack2', 'src/main/matched/get_dstack2.c', DSTACK_HARNESS)
        if ok:
            self.assertNotEqual(out.split()[-3:], ['1', '80100000', '80100040'])

    def test_bustup_frame_cell(self):
        stubs = ''.join('void %s() {}\n' % s for s in (
            'palette_upload_enqueue', 'load_tpage_buf_lock', 'class_clear_n',
            'bustup_face_set', 'bustup_timer_clamp'))
        text = BUSTUP_HARNESS.replace('STUBS', stubs)
        with tempfile.TemporaryDirectory() as d:
            ok, out = run_case('bustup_sprite_init',
                               edited_copy('main/bustup_sprite_init', d), text)
            self.assertTrue(ok, out)
        # the matched source reads 0x8010FFFF_80100200: a fault
        ok, out = run_case('bustup_sprite_init', 'src/main/matched/bustup_sprite_init.c', text)
        self.assertFalse(ok, out)


SPRINTF_HARNESS = RAM_HEAD + r"""
int func_800B4880(const char *s) { return (int)strlen(s); }
void *func_800B56B0(const void *s, int c, int n) { return memchr(s, c, n); }
void func_800B56C0(void *d, const void *s, int n) { memmove(d, s, n); }
#include "VARARGS_C"
int func_800B48D0();   /* unprototyped, as the game declares it */
int main(void)
{
    char out[128], local[16];
    if (!map_ram()) return 2;
    memcpy((char *)0x800E8D4CUL, "0123456789ABCDEF", 16);
    memcpy((char *)0x800E8D60UL, "0123456789abcdef", 16);
    strcpy((char *)0x80100000UL, "Fuji");
    strcpy((char *)0x80100010UL, "Shiori");
    strcpy(local, "host");
    func_800B48D0(out, "%s %s/%d/%x/%-5s|%3d", (char *)0x80100000UL, (char *)0x80100010UL,
                  42, 255, local, 7);
    printf("%s\n", out);
    return 0;
}
"""


# Rei's epilogue as ending_result_disp passes it (ENDING, end_12_rei): 530
# bytes in 33 lines of 6..20 bytes, each but the last ended by a one-byte
# ' ' (the text's own line lengths, measured on the port; the text itself
# is game data and not in the repo).  Line i is filled with the SJIS pair
# 82 (9F + i) and every drawn line is checked.  The last line lies past
# buf[512] -- in tmpBuf on the console -- and is drawn from there.
#
# The first version of this harness passed ONE 530-byte line, so the
# expansion loop's bound (len = pos - start = 530) walked work[64..528] --
# past work into the canary, saved registers and the caller's frame -- and
# whenever one of those even bytes was 0x46 ('F', whose arm copies tmpBuf
# into work[n..]) it wrote 18 bytes over the canary: "stack smashing
# detected" about 1 run in 100.  No game message has a line longer than
# work[64] (RENDER-PARITY-HANDOFF.md, 2026-11-01).  Before each call the
# stack the frame will occupy is filled with 'F', at 16 stack alignments,
# so a read of a frame byte the function did not write takes that arm on
# every run rather than by chance.
MSD_LINES = [14, 12, 16, 16, 18, 16, 12, 16, 18, 20, 16, 10, 20, 16, 18, 12, 16,
             12, 14, 14, 18, 14, 12, 14, 12, 18, 16, 16, 6, 20, 16, 14, 16]

MSD_HARNESS = RAM_HEAD + r"""
#include <stdlib.h>
#include <alloca.h>
void func_800B4890(char *d, const char *s) { strcpy(d, s); }
int func_800B4880(const char *s) { return (int)strlen(s); }
void func_800B48A0(char *d, const char *s) { strcat(d, s); }
char *func_800B4830(char *d, const char *s, int n) { int i = 0; if (n <= 0) return d;
    while (i < n) { char c = s[i]; d[i++] = c; if (!c) break; } while (i < n) d[i++] = 0; return d; }
int func_800B48D0() { return 0; }
static const int lines[] = { @LINES@ };
#define NLINES ((int)(sizeof lines / sizeof lines[0]))
static int drawn, bad;
int set_kanji_string(int x, int y, int color, unsigned char *w, int arg4)
{
    int i;
    if (drawn >= NLINES || (int)strlen((char *)w) != lines[drawn])
        bad++;
    else
        for (i = 0; i < lines[drawn]; i += 2)
            if (w[i] != 0x82 || w[i + 1] != 0x9F + drawn) bad++;
    drawn++;
    return 0;
}
int msg_str_disp();
static char msg[@MSGSIZE@];
static __attribute__((noinline)) void poison(void)
{
    volatile char pad[8192];
    memset((char *)pad, 'F', sizeof pad);
}
static __attribute__((noinline)) int one(int shift)
{
    volatile char *a = alloca(shift + 1);
    a[0] = 0;
    poison();
    return msg_str_disp(0, 0, 0, msg, 0, "", "", 0);
}
int main(void)
{
    int i, j, n = 0, shift;
    if (!map_ram()) return 2;
    for (i = 0; i < NLINES; i++) {
        for (j = 0; j < lines[i]; j += 2) { msg[n++] = (char)0x82; msg[n++] = (char)(0x9F + i); }
        if (i + 1 < NLINES) msg[n++] = @SEP@;
    }
    msg[n] = 0;
    printf("%d bytes\n", n);
    for (shift = 0; shift < 256; shift += 16) {
        drawn = bad = 0;
        one(shift);
        if (drawn != NLINES || bad) {
            printf("shift %d: %d lines, %d bad\n", shift, drawn, bad);
            return 1;
        }
    }
    printf("ok\n");
    return 0;
}
"""


def msd_harness(lines=MSD_LINES, sep="' '"):
    return (MSD_HARNESS.replace('@LINES@', ', '.join(map(str, lines)))
            .replace('@MSGSIZE@', str(sum(lines) + len(lines))).replace('@SEP@', sep))


class MsgStrDisp(unittest.TestCase):
    def test_long_message_stays_in_the_frame(self):
        self.assertEqual(sum(MSD_LINES) + len(MSD_LINES) - 1, 530)
        with tempfile.TemporaryDirectory() as d:
            ok, out = run_case('msg_str_disp', edited_copy('main/msg_str_disp', d), msd_harness())
            self.assertTrue(ok, out)
            self.assertEqual(out.split()[-2:], ['bytes', 'ok'])
            self.assertIn('530 bytes', out)
        # negative control: the matched source's frame (GCC's order, the
        # canary after buf[512]) aborts on the same message
        ok, out = run_case('msg_str_disp', 'src/main/matched/msg_str_disp.c', msd_harness())
        self.assertFalse(ok, out)

    def test_one_line_message_fails_every_run(self):
        """Determinism control: the first harness's message, one 530-byte
        line, makes the expansion loop read work[64..528], outside the
        frame.  It passed about 99 runs in 100; here the drawn line (work
        holds 64 bytes, the console's too) cannot be the 530 bytes asked
        for, so it fails on every run, whatever the stack held."""
        self.assertLess(max(MSD_LINES), 64)
        with tempfile.TemporaryDirectory() as d:
            ok, out = run_case('msg_str_disp', edited_copy('main/msg_str_disp', d),
                               msd_harness([530]))
            self.assertFalse(ok, out)


class VarArgs(unittest.TestCase):
    def test_sprintf_arguments(self):
        text = SPRINTF_HARNESS.replace('VARARGS_C', str(ROOT / 'pc/src/ps1_varargs.c'))
        want = 'Fuji Shiori/42/ff/host |  7'
        with tempfile.TemporaryDirectory() as d:
            ok, out = run_case('sprintf', edited_copy('main/sprintf', d), text)
            self.assertTrue(ok, out)
            self.assertEqual(out.splitlines()[-1], want)
        ok, out = run_case('sprintf', 'src/main/matched/sprintf.c', text)
        self.assertFalse(ok and out.splitlines()[-1:] == [want], out)

    def test_argument_count(self):
        import ctypes
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d) / 'v.so'
            subprocess.run(['gcc', '-shared', '-fPIC', '-o', str(lib),
                            str(ROOT / 'pc/src/ps1_varargs.c')], check=True)
            v = ctypes.CDLL(str(lib))
            v.ps1_va_ptr.restype = ctypes.c_void_p
            v.ps1_va_ptr.argtypes = [ctypes.c_longlong]
            for fmt, n in ((b'abc', 0), (b'%s %s', 2), (b'%%d', 0), (b'%*.*d', 3), (b'%-08lx', 1),
                           (b'%5.2s%c%n', 3), (b'%d %f %d', 1)):
                self.assertEqual(v.ps1_va_count(fmt), n, fmt)
            self.assertEqual(v.ps1_va_ptr(-0x7FF00000), 0x80100000)     # sign-extended PS1
            self.assertEqual(v.ps1_va_ptr(0x7FFD12345678), 0x7FFD12345678)   # host


if __name__ == '__main__':
    unittest.main()
