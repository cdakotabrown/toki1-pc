# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the debug toolkit's host-side tools (no game binary or
assets needed): the save-state file reader / differ, the album record
editor, the scenario parser.  The end-to-end save-state check is
pc/tools/savestate_roundtrip.py; the scenarios run with pc/tools/scenario.py."""
import glob
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcr_tool  # noqa: E402
import playthrough as pt  # noqa: E402
import savestate_tool as sst  # noqa: E402
import scenario  # noqa: E402
import tokimeki_album as alb  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def section(tag, data, addr=0, paged=False):
    if paged:
        body = bytearray()
        for off in range(0, len(data), 4096):
            page = data[off:off + 4096]
            if any(page):
                body += b'\x01' + page
            else:
                body += b'\x00'
    else:
        body = data
    return tag.encode() + struct.pack('<IQQQ', 1 if paged else 0, addr, len(data), len(body)) + bytes(body)


def make_state(path, exrw, pram, keep=()):
    keep_raw = b''.join(struct.pack('<QQ', lo, hi) for lo, hi in keep)
    with open(path, 'wb') as fh:
        fh.write(b'TOKISTv1')
        fh.write(section('INFO', b'format=1\nbuild_id=abc\nframe_counter=5\nexrw=1000-3000\n'))
        fh.write(section('MAPS', b''))
        fh.write(section('KEEP', keep_raw))
        fh.write(section('EXRW', exrw, 0x1000, paged=True))
        fh.write(section('PRAM', pram, 0x80000000, paged=True))
        fh.write(section('MMIO', bytes(16), 0x1F801000))


class FakeSyms:
    def name(self, addr):
        return None, 0

    def lookup(self, label):
        return None, None


class StateFileTest(unittest.TestCase):
    def test_read_and_diff(self):
        with tempfile.TemporaryDirectory() as d:
            ex = bytearray(0x2000)
            ex[0x10] = 7
            ram = bytearray(0x3000)
            ram[0x2001] = 9
            a, b = os.path.join(d, 'a.tks'), os.path.join(d, 'b.tks')
            make_state(a, bytes(ex), bytes(ram), keep=[(0x1100, 0x1108)])
            st = sst.read_state(a)
            self.assertEqual(st['info']['frame_counter'], '5')
            self.assertEqual(st['regions']['EXRW'][1], bytes(ex))
            self.assertEqual(st['regions']['PRAM'][1], bytes(ram))
            self.assertEqual(st['keep'], [(0x1100, 0x1108)])
            # a change inside a KEEP range is not a difference; outside it is
            ex2 = bytearray(ex)
            ex2[0x100] = 1          # 0x1100: kept
            make_state(b, bytes(ex2), bytes(ram), keep=[(0x1100, 0x1108)])
            rep = sst.diff_states(sst.read_state(a), sst.read_state(b), FakeSyms())
            self.assertFalse(any(rep.values()))
            ex2[0x200] = 1          # 0x1200: restored state
            ram2 = bytearray(ram)
            ram2[0] = 1
            make_state(b, bytes(ex2), bytes(ram2), keep=[(0x1100, 0x1108)])
            rep = sst.diff_states(sst.read_state(a), sst.read_state(b), FakeSyms())
            self.assertEqual(rep['exrw'], [(0x1200, 0x1201)])
            self.assertEqual(rep['pram'], [(0x80000000, 0x80000001)])

    def test_format2_records_and_host_noise(self):
        """format 2 (pc/hal/dbg_state.c): named native records; a Windows
        state's comparison leaves the CD queue payload tails out"""
        def record(key, addr, data):
            k = key.encode()
            body = bytearray()
            for off in range(0, len(data), 4096):
                page = data[off:off + 4096]
                body += (b'\x01' + page) if any(page) else b'\x00'
            return struct.pack('<H', len(k)) + k + struct.pack('<QQQ', addr, len(data), len(body)) + bytes(body)

        def make(path, platform, vram, pram):
            recs = [record('g_vram', 0x1000, vram), record('cd.c:g_x', 0x9000, b'\x05\x00\x00\x00')]
            natv = struct.pack('<I', len(recs)) + b''.join(recs)
            with open(path, 'wb') as fh:
                fh.write(b'TOKISTv2')
                fh.write(section('INFO', f'format=2\nbuild_id=ab\nplatform={platform}\n'
                                         f'frame_counter=9\n'.encode()))
                fh.write(section('MAPS', b''))
                fh.write(section('PRAM', pram, 0x80000000, paged=True))
                fh.write(section('MMIO', bytes(16), 0x1F801000))
                fh.write(b'NATV' + struct.pack('<IQQQ', 0, 0, len(vram) + 4, len(natv)) + natv)

        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, 'a.tks'), os.path.join(d, 'b.tks')
            vram = bytearray(8192)
            vram[5] = 3
            ram = bytearray(0x100000)
            noise = sst.CDQ_QUE + 12      # record 0, payload tail
            make(a, 'windows-x86_64', bytes(vram), bytes(ram))
            st = sst.read_state(a)
            self.assertEqual(st['format'], 2)
            self.assertEqual(st['records']['g_vram'], (0x1000, bytes(vram)))
            self.assertEqual(st['records']['cd.c:g_x'][1], b'\x05\x00\x00\x00')
            vram2, ram2 = bytearray(vram), bytearray(ram)
            vram2[100] = 1
            ram2[noise] = 0x77
            make(b, 'windows-x86_64', bytes(vram2), bytes(ram2))
            rep = sst.diff_states(st, sst.read_state(b))
            self.assertEqual(rep['vram'], [(0x1000 + 100, 0x1000 + 101)])
            self.assertEqual(rep['pram'], [])               # Windows: the payload tail is noise
            self.assertEqual(rep['host_noise'], [(0x80000000 + noise, 0x80000000 + noise + 1)])
            self.assertEqual(sst.name_of(st, 0x1000 + 100), 'g_vram+0x64')
            # the same bytes in Linux states are a difference (no mask there)
            make(a, 'linux-x86_64', bytes(vram), bytes(ram))
            make(b, 'linux-x86_64', bytes(vram), bytes(ram2))
            rep = sst.diff_states(sst.read_state(a), sst.read_state(b))
            self.assertEqual(rep['pram'], [(0x80000000 + noise, 0x80000000 + noise + 1)])

    def test_diff_ranges_do_not_bridge_equal_bytes(self):
        a = bytes([0, 1, 0, 0, 1, 0])
        b = bytes([0, 2, 0, 0, 2, 0])
        self.assertEqual(sst.diff_bytes(a, b, 100), [(101, 102), (104, 105)])

    def test_not_a_state(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'x')
            with open(p, 'wb') as fh:
                fh.write(b'nope')
            with self.assertRaises(sst.StateError):
                sst.read_state(p)


