# SPDX-License-Identifier: GPL-3.0-or-later
"""audit_ps1_wide_access: 8-byte accesses to PS1 cells, typed from DWARF.

A fixture is compiled -O2 -fno-pie the way the port compiles the game:
  * `extern u8 *h_img, *h_hdr;` stores of a PS1 address -- POINTER rows
    (the bust-up class);
  * a `long` cell -- LONG;
  * a struct of four u16 members written together, which GCC merges into
    one 64-bit store -- MERGE (benign: the console writes the same bytes);
  * the PS1-width spelling (`extern u32 h_img;`) -- no row (the fix, and
    the negative control for the detector);
  * `extern u8 *names[]; ... names[i]`: GCC reaches the element through a
    lea'd base (`lea names(%rip),%rax; mov (%rax,%rdx,8),%rsi`) -- a
    WIDE-INDEX POINTER row found by following the lea (date_name_set);
    the u32 spelling is the negative control;
  * two adjacent u32 constants stored through a u32 view of a pointer
    table, merged by GCC into one `movabs $hi:lo; mov %rax, tbl` --
    MERGE (the generated overlay tables), where a pointer constant store
    stays POINTER.
"""
import os
import subprocess
import tempfile
import unittest

import audit_ps1_wide_access as wide

FIXTURE = r"""
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef struct { u16 a, b, c, d; } Quad;
extern u8 *h_img;
extern u8 *h_hdr;
extern long h_long;
extern Quad quad;
extern u8 tbl[];
void wide_writer(void) {
    h_img = tbl;
    h_hdr = tbl + 8;
    h_long = 5;
}
void merged_writer(void) {
    quad.a = 1; quad.b = 2; quad.c = 3; quad.d = 4;
}
u8 *wide_reader(void) { return h_img; }
extern u8 *names[];
u8 *pick(int i) { return names[i]; }
extern void *ptbl[];
void pair_writer(void) {
    *(u32 *)((u32 *)ptbl + 0) = 0x801C6000u;
    *(u32 *)((u32 *)ptbl + 1) = 0x801AE000u;
}
void ptr_writer(void) { ptbl[2] = (void *)0x801C6000UL; }
"""
NARROW = r"""
typedef unsigned char u8;
typedef unsigned int u32;
extern u32 h_img;
extern u32 h_hdr;
extern u8 tbl[];
void narrow_writer(void) {
    h_img = (u32)(unsigned long)tbl;
    h_hdr = (u32)(unsigned long)(tbl + 8);
}
u32 narrow_reader(void) { return h_img; }
extern u32 names[];
u8 *pick(int i) { return (u8 *)(unsigned long)names[i]; }
"""
# PS1 layout: four-byte cells back to back
ADDRS = {'h_img': 0x8012DD14, 'h_hdr': 0x8012DD18, 'h_long': 0x8012DD1C,
         'nxt': 0x8012DD20, 'quad': 0x8012DD30, 'quad_c': 0x8012DD34,
         'tbl': 0x800D0000, 'names': 0x800B9AE0, 'ptbl': 0x8015CF60}
STARTS = sorted(set(ADDRS.values()))


class Fixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.rows = {}
        # 'pie': GCC's default position-independent code, where an indexed
        # element is reached through a lea'd base register
        for name, src, pie in (('wide', FIXTURE, '-fno-pie'), ('narrow', NARROW, '-fno-pie'),
                               ('wide_pie', FIXTURE, '-fpie'), ('narrow_pie', NARROW, '-fpie')):
            c = os.path.join(cls.tmp.name, name + '.c')
            o = os.path.join(cls.tmp.name, name + '.o')
            with open(c, 'w') as fh:
                fh.write(src)
            argv = ['gcc', '-O2', pie, '-c', c, '-o', o]
            subprocess.run(argv, check=True)
            rows = wide.scan_object(o, set(ADDRS), ADDRS, STARTS)
            wide.type_rows('pc/build', name + '.o', rows, argv=argv)
            cls.rows[name] = rows

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def by(self, which, cls, name):
        return [r for r in self.rows[which] if r['cls'] == cls and r['name'] == name]

    def test_pointer_stores_and_loads(self):
        for name in ('h_img', 'h_hdr'):
            rows = self.by('wide', 'WIDE-STORE', name)
            self.assertEqual([r['type'] for r in rows], ['POINTER'], name)
            self.assertEqual(rows[0]['func'], 'wide_writer')
            self.assertEqual(rows[0]['cell'], 4)
        rows = self.by('wide', 'WIDE-LOAD', 'h_img')
        self.assertEqual([(r['type'], r['func']) for r in rows],
                         [('POINTER', 'wide_reader')])

    def test_long(self):
        self.assertEqual([r['type'] for r in self.by('wide', 'WIDE-STORE', 'h_long')],
                         ['LONG'])

    def test_merged_member_stores_are_benign(self):
        rows = self.by('wide', 'WIDE-STORE', 'quad')
        self.assertTrue(rows)
        self.assertEqual({r['type'] for r in rows}, {'MERGE'})
        self.assertFalse(wide.gated(dict(rows[0], live=True, obj='w.o'), set()))

    def test_ps1_width_spelling_has_no_rows(self):
        """Negative control: u32 cells make 4-byte accesses."""
        self.assertEqual(self.rows['narrow'], [])

    def test_index_is_wide_index(self):
        for which in ('wide', 'wide_pie'):
            rows = self.by(which, 'WIDE-INDEX', 'names')
            self.assertEqual([(r['type'], r['func']) for r in rows], [('POINTER', 'pick')], which)
            self.assertTrue(wide.gated(dict(rows[0], live=True, obj='w.o'), set()))
        # position-independent code reaches it through the lea'd base
        via = self.by('wide_pie', 'WIDE-INDEX', 'names')[0]['via']
        self.assertTrue(via and via.split(': ', 1)[1].startswith('lea'), via)

    def test_narrow_index_has_no_rows(self):
        """Negative control: `extern u32 names[]` strides 4"""
        self.assertEqual(self.by('narrow_pie', 'WIDE-INDEX', 'names'), [])
        self.assertEqual(self.by('narrow', 'WIDE-INDEX', 'names'), [])

    def test_constant_pair_is_merge(self):
        for which in ('wide', 'wide_pie'):
            pair = self.by(which, 'WIDE-STORE', 'ptbl')
            self.assertEqual({(r['func'], r['type']) for r in pair},
                             {('pair_writer', 'MERGE'), ('ptr_writer', 'POINTER')}, which)

    def test_gate_and_allow(self):
        r = dict(self.by('wide', 'WIDE-STORE', 'h_img')[0], live=True, obj='w.o')
        self.assertTrue(wide.gated(r, set()))
        self.assertFalse(wide.gated(r, {'wide_writer'}))
        self.assertFalse(wide.gated(r, {'w.o:wide_writer'}))
        self.assertFalse(wide.gated(dict(r, live=False), set()))


