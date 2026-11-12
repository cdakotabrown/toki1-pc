# SPDX-License-Identifier: GPL-3.0-or-later
"""Seeded scene starts (pc/tools/seeded_starts.py) and playthrough.py's
`poke` item: the entry bytes against goto_step.c, the recipe rules that
keep the tool honest (every hand-built field cites its C; a poke only
after a wait; every result tagged seeded), and the verdict classes.
No emulator, no port run."""
import json
import os
import tempfile
import unittest

import playthrough as pt
import seeded_starts as ss


def ram_with(writes):
    ram = bytearray(0x200000)
    for addr, data in writes:
        o = addr - 0x80000000
        ram[o:o + len(data)] = data
    return bytes(ram)


class EntryTests(unittest.TestCase):
    def test_redirect_step_goes_through_the_overlay_loader(self):
        # step_state_save_and_redirect.c: 115 -> prev_step 115, now_step 2
        w = {e['name']: e for e in ss.entry_writes(115)}
        steps = w['now_step..prev_step']
        self.assertEqual(steps['addr'], 0x800EF868)
        self.assertEqual(steps['data'], bytes([2, 0, 0, 0, 0, 0, 115]))
        self.assertEqual(w['step_frame_cnt..step_counter']['data'], bytes(12))
        self.assertEqual(w['sound_seek_time']['data'], bytes(2))
        self.assertEqual(w['mes_base_y']['data'], (184).to_bytes(2, 'little'))

    def test_main_step_is_written_directly(self):
        # 208 (TT) is a game_main case of its own, not a redirect step
        self.assertNotIn(208, ss.redirect_steps())
        w = {e['name']: e for e in ss.entry_writes(208)}
        self.assertEqual(w['now_step..step_mode']['data'], bytes([208, 0, 0, 0, 0, 0]))

    def test_entry_addresses_match_the_symbol_table(self):
        for name, addr in (('now_step', 0x800EF868), ('prev_step', 0x800EF86E),
                           ('step_frame_cnt', 0x800EF85C), ('sound_seek_time', 0x800D07AC),
                           ('mes_base_y', 0x8012DD10), ('class_w', 0x801271B0)):
            self.assertEqual(ss.resolve(name)[0], addr, name)
        # the contiguous runs the entry writes rely on
        self.assertEqual(ss.resolve('step_counter')[0], 0x800EF864)
        self.assertEqual(ss.resolve('step_mode')[0], 0x800EF86D)

    def test_mes_window_switch_clears_only_set_bits(self):
        cw = ss.resolve('class_w')[0]
        mode = ss.resolve('basyo_init_tbl0')[0]
        probe = ram_with([(mode, (1).to_bytes(2, 'little')),       # 3 windows
                          (cw + 207, b'\xa4'), (cw + 68 + 207, b'\x04'),
                          (cw + 136 + 207, b'\x81'), (cw + 204 + 207, b'\x80')])
        w = ss.entry_writes(52, probe)
        names = {e['name']: e['data'] for e in w}
        self.assertEqual(names['k_disp_switch_tbl0[0]'], b'\x00')   # cw3_disp bit 7 set
        self.assertEqual(names['class_w[0*68+207]'], b'\x24')
        self.assertNotIn('class_w[1*68+207]', names)                # bit 7 already clear
        self.assertEqual(names['class_w[2*68+207]'], b'\x01')
        self.assertNotIn('class_w[3*68+207]', names)                # beyond mode + 2

    def test_no_k_disp_write_when_cw3_disp_clear(self):
        probe = ram_with([])
        names = {e['name'] for e in ss.entry_writes(52, probe)}
        self.assertNotIn('k_disp_switch_tbl0[0]', names)