def make_card_with_album(n=1):
    card = bytearray(mcr_tool.MC_SIZE)
    mcr_tool.format_card(card)
    for slot, name in ((1, 'BISLPS-000650'), (2, alb.album_name(n))):
        frame = mcr_tool.dir_frame(card, slot)
        frame[0:4] = struct.pack('<I', 0x51)
        frame[4:8] = struct.pack('<I', 8192)
        frame[8:10] = struct.pack('<H', 0xFFFF)
        frame[0x0A:0x0A + len(name)] = name.encode()
        mcr_tool.seal_frame(card, slot)
    sysrec = bytearray(alb.RECORD_SIZE)
    sysrec[4 + n] = 1
    struct.pack_into('<I', sysrec, 0, alb.checksum(sysrec))
    card[8192 + 0x200:8192 + 0x200 + alb.RECORD_SIZE] = sysrec
    rec = bytearray(alb.RECORD_SIZE)
    rec[5], rec[6], rec[7] = 95, 4, 4
    alb.write_record(card, n, rec)
    return card


class AlbumTest(unittest.TestCase):
    def test_reserved_date_matches_the_game(self):
        # tel_dialog20's entry for Shiori, 95/4/16, the park (date_save.txt)
        self.assertEqual(alb.reserved_date(0, 95, 4, 16, 0), 0x20104BE0)

    def test_calendar_helpers(self):
        self.assertEqual(alb.weekday(95, 4, 16), 6)     # a Sunday
        self.assertEqual(alb.pass_days(95, 4, 9), 5)     # new_game.txt: Sun 9 Apr, 5 days
        self.assertEqual(alb.season_of(8, 1), 1)

    def test_edit_round_trip(self):
        card = make_card_with_album(1)
        self.assertTrue(alb.album_listed(card, 1))
        rec = alb.read_record(card, 1)
        alb.set_field(rec, 'club', 'tennis')
        alb.set_field(rec, 'para_tai', '150')
        alb.set_field(rec, 'girl.shiori.tokimeki', '90')
        alb.set_field(rec, 'reserved.0', '0x20104BE0')
        alb.write_record(card, 1, rec)
        back = alb.read_record(card, 1)
        self.assertEqual(alb.get_field(back, 'basyo_pack') >> 12, alb.CLUBS.index('tennis'))
        self.assertEqual(alb.get_field(back, 'para_tai'), 150)
        self.assertEqual(struct.unpack_from('<h', back, alb.FIELDS['girl_tbl'][0] + 2)[0], 90)
        # both records carry the checksum save_check verifies
        blk = alb.find_file(card, alb.album_name(1))
        for off in alb.RECORD_OFFSETS:
            r = card[blk * 8192 + off:blk * 8192 + off + alb.RECORD_SIZE]
            self.assertEqual(struct.unpack_from('<I', r, 0)[0], sum(r[4:]))

    def test_corrupt_record_is_refused(self):
        card = make_card_with_album(1)
        blk = alb.find_file(card, alb.album_name(1))
        for off in alb.RECORD_OFFSETS:
            card[blk * 8192 + off + 100] ^= 0xFF
        with self.assertRaises(alb.AlbumError):
            alb.read_record(card, 1)

    def test_calendar_from_ram(self):
        # the cal_font_load trailer layout: picture idx at +8192 holds
        # n, m, n kind words, n event words
        ram = bytearray(0x200000)
        table = 0x80100000
        struct.pack_into('<I', ram, 0x1000, table)          # cal_font_load_tbl0
        for idx in range(36):
            pic = 0x80110000 + idx * 0x3000
            struct.pack_into('<I', ram, table - 0x80000000 + 4 * idx, pic)
            base = pic - 0x80000000 + 8192
            n = 28 + idx % 4
            struct.pack_into('<II', ram, base, n, 3)
            for i in range(n):
                struct.pack_into('<I', ram, base + 8 + 4 * i, idx)
                struct.pack_into('<I', ram, base + 8 + 4 * (n + i), 100 + i)
        cal = alb.calendar_cal(bytes(ram), 95, 6, {'cal_font_load_tbl0': 0x80001000})
        self.assertEqual(len(cal), 128)
        self.assertEqual(cal[0], 28 + 2 % 4)            # June 95 = picture 2
        self.assertEqual(cal[1], 2)
        self.assertEqual(cal[32], 3)
        self.assertEqual(cal[33], 100)
        self.assertEqual(cal[64], 28 + 3 % 4)           # the next month


