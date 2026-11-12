# SPDX-License-Identifier: GPL-3.0-or-later
"""pc/tools/measure_cpu_costs.py: entry-gap attribution on synthetic probe
traces, and the table it emits."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_cpu_costs as m  # noqa: E402


def frame(n, fc, step=0x14):
    return (f'frame={n} pad=0000 tick=0 now_step={step:02x} now_sub_step=00 '
            f'now_sub_sub_step=00 now_girl=00 frame_counter={fc.to_bytes(4, "little").hex()}\n')


def pc(label, tick):
    return f'pc={label} tick={tick} frame=0 ra=0 a0=0 a1=0 a2=0 a3=0\n'


class GapTests(unittest.TestCase):
    def run_gaps(self, lines, **kw):
        with tempfile.NamedTemporaryFile('w', suffix='.trace', delete=False) as f:
            f.writelines(lines)
        try:
            return m.gaps(f.name, **kw)
        finally:
            os.unlink(f.name)

    def test_entry_to_next_entry(self):
        st = self.run_gaps([frame(0, 5), pc('a', 100), pc('b', 160), pc('a', 200), pc('c', 230)])
        self.assertEqual(st['a'][:2], [2, 60 + 30])
        self.assertEqual(st['b'][:2], [1, 40])

    def test_exception_time_is_not_the_interrupted_function(self):
        # a: 100..150 work, exception 150..1150 (with a callback inside),
        # back at 1150, b at 1170: a's gap is 50 + 20.
        st = self.run_gaps([frame(0, 5), pc('a', 100), pc('@exc', 150), pc('cb', 400),
                            pc('@rfe', 1150), pc('b', 1170), pc('c', 1200)])
        self.assertEqual(st['a'][:2], [1, 70])
        self.assertNotIn('cb', st)

    def test_inside_symbol_folds_into_its_host(self):
        st = self.run_gaps([frame(0, 5), pc('melt_font', 0), pc('func_8004F91C', 1),
                            pc('x', 4552)], inside={'func_8004F91C'})
        self.assertEqual(st['melt_font'][:2], [1, 4552])

    def test_hal_replacement_absorbs_its_helpers(self):
        st = self.run_gaps([frame(0, 5), pc('SetDrawMode', 0), pc('get_mode', 27),
                            pc('get_tw', 72), pc('AddPrim', 146), pc('get_tw', 200),
                            pc('z', 274)])
        self.assertEqual(st['SetDrawMode'][:2], [1, 146])
        self.assertEqual(st['get_tw'][:2], [1, 74])     # the call outside it

    def test_hal_gaps_fold_helpers_called_from_inside(self):
        # ClearOTagR's _otc / set_alarm / VSync(-1) are its own time; the
        # game's VSync(0) after it returns (caller game_main) is not.
        orig = m.caller_of
        callers = {'r1': 'ClearOTagR', 'r2': '_otc', 'r3': 'set_alarm', 'r4': 'game_main'}
        m.caller_of = lambda rest: callers.get(rest.split('ra=')[1].split()[0])
        def p2(label, tick, ra):
            return f'pc={label} tick={tick} frame=0 ra={ra} a0=0 a1=0 a2=0 a3=0\n'
        lines = [frame(0, 5), p2('ClearOTagR', 0, 'r4'), p2('_otc', 64, 'r1'),
                 p2('set_alarm', 416, 'r2'), p2('VSync', 420, 'r3'), p2('game_main', 640, 'r4'),
                 p2('ClearOTagR', 1000, 'r4'), p2('VSync', 1060, 'r4'), p2('x', 9000, 'r4')]
        with tempfile.NamedTemporaryFile('w', suffix='.trace', delete=False) as f:
            f.writelines(lines)
        try:
            st = m.hal_gaps(f.name, lo=0)
        finally:
            os.unlink(f.name)
            m.caller_of = orig
        self.assertEqual(st[('ClearOTagR', 0x14)], [2, 640 + 60])

    def test_by_step_keys_the_scene(self):
        st = self.run_gaps([frame(0, 5, 0x14), pc('a', 0), pc('b', 10),
                            frame(1, 6, 0x40), pc('a', 100), pc('b', 130)], by_step=True)
        self.assertEqual(st[('a', 0x14)][:2], [1, 10])
        self.assertEqual(st[('a', 0x40)][:2], [1, 30])

    def test_window(self):
        st = self.run_gaps([frame(0, 5), pc('a', 0), pc('b', 10),
                            frame(1, 9), pc('a', 100), pc('b', 130)], lo=8, hi=9)
        self.assertEqual(st['a'][:2], [1, 30])


class EmitTests(unittest.TestCase):
    def test_table_rows(self):
        d = tempfile.mkdtemp()
        tsv, out = os.path.join(d, 'c.tsv'), os.path.join(d, 't.c')
        with open(tsv, 'w') as f:
            f.write('# header\n')
            f.write('b\t-1\t100\t50\t5000\n')
            f.write('b\t20\t40\t90\t3600\n')     # differs, enough calls: kept
            f.write('b\t21\t40\t52\t2080\n')     # within 10%: dropped
            f.write('b\t22\t3\t500\t1500\n')     # too few calls: dropped
            f.write('a\t-1\t10\t7\t70\n')
            f.write('v_wait\t-1\t9\t170000\t1\n')  # a wait: never charged
        class A:
            pass
        a = A()
        a.tsv, a.out, a.min_calls = tsv, out, 16
        m.cmd_emit(a)
        with open(out) as fh:
            text = fh.read()
        self.assertLess(text.index('{"a", -1, 7u}'), text.index('{"b", -1, 50u}'))
        self.assertIn('{"b", 20, 90u}', text)
        self.assertNotIn('{"b", 21,', text)
        self.assertNotIn('{"b", 22,', text)
        self.assertNotIn('v_wait', text)

    def test_merge_adds_only_new_functions(self):
        d = tempfile.mkdtemp()
        base, new = os.path.join(d, 'base.tsv'), os.path.join(d, 'boot.tsv')
        with open(base, 'w') as f:
            f.write('# header\n')
            f.write('b\t-1\t100\t50\t5000\n')
            f.write('b\t20\t40\t90\t3600\n')
        with open(new, 'w') as f:
            f.write('# header\n')
            f.write('b\t-1\t1\t999\t999\n')          # BASE has b: kept as is
            f.write('InitPAD\t-1\t1\t47767\t47767\n')
            f.write('InitPAD\t0\t1\t47767\t47767\n')  # scene rows: not merged
            f.write('_bu_init\t-1\t1\t38830966\t38830966\n')  # a device wait
            f.write('VSync\t-1\t9\t100\t900\n')       # a wait
        class A:
            pass
        a = A()
        a.base, a.new, a.out = base, new, None
        m.cmd_merge(a)
        with open(base) as fh:
            lines = fh.read().splitlines()
        self.assertEqual(lines[0], '# header')
        self.assertEqual(lines[1:], ['InitPAD\t-1\t1\t47767\t47767',
                                     'b\t-1\t100\t50\t5000', 'b\t20\t40\t90\t3600'])


class CallerTests(unittest.TestCase):
    def test_caller_keys(self):
        m._CALLERS = ([0x80040000, 0x80050000], ['mainf', 'other'])
        lines = [frame(0, 5), 'pc=a tick=0 frame=0 ra=80040010 a0=0\n',
                 'pc=b tick=10 frame=0 ra=80150000 a0=0\n',
                 'pc=a tick=300 frame=0 ra=80050008 a0=0\n',
                 'pc=c tick=310 frame=0 ra=80040000 a0=0\n']
        with tempfile.NamedTemporaryFile('w', suffix='.trace', delete=False) as f:
            f.writelines(lines)
        try:
            st = m.gaps(f.name, inside=frozenset(), by_step=True, ovl=True)
        finally:
            os.unlink(f.name)
            m._CALLERS = None
        self.assertEqual(st[('a', 0x14, 'mainf')][:2], [1, 10])
        self.assertEqual(st[('b', 0x14, '@ovl')][:2], [1, 290])
        self.assertEqual(st[('a', 0x14, 'other')][:2], [1, 10])

    def test_emit_caller_rows(self):
        d = tempfile.mkdtemp()
        tsv, ovl, out = (os.path.join(d, n) for n in ('c.tsv', 'o.tsv', 't.c'))
        with open(tsv, 'w') as f:
            f.write('b\t-1\t100\t50\t5000\n')
        with open(ovl, 'w') as f:
            f.write('# h\n')
            f.write('b\t-1\tf\t40\t300\t12000\n')    # differs: kept
            f.write('b\t-1\tg\t40\t52\t2080\n')      # within 10%: dropped
        class A:
            pass
        a = A()
        a.tsv, a.out, a.min_calls, a.ovl = tsv, out, 16, ovl
        m.cmd_emit(a)
        with open(out) as fh:
            text = fh.read()
        self.assertIn('{"b", -1, 300u, "f"}', text)
        self.assertNotIn('"g"', text)


class IrqTests(unittest.TestCase):
    def test_sequencer_window_and_base(self):
        lines = [frame(0, 5), pc('@exc', 0), pc('trapIntrVSync', 21000),
                 pc('SsSeqCalledTbyT', 21050), pc('SeqPlay', 21100), pc('NoteOn', 21300),
                 pc('@seqret', 22000), pc('@rfe', 22500)]
        with tempfile.NamedTemporaryFile('w', suffix='.trace', delete=False) as f:
            f.writelines(lines)
        try:
            st, bases = m.irq_gaps(f.name)
        finally:
            os.unlink(f.name)
        self.assertEqual(st['SsSeqCalledTbyT'], [1, 50])
        self.assertEqual(st['SeqPlay'], [1, 200])
        self.assertEqual(st['NoteOn'], [1, 700])
        self.assertEqual(bases, [22500 - 950])


if __name__ == '__main__':
    unittest.main()


def read_lines(path):
    with open(path) as fh:
        return fh.readlines()


class BootTests(unittest.TestCase):
    """round 5: the boot table, the BIOS-call continuation and HAL tails"""

    def trace(self, lines):
        f = tempfile.NamedTemporaryFile('w', suffix='.trace', delete=False)
        f.writelines(lines)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def p2(self, label, tick, ra):
        return f'pc={label} tick={tick} frame=0 ra={ra} a0=0 a1=0 a2=0 a3=0\n'

    def test_continuation_after_a_bios_call(self):
        # bzero called from girl_rec_init (ra 80041e04) returns at 1000; the
        # caller runs to work_clear at 2191; an exception inside is excluded
        orig = m.caller_of
        m.caller_of = lambda rest: 'girl_rec_init'
        try:
            path = self.trace([frame(0, 0, 0x10), self.p2('bzero', 0, '80041e04'),
                               self.p2('@ret80041E04', 1000, '0'), self.p2('@exc', 1500, '0'),
                               self.p2('@rfe', 1600, '0'), self.p2('work_clear', 2291, '0')])
            out = path + '.tsv'
            self.addCleanup(lambda: os.path.exists(out) and os.unlink(out))

            class A:
                trace, lo, hi = path, 0, 0
            A.out = out
            m.cmd_analyze_ret(A)
            rows = [ln.split('\t') for ln in read_lines(out) if not ln.startswith('#')]
        finally:
            m.caller_of = orig
        self.assertIn(['bzero@ret', '16', 'girl_rec_init', '1', '1191', '1191\n'], rows)

    def test_tail_by_caller(self):
        orig = m.caller_of
        m.caller_of = lambda rest: 'draw_and_disp_env_init'
        try:
            path = self.trace([frame(0, 0, 0x00), self.p2('PutDrawEnv', 0, 'x'),
                               self.p2('SetDrawEnv', 31, 'x'), self.p2('SetIntrMask', 1169, 'x'),
                               self.p2('print_dr_env', 1721, 'x')])
            out = path + '.tsv'
            self.addCleanup(lambda: os.path.exists(out) and os.unlink(out))

            class A:
                trace, lo, hi = path, 0, 0
            A.out = out
            m.cmd_analyze_tail(A)
            rows = [ln.split('\t')[:5] for ln in read_lines(out) if not ln.startswith('#')]
        finally:
            m.caller_of = orig
        self.assertIn(['PutDrawEnv@tail', '0', 'draw_and_disp_env_init', '1', '552'], rows)

    def test_emit_boot_rows(self):
        base = self.trace(['# h\n', 'f\t-1\t1\t39\t39\n', 'f\t16\t1\t39\t39\n',
                           'VSync\t-1\t9\t100\t900\n', 'H\t-1\t1\t15\t15\n'])
        hal = self.trace(['# h\n', 'H\t-1\t1\t263\t263\n'])
        ret = self.trace(['# h\n', 'bzero@ret\t16\tgirl_rec_init\t1\t1191\t1191\n'])
        out = base + '.c'
        self.addCleanup(lambda: os.path.exists(out) and os.unlink(out))

        class A:
            tsv = base
        A.hal, A.ret, A.out = hal, [ret], out
        m.cmd_emit_boot(A)
        text = ''.join(read_lines(out))
        self.assertIn('{"f", 16, 39u}', text)
        self.assertIn('{"H", -1, 263u}', text)           # the HAL row replaces the plain gap
        self.assertNotIn('15u', text)
        self.assertNotIn('"VSync"', text)                # a wait is never charged
        self.assertIn('{"bzero@ret", 16, 1191u, "girl_rec_init"}', text)
        self.assertIn('cpu_cost_boot_count', text)
