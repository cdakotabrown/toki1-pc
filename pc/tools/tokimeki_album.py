#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The game's album save (an in-game save, "BISLPS-00065N") on a PSX card.

Sources (matched C, PS1 addresses from config/symbol_names.json):

* File name: save_name_set (0x800563DC) appends the slot's hex digit to
  "bu00:BISLPS-00065" -- slot 0 is the system file, album N is
  BISLPS-00065N (N hex).  Each is one 8 KiB block, an image of album_chr
  (0x8012B600) written by card_file_write (0x80054C74): "SC", icon, title,
  then RECORD 0 at +0x200 and its backup copy RECORD 1 at +0x1100, 0xF00
  bytes each.
* Checksum (save_game_set 0x80055950 writes it, save_check 0x80055DD0
  checks it): rec[0..3] = little-endian u32 sum of the unsigned bytes
  rec[4..0xEFF].  save_check takes the first record whose sum matches and
  whose bytes 4.. are not all zero; save_game_restore (0x80055E94) also
  refuses a record whose year or month is 0.
* Restore: save_game_restore copies the record field by field into PS1 RAM
  (the FIELDS table below).  After the load NAME_ENT's name_ent_exit goes
  to the night schedule (step 52, sub-step 255 -> 1), or to name entry
  (step 20) when the date is 95/4/4 or 98/3/1.
* The system file (slot 0) must list the album: save_sys_set sets
  album_status[N] (0x800F01F4 + N) to 1.  A card made by the game itself
  (pc/tools/scenario.py uses the one new_game.txt writes) already does.

