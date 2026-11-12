#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Seeded scene starts: enter a chosen scene from a chosen state, the same
way on the port and on the reference, and compare (a COVERAGE TOOL).

Every result this tool writes is tagged `seeded`.  It is not a normal run:
the normal-run parity rule (no state writes and no scene jumps during a
run) stays in force for every other suite.
Here the state is written ONCE, at one defined point, on both machines,
from the same bytes:

  1. Power-on and the game's own path to the defined point P, on both
     machines with the same pad input (playthrough.py --retail):
       base `title`         a freshly formatted card; P = the title menu
                            (18/2/1) waiting for input;
       base `checkpoint:ID` a pc/tools/checkpoints.py card, loaded by the
                            title's Continue -> album 1; P = the loaded
                            planner (52/1, `album_loaded`);
       base `album`         a card crafted here with checkpoints.craft (a
                            crafted album record), loaded the same way.
  2. At P + 30 VBlanks: the seed, as `poke` lines of the playthrough script
     (pc/hal/trace.c PC_INPUT `poke` on the port, the reference emulator's
     `-poke FRAME:ADDR:HEX` on the reference; both apply the bytes at the
     start of that VBlank, before its trace row -- the same frame of the
     same game state, the game idling in VSync on an input screen).  The
     seed is, in this order:
       - ranges of a RAM capture taken at the scene's entry in a real run
         (`capture`: the port at the first main-loop boundary where the
         game's own goto_step(N) has run), or of a PS1 RAM dump
         (disks/dumps/ram.bin);
       - hand-built fields, each with the C it was read from (`why`);
       - the kernel rand seed (0x80009010; on the port g_psx_rand_seed,
         where the trace reads it), 1093 unless the recipe says otherwise:
         the title's srand(vblnk_counter) value on the reference,
         so both machines draw the same numbers;
       - the scene entry: the stores goto_step(N) makes
         (src/main/matched/goto_step.c) -- step_frame_cnt, step_state,
         step_counter = 0; now_step = N (or 2 with prev_step = N for the
         steps step_state_save_and_redirect.c sends through the overlay
         loader); now_sub_step, now_sub_sub_step, scene_entry_b/c,
         step_mode = 0; sound_seek_time = 0; and mes_window_switch(0)'s
         mes_base_y = 184.  Its k_disp_switch(0, 0) runs only when
         cw3_disp bit 7 is set, so the script checks that bit is clear
         at P (`seed_ok`); its class_w bit-7 clears are checked in the
         seed gate's RAM dumps (`mes_window_bits`, reported).
  3. `shot seeded` dumps both machines' RAM right after the seed (the seed
     gate: every seeded byte is in place on both, and the restore block
     0x800EE79D..0x800EF6D4 is identical), then `wait entered`, then the
     input: a script (fragments "@NAME" of pc/tools/checkpoints/play), the
     explorer's random input (explorer.generate, a pure function of the
     run id and seed), or both.

Verdicts (summary.md): PASS when both machines enter the scene and the port
has no crash and no fault fold; the first divergence after `entered` is
classed as in coverage_suite.py.  A failure the reference shows too is a
state problem (`state:both`), not a port defect; only port-vs-reference
divergences (`port-only` failures, `path` / `state` classes) are triaged.

    python3 pc/tools/seeded_starts.py list
    python3 pc/tools/seeded_starts.py show ID
    python3 pc/tools/seeded_starts.py capture [CAPID ...]
    python3 pc/tools/seeded_starts.py build [ID|GROUP ...]
    python3 pc/tools/seeded_starts.py run [ID|GROUP ...] --jobs 2 \\
        --reach-bin build/coverage/tokimeki-reach [--no-retail] [--rebuild]
    python3 pc/tools/seeded_starts.py summary
    python3 pc/tools/seeded_starts.py report --bin build/coverage/tokimeki-reach \\
        [--prior PRIOR_REPORT.json|LIST_DIR ...]