class ScenarioFileTest(unittest.TestCase):
    def test_every_scenario_parses(self):
        files = sorted(glob.glob(os.path.join(HERE, 'scenarios', '*.txt')))
        self.assertTrue(files)
        for f in files:
            sc = scenario.parse(f)
            self.assertIn(sc['start'], ('album', 'state'))
            self.assertTrue(sc['expect']['conds'], f)
            for c in sc['expect']['conds']:
                pt.parse_cond(c)
            for kv in sc['album']:
                k, _, v = kv.partition('=')
                if k != 'date':
                    alb.set_field(bytearray(alb.RECORD_SIZE), k, v)

    def test_load_sequence_and_inputs_compile(self):
        for f in sorted(glob.glob(os.path.join(HERE, 'scenarios', '*.txt'))):
            sc = scenario.parse(f)
            if sc['start'] != 'album':
                continue
            with tempfile.TemporaryDirectory() as d:
                p = os.path.join(d, 's.txt')
                with open(p, 'w') as fh:
                    fh.write('hold 2\nphase scenario\n' + scenario.LOAD_ALBUM_1 + '\n'.join(sc['input']) + '\n')
                script = pt.parse(p)
                text = pt.numeric_phase(script['phases'][0], script['hold'], False)
                self.assertIn('wait album_loaded', text)


if __name__ == '__main__':
    unittest.main()