Field meanings (FIELDS: record offset, size, PS1 address, what):
  calendar  season (0 term, 1/2/3 summer/winter/spring holidays), year
            (95..98), month, day, youbi (0 Monday .. 6 Sunday), pass_days.
            cal (128 bytes) is the day-kind/event table of this month and
            the next (cal_font_load 0x800689FC fills it from the calendar
            picture's trailer); it must match the month -- calendar_cal()
            builds it from the game's own calendar data.
  player    player_rec 0x800EE824: names (SJIS), basyo_pack (u16; bits
            12..15 = club, see CLUBS), birthday (bits 0..3 month, 4..8 day),
            the nine stats as s32 16.16 (para_* is the s16 integer half).
  girls     girl_tbl 0x800EE91C, 16 x 56 bytes (GIRLS order): tokimeki +2,
            yuukou +6, bomb +10 (s16, clamped 0..128 by girls_param_clamp),
            flags +12 (u32, bit 1 = met).
  reserved_date 0x800EECAC, 8 x u32: bits 0..4 place, 5..11 year, 12..15
            month, 16..20 day, 24 machiawase_flag, 25..28 girl, 29 active
            (tel_dialog20 / machiawase_set / reserved_date_check; e.g.
            0x20104BE0 = Shiori, 95/4/16, place 0 the neighbourhood park).
  bukatu_rec 0x800EEDCC, 12 x 4 bytes (club records); ev_flag 0x800EECCC
            (256 event flags).
"""
import struct

BLOCK = 8192
RECORD_OFFSETS = (0x200, 0x1100)
RECORD_SIZE = 0xF00

# name: (record offset, size or struct format, PS1 address, meaning)
FIELDS = {
    'season':        (4, 'B', 0x800EE79D, 'season_no: 0 term, 1/2/3 summer/winter/spring holidays'),
    'year':          (5, 'B', 0x800EE79E, 'now_year (95..98)'),
    'month':         (6, 'B', 0x800EE79F, 'now_month'),
    'day':           (7, 'B', 0x800EE7A0, 'now_day'),
    'youbi':         (8, 'B', 0x800EE7A1, 'now_youbi: 0 Monday .. 6 Sunday'),
    'pass_days':     (10, '<H', 0x800EE7A2, 'days since the start'),
    'cal':           (12, 128, 0x800EE7A4, 'cal_day_kind: this month and the next'),
    'player':        (140, 124, 0x800EE824, 'player_rec'),
    'p_sei':         (140 + 0x10, 8, 0x800EE834, 'surname (SJIS)'),
    'p_mei':         (140 + 0x18, 8, 0x800EE83C, 'given name (SJIS)'),
    'p_adana':       (140 + 0x20, 16, 0x800EE844, 'nickname (SJIS)'),
    'basyo_pack':    (140 + 0x30, '<H', 0x800EE854, 'bits 12..15 club; byte +1 low nibble byouki_kind'),
    'byouki_state':  (140 + 0x32, 'B', 0x800EE856, 'illness state'),
    'birthday':      (140 + 0x34, '<H', 0x800EE858, 'bits 0..3 month, 4..8 day'),
    'girl_tbl':      (264, 896, 0x800EE91C, '16 girl records x 56 bytes'),
    'mascot_flag2':  (1160, 16, 0x800EEC9C, ''),
    'reserved_date': (1176, 32, 0x800EECAC, '8 x u32 date reservations'),
    'bukatu_rec':    (1208, 48, 0x800EEDCC, '12 club records x 4 bytes'),
    'ev_flag':       (1256, 256, 0x800EECCC, 'event flags ev_flag00..FF'),
    'activity':      (3620, '<I', 0x800EF6C8, 'sched_icon: & 0xF the week activity'),
    # the rest of what save_game_restore copies (src/main/matched/
    # save_game_restore.c: the Chr struct a2024..w4140, record offset =
    # struct offset - 512); names from config/symbol_names.json
    'date_basyo_hist': (1512, 4, 0x800EEE88, 'the last four date places, newest first '
                        '(date_exit shifts basyo_hist..8B; 119 = none)'),
    'last_date_spot':  (1516, 16, 0x800EEE8C, 'last_date_spot[16]'),
    'last_date_spot_timer': (1532, 16, 0x800EEE9C, 'last_date_spot_timer[16]'),
    'last_date_spot_flag':  (1548, 16, 0x800EEEAC, 'last_date_spot_flag[16]'),
    'rec_tbl':       (1564, 2048, 0x800EEEBE, '256 x 8-byte record list; rec_table_trim '
                      'rebuilds rec_idx from it on load'),
    'icon_mode':     (3612, 'B', 0x800EF6CC, 'option: icon mode'),
    'icon_num':      (3613, 'B', 0x800EF6CD, 'option: icon number'),
    'menu_mode':     (3614, 'B', 0x800EF7F2, 'option: menu mode'),
    'bustup_sw':     (3615, 'B', 0x800EF6CE, 'option: portraits on/off'),
    'voice_level':   (3616, 'B', 0x800EE79C, 'option: voice'),
    'csr_speed':     (3624, 'B', 0x800EF6CF, 'option: cursor speed'),
    'click_mode':    (3625, 'B', 0x800EF6D0, 'option: click mode'),
    'click_speed':   (3626, 'B', 0x800EF6D3, 'option: click speed'),
    'mouse_btn_mode': (3627, 'B', 0x800EF6D5, 'option: mouse buttons'),
    'mouse_pad_st':  (3628, '<I', 0x800EF6C4, 'mouse/pad state word; also screen_w+3940, the '
                      'SpotFlags word TEL tel_spot_menu_set and basyo_flag_set read (magazine '
                      'bits)'),
}
STATS = ('para_tai', 'para_bun', 'para_ri', 'para_gei', 'para_un', 'para_zat',
         'para_you', 'para_kon', 'para_str')
for _i, _n in enumerate(STATS):   # the s16 integer half of each 16.16 slot
    FIELDS[_n] = (140 + 0x38 + 4 * _i + 2, '<h', 0x800EE85E + 4 * _i, 'stat (integer part)')
CLUBS = ('literature', 'drama', 'science', 'computer', 'art', 'brass', 'baseball',
         'soccer', 'tennis', 'swimming', 'basketball', 'none')
GIRLS = ('shiori', 'mio', 'yuina', 'ayako', 'saki', 'yukari', 'nozomi', 'mira',
         'yuko', 'megumi', 'yumi', 'miharu', 'rei', 'yoshio')
GIRL_FIELDS = {'tokimeki': (2, '<h'), 'yuukou': (6, '<h'), 'bomb': (10, '<h'), 'flags': (12, '<I'),
               # birthday (bits 0..3
               # month, 4..8 day, 9..10 blood type); +20 flags[0]; +21..+55
               # flags[1..35], the date-spot visit counters the ending sums
               # (spot.<k> is the byte at +20+k, k = 0..35)
               'birthday': (16, '<H')}
GIRL_SPOTS = 36
# bukatu_rec[club]: {u16 points (club sessions; master_bukatu_judge,
# touzyou_basyo_check), u8 b (low nibble = level, schedule_make's
# Koshien/final test), u8 c}
BUKATU_FIELDS = {'points': (0, '<H'), 'b': (2, 'B'), 'c': (3, 'B')}
# the date places' open bytes (spot.N): player_rec + 92 (screen_w + 288)
SPOT_OFFSET, SPOT_COUNT = 140 + 92, 16
# girl flag-word bits (record +12) the inventory names
FLAG_MET, FLAG_PHONE, FLAG_BIRTHDAY = 1 << 1, 1 << 3, 1 << 6


class AlbumError(Exception):
    pass


def checksum(rec):
    return sum(rec[4:RECORD_SIZE]) & 0xFFFFFFFF


def record_ok(rec):
    return (struct.unpack_from('<I', rec, 0)[0] == checksum(rec) and any(rec[4:RECORD_SIZE])
            and rec[5] != 0 and rec[6] != 0)


def find_file(card, name):
    """-> block index of the single-block file NAME"""
    for i in range(1, 16):
        frame = card[i * 128:(i + 1) * 128]
        if frame[0] == 0x51 and frame[0x0A:0x1E].split(b'\0', 1)[0].decode('ascii', 'replace') == name:
            return i
    raise AlbumError(f'no file {name} on the card')


def album_name(n):
    if not 1 <= n <= 15:
        raise AlbumError('album number must be 1..15')
    return 'BISLPS-00065' + '%X' % n


def read_record(card, n):
    blk = find_file(card, album_name(n))
    data = card[blk * BLOCK:(blk + 1) * BLOCK]
    for off in RECORD_OFFSETS:
        rec = bytearray(data[off:off + RECORD_SIZE])
        if record_ok(rec):
            return rec
    raise AlbumError(f'album {n}: no valid record (checksum)')


def write_record(card, n, rec):
    """Store REC as both records of album N with a fresh checksum."""
    blk = find_file(card, album_name(n))
    rec = bytearray(rec)
    struct.pack_into('<I', rec, 0, checksum(rec))
    for off in RECORD_OFFSETS:
        card[blk * BLOCK + off:blk * BLOCK + off + RECORD_SIZE] = rec


def album_listed(card, n):
    """the system file's album_status[n] (save_sys_set: record +4 + n)"""
    blk = find_file(card, album_name(15)[:-1] + '0')
    data = card[blk * BLOCK:(blk + 1) * BLOCK]
    for off in RECORD_OFFSETS:
        rec = data[off:off + RECORD_SIZE]
        if struct.unpack_from('<I', rec, 0)[0] == checksum(rec) and any(rec[4:]):
            return rec[4 + n] != 0
    return False


def get_field(rec, name):
    off, fmt, _, _ = FIELDS[name]
    if isinstance(fmt, int):
        return bytes(rec[off:off + fmt])
    return struct.unpack_from(fmt, rec, off)[0]


def set_field(rec, name, value):
    """NAME=VALUE: a FIELDS name, club=<index or name>, girl.<n|name>.<field>,
    reserved.<i>=<u32>, ev_flag.<i>=<byte>, spot.<place>=<byte>; VALUE an
    integer or hex: bytes"""
    if name == 'club':
        v = CLUBS.index(value) if isinstance(value, str) and value in CLUBS else int(value, 0)
        old = struct.unpack_from('<H', rec, FIELDS['basyo_pack'][0])[0]
        struct.pack_into('<H', rec, FIELDS['basyo_pack'][0], (old & 0x0FFF) | (v & 0xF) << 12)
        return
    if name.startswith('girl.'):
        parts = name.split('.')
        who, field = parts[1], parts[2]
        g = GIRLS.index(who) if who in GIRLS else int(who, 0)
        base = FIELDS['girl_tbl'][0] + 56 * g
        if field == 'spot':
            k = int(parts[3], 0)
            if not 0 <= k < GIRL_SPOTS:
                raise AlbumError(f'{name}: spot 0..{GIRL_SPOTS - 1}')
            rec[base + 20 + k] = int(value, 0) & 0xFF
            return
        if field not in GIRL_FIELDS:
            raise AlbumError(f'unknown girl field {field}')
        off, fmt = GIRL_FIELDS[field]
        struct.pack_into(fmt, rec, base + off, int(value, 0))
        return
    if name.startswith('bukatu.'):
        parts = name.split('.')
        c = CLUBS.index(parts[1]) if parts[1] in CLUBS else int(parts[1], 0)
        off, fmt = BUKATU_FIELDS[parts[2] if len(parts) > 2 else 'points']
        struct.pack_into(fmt, rec, FIELDS['bukatu_rec'][0] + 4 * c + off, int(value, 0))
        return
    if name.startswith('reserved.'):
        i = int(name.split('.')[1])
        struct.pack_into('<I', rec, FIELDS['reserved_date'][0] + 4 * i, int(value, 0))
        return
    if name.startswith('spot.'):
        # the date places' open bytes: TelState spot[] at screen_w+288 =
        # player_rec+92 (tel_spot_menu_set: open when spot[i] & 0x7F;
        # basyo_flag_set writes 1 = open, 128 = lost); spot.0..15
        i = int(name.split('.')[1], 0)
        if not 0 <= i < SPOT_COUNT:
            raise AlbumError(f'{name}: spot 0..{SPOT_COUNT - 1}')
        rec[SPOT_OFFSET + i] = int(value, 0) & 0xFF
        return
    if name.startswith('ev_flag.'):
        i = int(name.split('.')[1], 0)
        rec[FIELDS['ev_flag'][0] + i] = int(value, 0) & 0xFF
        return
    if name not in FIELDS:
        raise AlbumError(f'unknown field {name}')
    off, fmt, _, _ = FIELDS[name]
    if isinstance(fmt, int):
        raw = bytes.fromhex(value[4:] if value.startswith('hex:') else value)
        if len(raw) > fmt:
            raise AlbumError(f'{name}: {len(raw)} bytes for a {fmt}-byte field')
        rec[off:off + len(raw)] = raw
    else:
        struct.pack_into(fmt, rec, off, int(value, 0))


def reserved_date(girl, year, month, day, place, flag=0, active=1, bits21=0):
    """a reserved_date entry (bits21: bits 21..23, meaning unknown; the
    game's own entry for Shiori, 95/4/16, place 0 is 0x20104BE0: bits
    21..23 = 0, machiawase_flag = 0, active = 1)"""
    return ((place & 0x1F) | (year & 0x7F) << 5 | (month & 0xF) << 12 | (day & 0x1F) << 16
            | (bits21 & 7) << 21 | (flag & 1) << 24 | (girl & 0xF) << 25 | (active & 1) << 29)


def weekday(year, month, day):
    """youbi (0 Monday .. 6 Sunday) of a 19YY date"""
    import datetime
    return datetime.date(1900 + year, month, day).weekday()


def pass_days(year, month, day):
    """days since Tue 4 Apr 1995, the first day (new_game.txt: pass_days 0)"""
    import datetime
    return (datetime.date(1900 + year, month, day) - datetime.date(1995, 4, 4)).days


def season_of(month, day):
    """season_no: summer holidays 20 Jul..31 Aug, winter 24 Dec..7 Jan,
    spring 25 Mar..5 Apr (an approximation for scenario dates; the game
    sets it from its calendar -- season_at() gives the exact value)"""
    if (month == 7 and day >= 20) or month == 8:
        return 1
    if (month == 12 and day >= 24) or (month == 1 and day <= 7):
        return 2
    if (month == 3 and day >= 25) or (month == 4 and day <= 5):
        return 3
    return 0


def calendar_cal(ram, year, month, symbols):
    """cal_day_kind for year/month from a PS1 RAM image that holds the
    calendar pictures (cal_font_load: picture idx = year*12 + month - 1144,
    its trailer at word 2048: n, m, then n kind bytes and n event bytes as
    words).  -> 128 bytes [this month][next month], each {n, kind[31], m,
    event[31]}; None if the pictures are not in RAM."""
    def rd(addr, n=4):
        o = addr - 0x80000000
        if not 0 <= o <= len(ram) - n:
            raise IndexError
        return struct.unpack_from('<I', ram, o)[0]
    tbl_ptr = symbols.get('cal_font_load_tbl0')
    if tbl_ptr is None:
        return None
    if year * 12 + month - 1144 >= 35:
        # cal_font_load returns at once for idx >= 35 (March 1998: there is
        # no April 98 picture), so the game keeps February's table
        # [Feb 98][Mar 98] through March
        return calendar_cal(ram, 98, 2, symbols)
    try:
        table = rd(tbl_ptr)
        out = bytearray()
        for idx in (year * 12 + month - 1144, year * 12 + month - 1143):
            pic = rd(table + 4 * idx)
            base = pic + 4 * 2048
            n, m = rd(base), rd(base + 4)
            if not (0 < n <= 31 and 0 <= m <= 31):
                return None
            block = bytearray(64)
            block[0] = n
            block[32] = m
            for i in range(n):
                block[1 + i] = rd(base + 4 * (2 + i)) & 0xFF
                block[33 + i] = rd(base + 4 * (2 + n + i)) & 0xFF
            out += block
        return bytes(out)
    except IndexError:
        return None


# schedule_make puts a type-253 entry (step 57, _schedule_main39) at the
# head of a day whose calendar event is one of these; _schedule_main39
# toggles season_no: non-zero -> 0, else by the month (7 -> 1 summer,
# 12 -> 2 winter, 3 -> 3 spring, other -> 0)
SEASON_EVENTS = (6, 14, 20, 10, 16, 2)


def month_events(ram, year, month, symbols):
    """[event of day 1, day 2, ...] of one month (its own calendar block)"""
    if (year, month) == (98, 3):
        cal = calendar_cal(ram, 98, 2, symbols)
        blk = None if cal is None else cal[64:128]
    else:
        cal = calendar_cal(ram, year, month, symbols)
        blk = None if cal is None else cal[0:64]
    if blk is None:
        return None
    return list(blk[33:33 + blk[0]])


def season_at(ram, year, month, day, symbols):
    """season_no on the night of YY/MM/DD (the night planner's date): the
    toggles of every season event from the first day (95/4/4, season 0)
    up to and including that day.  None if the calendar is not in RAM."""
    season = 0
    y, m = 95, 4
    while (y, m) <= (year, month):
        ev = month_events(ram, y, m, symbols)
        if ev is None:
            return None
        for d, e in enumerate(ev, 1):
            if (y, m) == (95, 4) and d < 4:
                continue
            if (y, m) == (year, month) and d > day:
                break
            if e in SEASON_EVENTS:
                season = 0 if season else {7: 1, 12: 2, 3: 3}.get(m, 0)
        y, m = (y + 1, 1) if m == 12 else (y, m + 1)
    return season


def apply_date(rec, year, month, day, ram=None, symbols=None, keep=()):
    """date=YY/MM/DD and what follows from it: youbi, pass_days, season and
    the month's cal table (from RAM holding the calendar pictures; without
    one, season_of()'s approximation and the record's own cal).  Fields
    named in KEEP are left alone (set explicitly by the caller)."""
    set_field(rec, 'year', str(year))
    set_field(rec, 'month', str(month))
    set_field(rec, 'day', str(day))
    if 'youbi' not in keep:
        set_field(rec, 'youbi', str(weekday(year, month, day)))
    if 'pass_days' not in keep:
        set_field(rec, 'pass_days', str(pass_days(year, month, day)))
    season = None
    if ram is not None:
        season = season_at(ram, year, month, day, symbols)
        if 'cal' not in keep:
            cal = calendar_cal(ram, year, month, symbols)
            if cal is None:
                raise AlbumError(f'no calendar data for {year}/{month}')
            off = FIELDS['cal'][0]
            rec[off:off + 128] = cal
    if 'season' not in keep:
        set_field(rec, 'season', str(season_of(month, day) if season is None else season))