Recipes: pc/tools/seeded/*.json ({"group": G, "captures": {...},
"starts": [...]}).  Everything
game-derived (cards, captures, scripts, traces, RAM dumps, reach lists)
lands under --out (default build/seeded).
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import playthrough as pt  # noqa: E402

ROOT = pt.ROOT
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import decomp, disc_input  # noqa: E402
RECIPES = os.path.join(HERE, 'seeded')
OUT = os.path.join(ROOT, 'build/seeded')
CHECKPOINTS = os.path.join(ROOT, 'build/checkpoints')
PC_BIN = os.path.join(ROOT, 'pc/build/tokimeki-pc')
DUMP = str(disc_input('disks/dumps/ram.bin'))
RAM_BASE = 0x80000000
PRINT = threading.Lock()

# the block save_game_restore writes (season_no .. mouse_pad_st), the
# game state a card carries (coverage_suite.RESTORE_BLOCK)
RESTORE_BLOCK = (0x800EE79D, 0x800EF6D4)
RAND_SEED = (0x80009010, 4)
RAND_SEED_DEFAULT = 1093
CLOCKS = (4000, 5000)    # frame_counter, vblnk_counter at the seed (see seed_writes)
POKE_MAX = 256          # bytes per poke line (pc/hal/trace.c)
SEED_DELAY = 30         # VBlanks from P to the seed

TITLE_POINT = 'wait seed_point now_step=0x12 now_sub_step=2 now_sub_sub_step=1 within 3000'
ALBUM_POINT = 'wait seed_point now_step=0x34 now_sub_step=1 within 1800'
TRACE = 'trace prev_step:1 work_tarao:4 event_girl_no:4 rpg_bat_arg:4 schedule_cursor:1 now_icon:1'


class SeedError(Exception):
    pass


def log(msg):
    with PRINT:
        print(msg, flush=True)


# ------------------------------------------------------------------ symbols
_SYMS = {}


def symbol_table():
    """name -> (address, size) from config/symbol_names.json"""
    if not _SYMS:
        with open(os.path.join(ROOT, decomp('config/symbol_names.json'))) as fh:
            table = json.load(fh)['symbols']
        for k, v in table.items():
            if ':' not in k and v.get('name'):
                _SYMS.setdefault(v['name'], (int(k, 16), v.get('size') or 4))
    return _SYMS


def resolve(spec):
    """'NAME[:SIZE]' / '0xADDR:SIZE' / 'restore_block' -> (addr, size)"""
    if spec == 'restore_block':
        return RESTORE_BLOCK[0], RESTORE_BLOCK[1] - RESTORE_BLOCK[0]
    name, _, size = spec.partition(':')
    if name.lower().startswith('0x'):
        addr, dsize = int(name, 16), None
    elif name in symbol_table():
        addr, dsize = symbol_table()[name]
    else:
        m = re.fullmatch(r'D_([0-9A-Fa-f]{8})', name)
        if not m:
            raise SeedError(f'{spec}: not in config/symbol_names.json')
        addr, dsize = int(m.group(1), 16), None
    n = int(size, 0) if size else dsize
    if not n:
        raise SeedError(f'{spec}: give the size')
    if not RAM_BASE <= addr <= RAM_BASE + 0x200000 - n:
        raise SeedError(f'{spec}: outside PS1 RAM')
    return addr, n


def check_port_addresses(writes, binary):
    """every seeded name the port binary defines must sit at the PS1
    address (the poke writes the address; a host-side copy would be missed)"""
    import scenario
    syms = scenario.symbols(binary)
    bad = []
    for w in writes:
        name = w['name'].partition(':')[0]
        if name in syms and syms[name] != w['addr'] and w['addr'] != RAND_SEED[0]:
            bad.append(f'{name}: port 0x{syms[name]:08X}, PS1 0x{w["addr"]:08X}')
    if bad:
        raise SeedError('seeded names away from their PS1 address on the port: ' + '; '.join(bad))


# ------------------------------------------------------------------ steps
def redirect_steps():
    """steps goto_step sends through step 2 (prev_step = N): the case
    labels of src/main/matched/step_state_save_and_redirect.c"""
    with open(os.path.join(ROOT, decomp('src/main/matched/step_state_save_and_redirect.c'))) as fh:
        text = fh.read()
    steps = {int(m.group(1)) for m in re.finditer(r'^\s*case (\d+):', text, re.M)}
    if len(steps) < 40:
        raise SeedError('step_state_save_and_redirect.c changed shape: re-derive the entry')
    return steps


def mes_window_state(ram):
    """what mes_window_switch(0) reads: basyo_init_tbl0 (the window count
    - 2), cw3_disp (= class_w[207], window 0's flag byte) and the flag
    bytes class_w[i * 68 + 207] -> (mode, [(addr, byte)])"""
    ma, _ = resolve('basyo_init_tbl0')
    cw, _ = resolve('class_w')
    mode = int.from_bytes(ram[ma - RAM_BASE:ma - RAM_BASE + 2], 'little', signed=True)
    n = max(0, min(mode + 2, 160))
    return mode, [(cw + i * 68 + 207, ram[cw - RAM_BASE + i * 68 + 207]) for i in range(n)]


def entry_writes(step, probe=None):
    """goto_step(STEP)'s stores as (name, addr, bytes, why).  PROBE: the
    RAM at the defined point (a port run of the same prefix): the window
    flag bytes mes_window_switch(0) clears depend on the screen"""
    if not 0 < step < 256:
        raise SeedError(f'step {step}')
    redirect = step in redirect_steps()
    w = []
    a, _ = resolve('step_frame_cnt')
    w.append(dict(name='step_frame_cnt..step_counter', addr=a, data=bytes(12),
                  why='goto_step.c: step_frame_cnt = step_state = step_counter = 0'))
    a, _ = resolve('now_step')
    steps = bytes([2 if redirect else step, 0, 0, 0, 0, 0])
    if redirect:
        steps += bytes([step])
    w.append(dict(name='now_step..' + ('prev_step' if redirect else 'step_mode'), addr=a, data=steps,
                  why=('goto_step.c: now_step = N; now_sub_step = now_sub_sub_step = '
                       'scene_entry_b = scene_entry_c = step_mode = 0'
                       + ('; step_state_save_and_redirect.c: prev_step = N, now_step = 2'
                          if redirect else ''))))
    a, _ = resolve('sound_seek_time')
    w.append(dict(name='sound_seek_time', addr=a, data=bytes(2),
                  why='goto_step.c: sound_seek_time = 0'))
    a, _ = resolve('mes_base_y')
    w.append(dict(name='mes_base_y', addr=a, data=(184).to_bytes(2, 'little'),
                  why='goto_step.c -> mes_window_switch(0) (mes_window_switch.c): mes_base_y = 184'))
    if probe is not None:
        _, flags = mes_window_state(probe)
        if flags and flags[0][1] & 0x80:
            a, _ = resolve('k_disp_switch_tbl0')
            w.append(dict(name='k_disp_switch_tbl0[0]', addr=a, data=bytes(1),
                          why='mes_window_switch.c: cw3_disp & 0x80 -> k_disp_switch(0, 0) '
                              '(k_disp_switch.c: k_disp_switch_tbl0[0] = 0)'))
        for i, (addr, b) in enumerate(flags):
            if b & 0x80:
                w.append(dict(name=f'class_w[{i}*68+207]', addr=addr, data=bytes([b & 0x7F]),
                              why='mes_window_switch.c: class_w[i * 68 + 207] &= 0x7F, '
                                  'i < basyo_init_tbl0 + 2 (the byte read at the defined point)'))
    return w


# ------------------------------------------------------------------ recipes
def load_recipes(path=RECIPES):
    """-> (starts, captures)"""
    starts, caps, seen = [], {}, set()
    for f in sorted(glob.glob(os.path.join(path, '*.json'))):
        with open(f) as fh:
            doc = json.load(fh)
        for cid, c in doc.get('captures', {}).items():
            if cid in caps:
                raise SeedError(f'{f}: duplicate capture {cid}')
            caps[cid] = dict(c, id=cid, file=os.path.relpath(f, ROOT))
        for r in doc.get('starts', []):
            for v in expand(r):
                v['group'] = doc['group']
                v['file'] = os.path.relpath(f, ROOT)
                if v['id'] in seen:
                    raise SeedError(f'{f}: duplicate start {v["id"]}')
                seen.add(v['id'])
                validate(v, caps)
                starts.append(v)
    return starts, caps


def expand(r):
    """a recipe with `variants` ({"suffix": {"fields": [...], ...}}) is one
    start per variant; a variant's fields come after the recipe's"""
    if 'variants' not in r:
        return [dict(r)]
    out = []
    for suffix, v in r['variants'].items():
        x = {k: val for k, val in r.items() if k != 'variants'}
        x['id'] = f'{r["id"]}_{suffix}'
        x['fields'] = list(r.get('fields', [])) + list(v.get('fields', []))
        for k, val in v.items():
            if k != 'fields':
                x[k] = val
        out.append(x)
    return out


def validate(r, caps):
    for k in ('id', 'scene', 'base', 'covers'):
        if k not in r:
            raise SeedError(f'{r.get("id")}: no {k}')
    base = r['base']
    if base.startswith('capture:') and base[8:] not in caps:
        raise SeedError(f'{r["id"]}: base {base}: unknown capture')
    if base not in ('title', 'album') and not base.startswith(('checkpoint:', 'capture:')):
        raise SeedError(f'{r["id"]}: base {base}')
    if base == 'album' and 'album' not in r:
        raise SeedError(f'{r["id"]}: base album needs `album`')
    for s in r.get('seed', []):
        src = s.get('capture') or s.get('dump')
        if s.get('capture') and s['capture'] not in caps:
            raise SeedError(f'{r["id"]}: unknown capture {s["capture"]}')
        if not src or not s.get('ranges') or not s.get('why'):
            raise SeedError(f'{r["id"]}: a seed source needs capture|dump, ranges and why')
    for f in r.get('fields', []):
        if 'name' not in f or 'value' not in f or not f.get('why'):
            raise SeedError(f'{r["id"]}: a hand-built field needs name, value and why (the C)')
        resolve(f['name'])
    inp = r.get('input', {})
    if inp.get('kind', 'explore') not in ('explore', 'script', 'none'):
        raise SeedError(f'{r["id"]}: input kind {inp.get("kind")}')
    entry_writes(r['scene'])


def select(starts, names):
    if not names:
        return starts
    out = [r for r in starts if r['id'] in names or r['group'] in names
           or any(r['id'].startswith(n + '_') for n in names)]
    if not out:
        raise SeedError(f'no start matches {" ".join(names)}')
    return out


def value_bytes(f, size):
    v = f['value']
    if isinstance(v, str) and v.startswith('hex:'):
        b = bytes.fromhex(v[4:])
    else:
        b = int(v, 0).to_bytes(size, 'little', signed=int(v, 0) < 0) if isinstance(v, str) \
            else int(v).to_bytes(size, 'little', signed=v < 0)
    if len(b) != size:
        raise SeedError(f'{f["name"]}: {len(b)} bytes for size {size}')
    return b


def capture_path(out, cid):
    return os.path.join(out, 'captures', cid)


def read_ram(path):
    with open(path, 'rb') as fh:
        ram = fh.read()
    if len(ram) != 0x200000:
        raise SeedError(f'{path}: not a 2 MiB PS1 RAM image')
    return ram


def sha(path_or_bytes):
    if isinstance(path_or_bytes, (bytes, bytearray)):
        return hashlib.sha256(path_or_bytes).hexdigest()
    with open(path_or_bytes, 'rb') as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def seed_writes(r, out, probe=None):
    """the recipe's seed as an ordered list of writes
    dict(name, addr, data, src, why) -- the recipe listing of `show`"""
    w = []
    for s in r.get('seed', []):
        if s.get('capture'):
            d = capture_path(out, s['capture'])
            ram_path = os.path.join(d, 'ram.bin')
            if not os.path.exists(ram_path):
                raise SeedError(f'{r["id"]}: capture {s["capture"]} not made (run `capture`)')
            src = f'capture:{s["capture"]} sha256:{sha(ram_path)[:16]}'
        else:
            ram_path = DUMP if s['dump'] == 'ram.bin' else os.path.join(ROOT, s['dump'])
            src = f'dump:{os.path.relpath(ram_path, ROOT)} sha256:{sha(ram_path)[:16]}'
        ram = read_ram(ram_path)
        for spec in s['ranges']:
            addr, n = resolve(spec)
            o = addr - RAM_BASE
            w.append(dict(name=spec, addr=addr, data=ram[o:o + n], src=src, why=s['why']))
    for f in r.get('fields', []):
        addr, n = resolve(f['name'])
        w.append(dict(name=f['name'], addr=addr, data=value_bytes(f, n), src='hand-built',
                      why=f['why']))
    seed = r.get('rand_seed', RAND_SEED_DEFAULT)
    if seed is not None:
        w.append(dict(name='rand_seed', addr=RAND_SEED[0], data=int(seed).to_bytes(4, 'little'),
                      src='hand-built',
                      why=('the kernel rand seed, both machines alike: srand(vblnk_counter) at '
                           'the title gives 1093 on the reference')))
    if r.get('clocks', True):
        # the scenes reseed rand from the game's clocks: srand(frame_counter)
        # (wipe_disp.c, KANGEI kangei_class_init.c, GEKO geko_scene_init1.c,
        # DATE date_effect_init.c) and srand(vblnk_counter) (title_data_read.c,
        # RPG_BAT rpg_bat_init.c, RENSYU rensyu_main0.c, EN_NICHI
        # en_nichi_init0.c, BUNKASAI's girl pick).  The boot path reaches P
        # at different VBlank counts on the two machines (vblnk_counter 5148
        # / 5154 at one BUNKASAI reseed), so both clocks are set alike.
        for name, v in (('frame_counter', CLOCKS[0]), ('vblnk_counter', CLOCKS[1])):
            a, _ = resolve(name)
            w.append(dict(name=name, addr=a, data=v.to_bytes(4, 'little'), src='hand-built',
                          why=('the clock srand() reseeds from, both machines alike '
                               '(seeded_starts.py seed_writes)')))
    for e in entry_writes(r['scene'], probe):
        w.append(dict(e, src='entry'))
    return w


def poke_lines(writes):
    """poke lines, POKE_MAX bytes each, all on the VBlank of `pre_seed`"""
    out = []
    for wr in writes:
        data = wr['data']
        if wr['addr'] == RAND_SEED[0]:
            chunks = [(wr['addr'], data)]       # written whole (pc/hal/trace.c)
        else:
            chunks = [(wr['addr'] + i, data[i:i + POKE_MAX]) for i in range(0, len(data), POKE_MAX)]
        for a, c in chunks:
            out.append(f'poke 0 0x{a:08X} {c.hex()}'
                       f'   # {wr["name"]} ({wr["src"].split(" ")[0]})')
    return out


# ------------------------------------------------------------------ cards
def blank_card(out):
    path = os.path.join(out, 'cards', 'blank.mcr')
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        subprocess.run([sys.executable, os.path.join(HERE, 'mcr_tool.py'), 'format', path],
                       check=True, stdout=subprocess.DEVNULL)
    return path


def album_card(r, out, binary):
    """a crafted album (checkpoints.craft over the played/template base)"""
    import checkpoints as cp
    path = os.path.join(out, 'cards', r['id'] + '.mcr')
    spec = dict(r['album'])
    spec.setdefault('segment', {'kind': 'week'})
    spec['id'] = r['id']
    bases = cp.ensure_bases(CHECKPOINTS, binary)
    card, _ = cp.craft(spec, bases)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as fh:
        fh.write(card)
    return path


def base_card(r, out, binary):
    base = r['base']
    if base == 'title':
        return blank_card(out)
    if base == 'album':
        return album_card(r, out, binary)
    cid = base.split(':', 1)[1]
    if base.startswith('capture:'):
        # the checkpoint whose run the capture was taken from
        cid = capture_checkpoint(out, cid)
    path = os.path.join(CHECKPOINTS, cid, 'card.mcr')
    if not os.path.exists(path):
        raise SeedError(f'{r["id"]}: {path} missing (checkpoints.py build)')
    return path


def prefix(r):
    import checkpoints as cp
    if r['base'] == 'title':
        return [TITLE_POINT]
    return [cp.LOAD.rstrip(), ALBUM_POINT]


# ------------------------------------------------------------------ scripts
def input_lines(r, seed):
    import checkpoints as cp
    inp = r.get('input', {})
    kind = inp.get('kind', 'explore')
    out = []
    for line in inp.get('lines', []):
        out.append(cp.fragment(line) if line.startswith('@') else line)
    if kind == 'explore':
        import explorer
        out += explorer.generate(f'seeded:{r["id"]}', seed, inp.get('vblanks', 9000))
    return out


def jobs_of(r):
    inp = r.get('input', {})
    if inp.get('kind', 'explore') == 'explore':
        return [(r, s, f'{r["id"]}-s{s}') for s in inp.get('seeds', [1])]
    return [(r, 0, r['id'])]


def script(r, seed, job, writes):
    lines = [f'# SEEDED START {job}: generated by pc/tools/seeded_starts.py ({r["file"]})',
             f'# scene {r["scene"]} ({r.get("overlay", "?")}), base {r["base"]}',
             f'# covers: {"; ".join(r["covers"])}',
             '# a seeded run: state written once at the defined point (seed_point + '
             f'{SEED_DELAY}), never a normal-run result',
             'hold 2', TRACE, f'phase {job}']
    lines += prefix(r)
    # the RAM just before the seed, on both machines (the gate compares
    # the bytes the probe read), then the seed on the same VBlank
    lines.append(f'shot pre_seed {SEED_DELAY}')
    lines += poke_lines(writes)
    lines.append('shot seeded 0')
    expect = r.get('expect') or f'now_step={r["scene"]}'
    lines.append(f'wait entered {expect} within {r.get("enter_within", 3000)}')
    lines += input_lines(r, seed)
    lines.append('end 60')
    return '\n'.join(lines) + '\n'


def max_frames(r):
    inp = r.get('input', {})
    return r.get('max_frames', 9000 + (inp.get('vblanks', 9000) if inp.get('kind', 'explore')
                                       == 'explore' else inp.get('frames', 6000)))


def probe(r, card, out, binary, timeout=900):
    """the RAM at the defined point (P + SEED_DELAY) of a port run of the
    recipe's own prefix: what mes_window_switch(0) would read there.  The
    run's seed gate checks both machines hold the same bytes at that point
    (`pre_seed` shot), so the values written are what the call computes on
    each.  Cached per card and prefix."""
    text = '\n'.join(['hold 2', 'phase probe'] + prefix(r)
                     + [f'shot pre_seed {SEED_DELAY}', 'end 1']) + '\n'
    key = hashlib.sha256(text.encode() + sha(card).encode()).hexdigest()[:16]
    d = os.path.join(out, 'probes', key)
    ram = os.path.join(d, 'shots', 'probe', 'pre_seed.ram')
    if not os.path.exists(ram):
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, 'probe.txt')
        with open(path, 'w') as fh:
            fh.write(text)
        with open(os.path.join(d, 'playthrough.log'), 'w') as fh:
            subprocess.run([sys.executable, os.path.join(HERE, 'playthrough.py'), path,
                            '--bin', binary, '--out', d, '--start-card', card, '--shots',
                            '--vram', '--timeout', str(timeout), '--max-frames', '8000'],
                           stdout=fh, stderr=subprocess.STDOUT)
        if not os.path.exists(ram):
            raise SeedError(f'{r["id"]}: the probe did not reach the defined point ({d})')
    return read_ram(ram), os.path.relpath(ram, ROOT)


def build(starts, out, binary, jobs=2):
    index = []
    cards = {r['id']: base_card(r, out, binary) for r in starts}
    # the probes first, JOBS port runs at a time (each is cached); starts
    # with the same card and prefix (variants) share one probe, run once
    uniq = {}
    for r in starts:
        uniq.setdefault((sha(cards[r['id']]), tuple(prefix(r))), r)
    with ThreadPoolExecutor(max(1, jobs)) as ex:
        list(ex.map(lambda r: probe(r, cards[r['id']], out, binary), uniq.values()))
    probes = {r['id']: probe(r, cards[r['id']], out, binary) for r in starts}
    for r in starts:
        card = cards[r['id']]
        pram, ppath = probes[r['id']]
        writes = seed_writes(r, out, pram)
        check_port_addresses(writes, binary)
        for rr, seed, job in jobs_of(r):
            d = os.path.join(out, 'scripts')
            os.makedirs(d, exist_ok=True)
            path = os.path.join(d, job + '.txt')
            with open(path, 'w') as fh:
                fh.write(script(rr, seed, job, writes))
            pt.parse(path)          # refuses a malformed script now, not mid-run
            index.append(dict(job=job, id=r['id'], group=r['group'], scene=r['scene'],
                              overlay=r.get('overlay'), base=r['base'], seed=seed, card=card,
                              script=path, max_frames=max_frames(r), covers=r['covers'],
                              probe=ppath, probe_mes_window=mes_window_state(pram)[1],
                              writes=[dict(name=w['name'], addr=f'0x{w["addr"]:08X}',
                                           hex=w['data'].hex(), src=w['src'], why=w['why'])
                                      for w in writes]))
        log(f'[seeded] {r["id"]:28s} scene {r["scene"]:3d} {r.get("overlay", ""):9s} '
            f'{len(writes)} writes, {sum(len(w["data"]) for w in writes)} bytes')
    with open(os.path.join(out, 'index.json'), 'w') as fh:
        json.dump(index, fh, indent=1)
    return index


# ------------------------------------------------------------------ capture
def capture(c, out, binary, timeout=1800):
    """RAM at the scene's entry in a real run: the checkpoint segment's own
    input on the port, PC_DBG_CMDS `when COND do ram` (the first main-loop
    boundary where the game's goto_step has run), then quit"""
    d = capture_path(out, c['id'])
    if os.path.exists(os.path.join(d, 'meta.json')):
        return d
    cands = c['checkpoint'] if isinstance(c['checkpoint'], list) else [c['checkpoint']]
    errors = []
    for cid in cands:
        # the week's events follow the kernel rand: a segment that entered
        # the scene on one build may not on another, so a capture names
        # candidates, tried in order (meta.json records the one used)
        try:
            return capture_one(dict(c, checkpoint=cid), d, binary, timeout)
        except SeedError as e:
            errors.append(str(e))
    raise SeedError('; '.join(errors))


def capture_checkpoint(out, cid):
    with open(os.path.join(capture_path(out, cid), 'meta.json')) as fh:
        return json.load(fh)['checkpoint']


def capture_one(c, d, binary, timeout):
    os.makedirs(d, exist_ok=True)
    seg = os.path.join(CHECKPOINTS, c['checkpoint'], 'segment.txt')
    card = os.path.join(CHECKPOINTS, c['checkpoint'], 'card.mcr')
    s = pt.parse(seg)
    inp = os.path.join(d, 'segment.input')
    with open(inp, 'w') as fh:
        fh.write(pt.numeric_phase(s['phases'][0], s['hold'], False))
    work = os.path.join(d, 'work')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(os.path.join(work, 'pc'))
    os.makedirs(os.path.join(work, 'saves'))
    os.symlink(os.path.join(ROOT, 'pc/assets'), os.path.join(work, 'pc/assets'))
    os.symlink(str(disc_input('disks')), os.path.join(work, 'disks'))
    shutil.copy(card, os.path.join(work, 'saves/card1.mcr'))
    ram = os.path.join(d, 'ram.bin')
    cmds = os.path.join(d, 'capture.cmds')
    with open(cmds, 'w') as fh:
        fh.write(f'when {c["when"]} do ram {ram}\nafter 0 echo CAPTURED\nquit 0\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PC_', 'SCENE', 'TOKI_'))}
    env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PC_TURBO='1', PC_INPUT=inp,
               PC_TRACE_FRAMES=str(c.get('max_frames', 80000)), TOKI_INPUT_DEVICE='pad',
               PC_PARITY_DIR=os.path.join(work, 'parity'), PC_PARITY_FRAMES='1',
               PC_DBG_CMDS=cmds)
    from safe_process import run
    logp = os.path.join(d, 'capture.log')
    res = run([binary, os.path.join(ROOT, 'pc/assets')], cwd=work, env=env, timeout=timeout,
              log_path=logp, max_output=pt.PC_LOG_LIMIT)
    with open(logp, errors='replace') as fh:
        text = fh.read()
    shutil.rmtree(work, ignore_errors=True)
    if not os.path.exists(ram) or 'CAPTURED' not in text:
        raise SeedError(f'capture {c["id"]}: `{c["when"]}` never held '
                        f'(rc {res.returncode} {res.reason or ""}; {logp})')
    m = re.findall(r'\[TRACE\] PC_INPUT wait (\S+) reached', text)
    meta = dict(id=c['id'], checkpoint=c['checkpoint'], when=c['when'],
                source_script=os.path.relpath(seg, ROOT), script_sha256=sha(seg),
                card_sha256=sha(card), binary=os.path.relpath(binary, ROOT),
                ram_sha256=sha(ram), milestones_before=m, machine='port',
                note=c.get('note', ''))
    with open(os.path.join(d, 'meta.json'), 'w') as fh:
        json.dump(meta, fh, indent=1)
    log(f'[seeded] capture {c["id"]}: {c["when"]} from {c["checkpoint"]} '
        f'(after {m[-1] if m else "-"}) sha256 {meta["ram_sha256"][:16]}')
    return d


# ------------------------------------------------------------------ run
def seed_gate(d, job, item):
    """the RAM dumps around the seed on both machines (shots `pre_seed`,
    just before it, and `seeded`, just after it, on the same VBlank):
      - every seeded byte in place on both after the seed;
      - the window flag bytes mes_window_switch(0) reads, identical on both
        machines before the seed and equal to the probe's (so the cleared
        values written are what the call computes on each);
      - the restore block (the card's game state) before and after."""
    paths = {(m, lab): os.path.join(d, 'shots' if m == 'pc' else 'shots-retail', job, lab + '.ram')
             for m in ('pc', 'ps1') for lab in ('pre_seed', 'seeded')}
    if not all(os.path.exists(p) for p in paths.values()):
        return None
    ram = {k: read_ram(p) for k, p in paths.items()}
    bad = []
    for w in item['writes']:
        addr = int(w['addr'], 16)
        if addr == RAND_SEED[0]:
            continue                    # the port keeps it in g_psx_rand_seed (traced)
        data = bytes.fromhex(w['hex'])
        o = addr - RAM_BASE
        for side in ('pc', 'ps1'):
            if ram[side, 'seeded'][o:o + len(data)] != data:
                bad.append(f'{side}:{w["name"]}')
    want = [list(x) for x in item.get('probe_mes_window', [])]
    mes = {side: [list(x) for x in mes_window_state(ram[side, 'pre_seed'])[1]]
           for side in ('pc', 'ps1')}
    o0, o1 = RESTORE_BLOCK[0] - RAM_BASE, RESTORE_BLOCK[1] - RAM_BASE

    def block_diff(lab):
        a, b = ram['pc', lab], ram['ps1', lab]
        return sum(1 for i in range(o0, o1) if a[i] != b[i])
    return dict(seeded_bytes_wrong=bad,
                mes_window_same=mes['pc'] == mes['ps1'] == want,
                mes_window=dict(pc=mes['pc'], ps1=mes['ps1'], probe=want),
                restore_block_bytes=o1 - o0,
                restore_block_differing_before=block_diff('pre_seed'),
                restore_block_differing=block_diff('seeded'))


# `run --set NAME=VALUE` / `--profile`: passed to playthrough.py as they are
# (it strips the caller's PC_ / TOKI_ environment and applies these after),
# e.g. `--set TOKI_MUSIC_LOG=...` or `--set PC_DEBUG=sound` for one run.
PORT_ARGS = []


def run_job(item, out, binary, retail, timeout, rebuild):
    import coverage_suite as cs
    job = item['job']
    d = os.path.join(out, 'runs', job)
    res_path = os.path.join(d, 'result.json')
    if os.path.exists(res_path) and not rebuild:
        with open(res_path) as fh:
            return json.load(fh)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    cmd = [sys.executable, os.path.join(HERE, 'playthrough.py'), item['script'], '--bin', binary,
           '--out', d, '--start-card', item['card'], '--timeout', str(timeout),
           '--max-frames', str(item['max_frames']), '--shots', '--vram', '--shot-jobs', '1']
    cmd += PORT_ARGS
    if retail:
        cmd.append('--retail')
    t0 = time.time()
    log(f'[seeded] {job}: running')
    env = {k: v for k, v in os.environ.items() if k != 'REACH_LIST_FILE'}
    with open(os.path.join(d, 'playthrough.log'), 'w') as fh:
        r = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, env=env)
    rep = {}
    if os.path.exists(os.path.join(d, 'report.json')):
        with open(os.path.join(d, 'report.json')) as fh:
            rep = json.load(fh)
    pcm, psm = rep.get('pc', {}), rep.get('retail', {})
    res = dict(job=job, id=item['id'], group=item['group'], scene=item['scene'],
               overlay=item['overlay'], base=item['base'], seed=item['seed'], mode='seeded',
               report_mode=rep.get('mode'), rc=r.returncode, seconds=round(time.time() - t0),
               entered_pc='entered' in pcm, entered_retail=('entered' in psm) if retail else None,
               pc_failures=rep.get('failures', []), retail_failures=rep.get('retail_failures', []),
               folds=sum(rep.get('folds', {}).values()), milestones_pc=len(pcm),
               milestones_retail=len(psm), shots=rep.get('shots', {}))
    crashes = [f for f in res['pc_failures'] if 'crash' in f or 'fault-fold' in f
               or 'process ended' in f]
    res['pc_crashes'] = crashes
    if retail:
        res['seed_gate'] = seed_gate(d, job, item)
        try:
            res['frames'] = compare_entered(d, job, rep)
        except (OSError, ValueError) as e:
            res['frames'] = dict(status=f'error: {e}')
    res['verdict'] = verdict(res, retail)
    res['pass'] = res['verdict'].startswith('PASS')
    with open(res_path, 'w') as fh:
        json.dump(res, fh, indent=1)
    fr = res.get('frames') or {}
    log(f'[seeded] {job}: {res["verdict"]} in {res["seconds"]} s; frames {fr.get("status")} '
        f'{fr.get("rel", "")} {",".join(fr.get("fields", [])[:4])} {fr.get("cls", "")}')
    return res


ALIGN_SPAN = 300     # game frames the alignment of `entered` looks at
ALIGN_MAX = 3        # largest offset tried


def compare_entered(d, job, rep):
    """coverage_suite.frame_compare from `entered`, after aligning the two
    sides' first scene frames.  `entered` is the first VBlank sample that
    shows now_step = N; where that sample falls inside the game frame in
    which the overlay loader hands over differs between the machines (the
    port's VBlank lands at another point of the frame), so one side can
    show its first scene frame one frame earlier while the scene then runs
    the same frames (step_frame_cnt agrees).  The offset (|o| <= 3) whose
    step triples agree best over the first ALIGN_SPAN frames is used and
    reported as align_offset; 0 wins ties."""
    import copy
    import coverage_suite as cs
    import trace_compare as tc
    pcm, psm = rep.get('pc', {}), rep.get('retail', {})
    if 'entered' not in pcm or 'entered' not in psm:
        return dict(status='no-alignment')
    pc_rows = tc.parse_trace(os.path.join(d, f'{job}.pc.trace'), **cs.LIMITS)
    pc = tc.by_game_frame(pc_rows)
    ps_rows = tc.parse_trace(os.path.join(d, f'{job}.ps1.trace'), **cs.LIMITS)
    ps = tc.by_game_frame(ps_rows)
    g0p, g0s = pcm['entered']['game'], psm['entered']['game']

    def steps(r):
        return (r['now_step'], r['now_sub_step'], r['now_sub_sub_step']) if r else None

    def misses(o):
        n = 0
        for rel in range(1, ALIGN_SPAN):
            a, b = steps(pc.get(g0p + rel)), steps(ps.get(g0s + o + rel))
            if a and b and a != b:
                n += 1
        return n
    best = min(range(-ALIGN_MAX, ALIGN_MAX + 1), key=lambda o: (misses(o), abs(o)))
    paths = step_paths(pc_rows, ps_rows, g0p + 1, g0s + best + 1)
    del pc, ps, pc_rows, ps_rows
    rep2 = copy.deepcopy(rep)
    # the hand-over frame itself (2 -> N) is where the sampling phase shows:
    # compare from the next frame on
    rep2['pc']['entered']['game'] = g0p + 1
    rep2['retail']['entered']['game'] = g0s + best + 1
    out = cs.frame_compare(d, job, rep2, start='entered')
    out['align_offset'] = best
    out['step_path'] = paths
    return out


def step_paths(pc, ps, g0p, g0s):
    """the scene's step path on each side from the aligned start: the
    sequence of distinct (step, sub, sub-sub) triples and the frame (from
    the start) each began.  Equal sequences (to the end of the shorter run)
    mean the two machines went through the same screens, at different
    frames: a timing shift.  Where the sequences part, `cause` says whether
    the step before began on different frames on the two sides (`input
    timing`: the pad's presses are placed by VBlank after `entered` on both
    machines, so a scene running a frame later on one side takes a press
    in another step -- the timing track) or on the same frame (`path`: a
    divergence to triage)."""
    def seq(rows, g0):
        out = []
        for r in rows:
            f = r['frame_counter']
            if f < g0:
                continue
            t = (r['now_step'], r['now_sub_step'], r['now_sub_sub_step'])
            if not out or out[-1][0] != t:
                out.append((t, f - g0, r.get('rand_seed'), r.get('vblnk_counter')))
        return out
    a, b = seq(pc, g0p), seq(ps, g0s)
    # the scene's first sub-sub-step can already have moved on one side at
    # the aligned start: start both at the first triple they share (within
    # the first three)
    starts = [(i + j, i, j) for i in range(min(3, len(a))) for j in range(min(3, len(b)))
              if a[i][0] == b[j][0]]
    if starts:
        _, i, j = min(starts)
        a, b = a[i:], b[j:]
    # walk both: a triple only one side's VBlank samples caught (the game
    # passed through it inside one frame) is skipped, and counted
    i = j = skipped = 0
    k = None
    while i < len(a) and j < len(b):
        if a[i][0] == b[j][0]:
            i += 1
            j += 1
        elif j + 1 < len(b) and a[i][0] == b[j + 1][0]:
            j += 1
            skipped += 1
        elif i + 1 < len(a) and a[i + 1][0] == b[j][0]:
            i += 1
            skipped += 1
        else:
            k = (i, j)
            break
    out = dict(transitions_pc=len(a), transitions_retail=len(b), compared=min(i, j),
               transients_skipped=skipped, same=k is None, first_diff=None)
    if k is not None:
        i, j = k
        fd = dict(index=i, pc=list(a[i][0]), retail=list(b[j][0]),
                  pc_frame=a[i][1], retail_frame=b[j][1])
        if i and j:
            fd.update(before=list(a[i - 1][0]), before_pc_frame=a[i - 1][1],
                      before_retail_frame=b[j - 1][1])
            fd['cause'] = 'input timing' if a[i - 1][1] != b[j - 1][1] else 'path'
        else:
            fd['cause'] = 'path'
        if fd['cause'] == 'path' and a[i][2] is not None and a[i][2] != b[j][2]:
            # the branch drew different random numbers: a reseed from a game
            # clock (srand(vblnk_counter) / srand(frame_counter)) that the
            # two machines reached with different VBlank counts
            fd['cause'] = (f'rand (vblnk_counter {a[i][3]} / {b[j][3]})'
                           if a[i][3] != b[j][3] else 'rand')
        out['first_diff'] = fd
    return out


def recompare(out):
    """redo the frame comparison and the verdict of every finished run"""
    for p in sorted(glob.glob(os.path.join(out, 'runs', '*', 'result.json'))):
        d = os.path.dirname(p)
        with open(p) as fh:
            res = json.load(fh)
        rp = os.path.join(d, 'report.json')
        if res.get('entered_retail') is None or not os.path.exists(rp):
            continue
        with open(rp) as fh:
            rep = json.load(fh)
        try:
            res['frames'] = compare_entered(d, res['job'], rep)
        except (OSError, ValueError) as e:
            res['frames'] = dict(status=f'error: {e}')
        res['verdict'] = verdict(res, True)
        res['pass'] = res['verdict'].startswith('PASS')
        with open(p, 'w') as fh:
            json.dump(res, fh, indent=1)


def verdict(res, retail):
    """PASS / state:both (a state problem, not a port defect) / port-only /
    retail-only / gate"""
    pc_ok = res['entered_pc'] and not res['pc_crashes'] and not res['folds']
    if not retail:
        return 'PASS (port only)' if pc_ok and res['rc'] == 0 else 'FAIL port'
    rt_ok = bool(res['entered_retail'])
    gate = res.get('seed_gate')
    if gate and gate['seeded_bytes_wrong']:
        return 'FAIL gate (seeded bytes not in place)'
    if gate and not gate['mes_window_same']:
        return 'FAIL gate (window flags at the defined point differ from the probe)'
    if not res['entered_pc'] and not rt_ok:
        return 'FAIL state:both (neither machine entered the scene)'
    if not pc_ok and rt_ok:
        return 'FAIL port-only' + (' (crash/fold)' if res['pc_crashes'] or res['folds'] else '')
    if pc_ok and not rt_ok:
        return 'FAIL retail-only'
    fr = res.get('frames') or {}
    if fr.get('status') == 'diverged':
        sp = fr.get('step_path') or {}
        if sp.get('same'):
            return f'PASS diverged:{fr["cls"]}, same step path ({sp["compared"]} steps)'
        if sp:
            fd = sp['first_diff']
            return (f'PASS diverged:{fr["cls"]}, step path differs at step {fd["index"]} '
                    f'({fd["cause"]})')
        return f'PASS diverged:{fr["cls"]}'
    return 'PASS same' if fr.get('status') == 'same' else f'PASS ({fr.get("status")})'


def run_reach(item, out, reach_bin, timeout, rebuild):
    job = item['job']
    d = os.path.join(out, 'reach', job)
    done = os.path.join(d, 'done')
    if os.path.exists(done) and not rebuild:
        return
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    lists = os.path.join(out, 'lists')
    os.makedirs(lists, exist_ok=True)
    for old in glob.glob(os.path.join(lists, f'seeded-{job}.*.txt')):
        os.remove(old)
    env = dict(os.environ, REACH_LIST_FILE=os.path.join(lists, f'seeded-{job}.%p.txt'))
    cmd = [sys.executable, os.path.join(HERE, 'playthrough.py'), item['script'], '--bin', reach_bin,
           '--out', d, '--start-card', item['card'], '--timeout', str(timeout),
           '--max-frames', str(item['max_frames'])]
    with open(os.path.join(d, 'playthrough.log'), 'w') as fh:
        r = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, env=env)
    with open(done, 'w') as fh:
        fh.write(f'{r.returncode}\n')
    log(f'[seeded] {job}: reach run rc {r.returncode}')


def align_note(r):
    o = (r.get('frames') or {}).get('align_offset')
    return f' (entered aligned {o:+d})' if o else ''


def summary(out):
    rows = []
    for p in sorted(glob.glob(os.path.join(out, 'runs', '*', 'result.json'))):
        with open(p) as fh:
            rows.append(json.load(fh))
    with open(os.path.join(out, 'summary.json'), 'w') as fh:
        json.dump(dict(mode='seeded', runs=rows), fh, indent=1)
    import coverage_suite as cs
    lines = ['| run (SEEDED) | scene | base | verdict | seed gate | first divergence after entered '
             '| folds |', '|---|---|---|---|---|---|---|']
    counts = {}
    for r in rows:
        v = r['verdict']
        key = v.split(' ')[0] + (' ' + v.split(' ')[1].split(':')[0] if ' ' in v else '')
        counts[key] = counts.get(key, 0) + 1
        g = r.get('seed_gate') or {}
        gate = ('-' if not g else
                ('OK' if not g['seeded_bytes_wrong'] else 'WRONG ' + ','.join(g['seeded_bytes_wrong'][:3]))
                + f'; block {g.get("restore_block_differing", "?")} B')
        lines.append(f'| {r["job"]} | {r["scene"]} {r.get("overlay") or ""} | {r["base"]} | {v} | '
                     f'{gate} | {cs.fmt_frames(r.get("frames"))}{align_note(r)} | {r["folds"]} |')
    head = [f'# Seeded scene starts (SEEDED, not normal runs): {len(rows)} runs', '',
            'Verdicts: ' + ', '.join(f'{k} {n}' for k, n in sorted(counts.items())), '']
    text = '\n'.join(head + lines) + '\n'
    with open(os.path.join(out, 'summary.md'), 'w') as fh:
        fh.write(text)
    return text


def report(out, binary, prior):
    import coverage_report as cr
    lists = os.path.join(out, 'lists')
    res = {}
    for name, inputs in (('seeded', [lists]), ('union', list(prior) + [lists])):
        if name == 'union' and not prior:
            continue
        cr.main(['report', *inputs, '--bin', binary,
                 '--json', os.path.join(out, f'coverage-{name}.json'),
                 '--md', os.path.join(out, f'coverage-{name}.md')])
        with open(os.path.join(out, f'coverage-{name}.json')) as fh:
            res[name] = json.load(fh)
    if prior:
        names, addr_names = cr.binary_functions(binary)
        reached, unresolved, sources = cr.read_inputs(prior, addr_names)
        res['prior'] = cr.build_report(cr.load_universe(), reached, set(names), unresolved, sources)
    groups = sorted(set().union(*(set(r['groups']) for r in res.values())))
    lines = ['| group | ' + ' | '.join(res) + ' | in build |', '|---' * (len(res) + 2) + '|']
    for k in ['live_total', 'live_overlays', 'main_game']:
        s = [res[n]['summary'][k] for n in res]
        lines.append(f'| **{k}** | ' + ' | '.join(f'{x["reached"]} ({x["percent"]}%)' for x in s)
                     + f' | {s[0]["in_build"]} |')
    for g in groups:
        if g.startswith('dead:') or g == 'main:sdk':
            continue
        s = [res[n]['groups'].get(g, {}) for n in res]
        lines.append(f'| {g} | ' + ' | '.join(str(x.get('reached', 0)) for x in s)
                     + f' | {s[0].get("in_build", "?")} |')
    text = '\n'.join(lines) + '\n'
    with open(os.path.join(out, 'coverage-table.md'), 'w') as fh:
        fh.write('# Seeded starts coverage (SEEDED lists alone, and unioned)\n\n' + text)
    return text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=('list', 'show', 'capture', 'build', 'run', 'summary', 'report'))
    ap.add_argument('names', nargs='*')
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--bin', default=PC_BIN)
    ap.add_argument('--reach-bin')
    ap.add_argument('--jobs', type=int, default=2,
                    help='parallel jobs (each runs one emulator or port process at a time)')
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--no-retail', action='store_true')
    ap.add_argument('--rebuild', action='store_true')
    ap.add_argument('--recompare', action='store_true',
                    help='summary: redo the frame comparison and verdicts of finished runs')
    ap.add_argument('--prior', nargs='*', default=[],
                    help='report: earlier report JSONs / list dirs to union with')
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE',
                    help='run: a port setting or switch for every run (playthrough.py --set); '
                         'the port runs without the caller\'s PC_/TOKI_ environment')
    ap.add_argument('--profile', choices=('parity', 'enhanced'),
                    help='run: the port\'s profile (playthrough.py --profile); enhanced is not '
                         'comparable with the reference, use it with --no-retail')
    a = ap.parse_args(argv)
    PORT_ARGS[:] = [x for kv in a.set for x in ('--set', kv)] + (['--profile', a.profile] if a.profile else [])
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    try:
        starts, caps = load_recipes()
        if a.cmd == 'list':
            for r in starts:
                print(f'{r["id"]:30s} {r["group"]:10s} scene {r["scene"]:3d} '
                      f'{r.get("overlay", ""):9s} {r["base"]:22s} {"; ".join(r["covers"])[:70]}')
            print(f'{len(starts)} starts, {len(caps)} captures')
            return 0
        if a.cmd == 'show':
            for r in select(starts, a.names):
                print(json.dumps({k: v for k, v in r.items()}, indent=1))
                b = os.path.abspath(a.bin)
                for w in seed_writes(r, out, probe(r, base_card(r, out, b), out, b)[0]):
                    print(f'  0x{w["addr"]:08X} {len(w["data"]):5d} B  {w["name"]:30s} '
                          f'{w["src"]:34s} {w["why"]}')
            return 0
        if a.cmd == 'capture':
            bad = 0
            for cid, c in sorted(caps.items()):
                if not a.names or cid in a.names:
                    try:
                        capture(c, out, os.path.abspath(a.bin), a.timeout)
                    except SeedError as e:
                        print(f'seeded_starts: {e}')
                        bad += 1
            return 1 if bad else 0
        if a.cmd == 'summary':
            if a.recompare:
                recompare(out)
            print(summary(out))
            return 0
        if a.cmd == 'report':
            print(report(out, os.path.abspath(a.reach_bin or a.bin), a.prior))
            return 0
        chosen = select(starts, a.names)
        need = {s['capture'] for r in chosen for s in r.get('seed', []) if s.get('capture')}
        for cid in sorted(need):
            capture(caps[cid], out, os.path.abspath(a.bin), a.timeout)
        index = build(chosen, out, os.path.abspath(a.bin), a.jobs)
        if a.cmd == 'build':
            return 0
    except SeedError as e:
        print(f'seeded_starts: {e}')
        return 2
    jobs = [('seg', it) for it in index]
    if a.reach_bin:
        jobs += [('reach', it) for it in index]

    def one(job):
        kind, it = job
        try:
            if kind == 'seg':
                run_job(it, out, os.path.abspath(a.bin), not a.no_retail, a.timeout, a.rebuild)
            else:
                run_reach(it, out, os.path.abspath(a.reach_bin), a.timeout, a.rebuild)
        except Exception as e:      # one job's failure must not stop the others
            log(f'[seeded] {it["job"]} ({kind}): {type(e).__name__}: {e}')
    with ThreadPoolExecutor(max(1, a.jobs)) as ex:
        list(ex.map(one, jobs))
    print(summary(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