class Registers(unittest.TestCase):
    def test_wide_register(self):
        self.assertTrue(wide.wide_register('mov %rax,0x0(%rip)        # 1e <f+0x1e>'))
        self.assertTrue(wide.wide_register('movq $0x0,0x0(%rip)        # 21 <f+0x21>'))
        self.assertFalse(wide.wide_register('mov %eax,0x0(%rip)'))
        self.assertFalse(wide.wide_register('movq %xmm0,0x0(%rip)'))
        self.assertFalse(wide.wide_register('movl $0x1,0x0(%rip)'))


BUILD = os.path.join(wide.ROOT, 'pc/build/obj/.srcmap')


@unittest.skipUnless(os.path.exists(BUILD), 'needs a PC build (make -f pc/Makefile all)')
class BuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = wide.audit('pc/build')
        cls.allow = wide.load_allow(os.path.join(wide.ROOT, 'pc/tools/wide_access_allow.txt'))

    def test_fixed_writers_store_ps1_width(self):
        fixed = {'cal_class_init', 'telephone_choice_menu_init', 'konami_mark_set',
                 'yazirushi_disp_switch', 'serifu_run_end', 'graph_init',
                 'bust_up_ptr_set', 'bustup_addr_set', 'bustup_addr_set2',
                 'bustup_addr_set3', 'card_file_write', 'xa_wait_m',
                 'config_screen_init', 'date_name_set', 'dec_bg_load_check',
                 'bustup_check_init', 'bustup_load_step', 'zyotai_sprite_set_chk',
                 'album_info_disp', 'holiday_screen_init', 'holiday_club_select',
                 'join_club_message', 'magazine_main', 'SetRCnt', 'GetRCnt'}
        bad = [r for r in self.rows if r['func'] in fixed and r['live']
               and r['type'] in wide.DEFECT_TYPES]
        self.assertEqual(bad, [])

    def test_gate(self):
        bad = ['%s %s %s' % (r['func'], r['cls'], r['name'])
               for r in self.rows if wide.gated(r, self.allow)]
        self.assertEqual(bad, [])


class ReachNaked(unittest.TestCase):
    """--build-reach never instruments a naked function.  GCC still puts
    `lea this_fn,%rdi; call __cyg_profile_func_enter` in front of a naked
    body, so the BIOS trampoline `naked func_800B4880() { jmp strlen }`
    became strlen(its own address) in the reach binary (SHOUGATU's dialogue
    at scene 91 laid out a 20-byte name; plain vs reach traces differed
    from game frame 10,161).  Negative control: plain -finstrument-functions
    instruments the trampoline."""
    SRC = ('__attribute__((naked)) void tramp(void) { __asm__("jmp target"); }\n'
           'int normal(int x) { return x + 1; }\n')

    def compile(self, flags):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, 't.c')
            with open(src, 'w') as fh:
                fh.write(self.SRC)
            obj = os.path.join(d, 't.o')
            cmd = ['gcc', '-O2', '-c', src, '-o', obj]
            subprocess.run(['gcc', '-O2', '-c', src, '-o', obj] + list(flags(cmd)), check=True)
            return subprocess.run(['objdump', '-dr', obj], capture_output=True, text=True).stdout

    @staticmethod
    def body(dis, name):
        out, on = [], False
        for line in dis.splitlines():
            if line.endswith('<%s>:' % name):
                on = True
            elif on and line.endswith('>:'):
                break
            elif on:
                out.append(line)
        return '\n'.join(out)

    def test_reach_flags_skip_naked(self):
        dis = self.compile(wide.reach_flags)
        self.assertNotIn('__cyg_profile_func_enter', self.body(dis, 'tramp'))
        self.assertIn('__cyg_profile_func_enter', self.body(dis, 'normal'))

    def test_coverage_report_reach_skips_naked(self):
        """coverage_report.py build-reach (the coverage suite's and the
        explorer's reach binary) used a bare -finstrument-functions until
        2026-11-01; it now takes the same flags"""
        import coverage_report
        dis = self.compile(coverage_report.object_flags)
        self.assertNotIn('__cyg_profile_func_enter', self.body(dis, 'tramp'))
        self.assertIn('__cyg_profile_func_enter', self.body(dis, 'normal'))

    def test_plain_instrumentation_hits_naked(self):
        dis = self.compile(lambda cmd: ('-finstrument-functions',))
        self.assertIn('__cyg_profile_func_enter', self.body(dis, 'tramp'))


if __name__ == '__main__':
    unittest.main()