class RecipeTests(unittest.TestCase):
    def test_resolve_forms(self):
        self.assertEqual(ss.resolve('restore_block'), (0x800EE79D, 0x800EF6D4 - 0x800EE79D))
        self.assertEqual(ss.resolve('rpg_bat_arg:4'), (0x8012B1C0, 4))
        self.assertEqual(ss.resolve('0x800F0204:14'), (0x800F0204, 14))
        with self.assertRaises(ss.SeedError):
            ss.resolve('D_8012B1C0')             # address name: no size known
            # (a landed name always resolves: symbol_table() defaults its size to 4)
        with self.assertRaises(ss.SeedError):
            ss.resolve('0x7FFFFFF0:4')          # outside PS1 RAM
        with self.assertRaises(ss.SeedError):
            ss.resolve('no_such_symbol:1')

    def test_hand_field_without_why_is_refused(self):
        r = dict(id='x', scene=129, base='title', covers=['-'],
                 fields=[dict(name='rpg_bat_arg:4', value=1)])
        with self.assertRaises(ss.SeedError):
            ss.validate(r, {})
        r['fields'][0]['why'] = 'rpg_bat_init.c'
        ss.validate(r, {})

    def test_variants_expand_after_the_recipe_fields(self):
        r = dict(id='rpg', scene=129, base='title', covers=['-'],
                 fields=[dict(name='now_girl:1', value=2, why='a')],
                 variants={'b1': dict(fields=[dict(name='rpg_bat_arg:4', value=1, why='b')]),
                           'p': dict(scene=130)})
        out = {v['id']: v for v in ss.expand(r)}
        self.assertEqual([f['name'] for f in out['rpg_b1']['fields']], ['now_girl:1', 'rpg_bat_arg:4'])
        self.assertEqual(out['rpg_p']['scene'], 130)
        self.assertNotIn('variants', out['rpg_p'])

    def test_every_tracked_recipe_validates(self):
        starts, caps = ss.load_recipes()
        self.assertGreater(len(starts), 50)
        for r in starts:
            for f in r.get('fields', []):
                self.assertTrue(f['why'].strip(), r['id'])

    def test_poke_lines_chunk_and_keep_the_seed_whole(self):
        writes = [dict(name='restore_block', addr=0x800EE79D, data=bytes(600), src='capture:c x'),
                  dict(name='rand_seed', addr=0x80009010, data=(1093).to_bytes(4, 'little'),
                       src='hand-built')]
        lines = ss.poke_lines(writes)
        self.assertEqual(len(lines), 4)            # 256 + 256 + 88, then the seed
        self.assertTrue(all(l.startswith('poke 0 ') for l in lines))
        self.assertIn('0x800EE89D', lines[1])
        self.assertIn('0x80009010 45040000', lines[3])

    def test_seed_order_fields_rand_clocks_entry(self):
        r = dict(id='x', scene=129, base='title', covers=['-'],
                 fields=[dict(name='rpg_bat_arg:4', value=6, why='rpg_bat_init.c')])
        names = [w['name'] for w in ss.seed_writes(r, '/nonexistent')]
        self.assertEqual(names[:4], ['rpg_bat_arg:4', 'rand_seed', 'frame_counter', 'vblnk_counter'])
        self.assertEqual(names[4], 'step_frame_cnt..step_counter')
        r.update(rand_seed=None, clocks=False)
        names = [w['name'] for w in ss.seed_writes(r, '/nonexistent')]
        self.assertNotIn('rand_seed', names)
        self.assertNotIn('frame_counter', names)

    def test_step_paths_shift_and_path(self):
        def rows(seq):
            return [dict(frame_counter=f, now_step=t[0], now_sub_step=t[1], now_sub_sub_step=t[2])
                    for f, t in seq]
        a = rows([(0, (1, 0, 0)), (1, (1, 0, 1)), (5, (1, 0, 2)), (9, (1, 1, 0))])
        b = rows([(0, (1, 0, 0)), (2, (1, 0, 1)), (6, (1, 0, 2)), (11, (1, 1, 0))])
        self.assertTrue(ss.step_paths(a, b, 0, 0)['same'])
        c = rows([(0, (1, 0, 0)), (1, (1, 0, 1)), (5, (1, 0, 2)), (9, (1, 2, 0))])
        fd = ss.step_paths(a, c, 0, 0)['first_diff']
        self.assertEqual((fd['index'], fd['cause']), (3, 'path'))
        d = rows([(0, (1, 0, 0)), (1, (1, 0, 1)), (6, (1, 0, 2)), (9, (1, 2, 0))])
        self.assertEqual(ss.step_paths(a, d, 0, 0)['first_diff']['cause'], 'input timing')

    def test_values(self):
        self.assertEqual(ss.value_bytes(dict(name='x', value=1), 4), b'\x01\0\0\0')
        self.assertEqual(ss.value_bytes(dict(name='x', value='hex:0102'), 2), b'\x01\x02')
        self.assertEqual(ss.value_bytes(dict(name='x', value='0x34'), 1), b'\x34')
        with self.assertRaises(ss.SeedError):
            ss.value_bytes(dict(name='x', value='hex:01'), 2)


class VerdictTests(unittest.TestCase):
    def res(self, **kw):
        r = dict(entered_pc=True, entered_retail=True, pc_crashes=[], folds=0, rc=0,
                 seed_gate=dict(seeded_bytes_wrong=[], mes_window_same=True),
                 frames=dict(status='same'))
        r.update(kw)
        return r

    def test_classes(self):
        self.assertEqual(ss.verdict(self.res(), True), 'PASS same')
        self.assertEqual(ss.verdict(self.res(frames=dict(status='diverged', cls='timing')), True),
                         'PASS diverged:timing')
        self.assertTrue(ss.verdict(self.res(entered_pc=False, entered_retail=False),
                                   True).startswith('FAIL state:both'))
        self.assertTrue(ss.verdict(self.res(folds=3), True).startswith('FAIL port-only'))
        self.assertEqual(ss.verdict(self.res(entered_retail=False), True), 'FAIL retail-only')
        self.assertTrue(ss.verdict(self.res(seed_gate=dict(seeded_bytes_wrong=['pc:x'],
                                                           mes_window_same=True)),
                                   True).startswith('FAIL gate'))
        self.assertTrue(ss.verdict(self.res(seed_gate=dict(seeded_bytes_wrong=[],
                                                           mes_window_same=False)),
                                   True).startswith('FAIL gate'))


class PlaythroughPokeTests(unittest.TestCase):
    def parse(self, text):
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as fh:
            fh.write(text)
        try:
            return pt.parse(fh.name)
        finally:
            os.unlink(fh.name)

    def test_poke_compiles_to_the_port_input_and_the_reference_args(self):
        s = self.parse('phase p\nwait a now_step=0x12\npoke 30 0x800EF868 0200\nend 5\n')
        text = pt.numeric_phase(s['phases'][0], s['hold'], False)
        self.assertIn('poke 30 0x800EF868 0200', text)
        self.assertEqual(pt.seeded_pokes(s), [dict(phase='p', delta=30, addr='0x800EF868', bytes=2)])
        self.assertEqual(pt.poke_args([(100, 0x800EF868, b'\x02\x00')]),
                         ['-poke', '100:800EF868:0200'])
        self.assertEqual(pt.poke_args([(100, 0x800EF868, b'\x02')], upto=99), [])

    def test_poke_must_follow_a_wait(self):
        with self.assertRaises(pt.ScriptError):
            self.parse('phase p\npoke 1 0x800EF868 02\nwait a now_step=0x12\n')

    def test_poke_outside_ram_or_too_long_is_refused(self):
        for line in ('poke 1 0x7FFFFFFF 02', 'poke 1 0x801FFFFF 0202',
                     'poke 1 now_step 02', 'poke 1 0x800EF868 ' + '00' * 257):
            with self.subTest(line=line), self.assertRaises(pt.ScriptError):
                self.parse('phase p\nwait a now_step=0x12\n' + line + '\n')

    def test_a_script_without_pokes_is_not_seeded(self):
        s = self.parse('phase p\nwait a now_step=0x12\n+1 circle\n')
        self.assertEqual(pt.seeded_pokes(s), [])


class PortArgsTests(unittest.TestCase):
    """`run --set NAME=VALUE --profile P` reaches the playthrough command of
    every run (the port's own environment is stripped by playthrough.py)"""

    def cmd_of(self, argv):
        seen = []
        real = ss.subprocess.run

        def fake(cmd, **kw):
            seen.append(cmd)
            raise RuntimeError('stop')
        with tempfile.TemporaryDirectory() as d:
            ss.main(['list', '--out', d] + argv)
            item = dict(job='j', script='s.txt', card='c.mcr', max_frames=10)
            ss.subprocess.run = fake
            try:
                with self.assertRaises(RuntimeError):
                    ss.run_job(item, d, '/bin/true', False, 10, True)
            finally:
                ss.subprocess.run = real
        return seen[0]

    def test_set_and_profile_passed(self):
        cmd = self.cmd_of(['--set', 'TOKI_MUSIC_LOG=/x.jsonl', '--set', 'PC_DEBUG=sound',
                           '--profile', 'enhanced'])
        tail = cmd[cmd.index('--shot-jobs') + 2:]
        self.assertEqual(tail, ['--set', 'TOKI_MUSIC_LOG=/x.jsonl', '--set', 'PC_DEBUG=sound',
                                '--profile', 'enhanced'])

    def test_negative_control_nothing_by_default(self):
        cmd = self.cmd_of([])
        self.assertNotIn('--set', cmd)
        self.assertNotIn('--profile', cmd)


if __name__ == '__main__':
    unittest.main()
