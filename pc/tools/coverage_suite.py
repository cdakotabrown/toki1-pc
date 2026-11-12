#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checkpoint differential suite (coverage phase 3).

For every checkpoint of pc/tools/checkpoints.py: load its card through the
title's Continue and play its segment on the port and on the reference
(playthrough.py --retail --start-card), then compare

  * the album gate: the traced fields at `album_loaded`, and the RAM dumps
    of the `album_loaded` shot (--shots --vram on both machines): every
    tokimeki_album field and the whole block save_game_restore writes
    (0x800EE79D..0x800EF6D4) -- the card load must give identical state;
  * the per-frame traces from `album_loaded` on, aligned on the game's own
    frame counter: the first frame and field that differ, and its class;
  * milestones, rand branches and fault folds of both runs.

Optionally (--reach-bin) the same segment runs once more on the reach build
(coverage_report.py build-reach) with REACH_LIST_FILE, so
`coverage_report.py report` can union the suite's lists with the baseline.

    python3 pc/tools/coverage_suite.py run [ID|GROUP ...] --out build/coverage-suite \\
        [--jobs 4] [--reach-bin build/coverage/tokimeki-reach] [--no-retail] [--rebuild]
    python3 pc/tools/coverage_suite.py summary --out build/coverage-suite [--recompare]

`run` builds the checkpoints first (checkpoints.py build, cached), skips a
segment whose result.json exists unless --rebuild, and writes
DIR/summary.md and DIR/summary.json (one row per segment: PASS/FAIL, the
album gate, the first divergent frame and field, branches, folds).

Classes of the first divergence:
  rand:schedule  schedule_buf / schedule_cursor first, on the planning step
                 (48): schedule_make's rand placed the week's events
                 differently.  The two machines' kernel rand seeds differ
                 (srand(vblnk_counter) at the title, then a rand call count
                 that follows frame timing; the card does not carry the seed),
                 so this is expected, not a port defect -- it ends the
                 frame-exact comparison, the milestones go on.
  timing         the two machines dropped a different number of frames since
                 the load (VBlanks minus game frames), so the pad's presses
                 -- placed by VBlank on both -- reached different game frames
                 (the hand stops on another icon, a menu takes another line):
                 the timing track (cpu_cost, the drive, the GPU), reported
                 with the first frame the drop counts differ (lag_first)
  rand:path / rand:state
                 the steps or a value differ while the two kernel rand
                 seeds differ (an srand(frame_counter) reseed one game
                 frame apart -- the title's frame offset -- gives seeds one
                 apart: m98_01's New Year fortune, stress 28 / 33)
  path           the step triple differs first, with equal rand seeds
  state          a traced value differs on the same step, equal seeds:
                 to be triaged
Every non-rand class is a triage item.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tokimeki_album as alb  # noqa: E402
import trace_compare as tc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
PC_BIN = os.path.join(ROOT, 'pc/build/tokimeki-pc')
LIMITS = dict(max_rows=120000, max_decoded=1536 * 1024 * 1024)
# not state: clocks, pad, the display buffer, the rand seed (reported apart)
SKIP = {'frame', 'tick', 'crashes', 'frame_counter', 'vblnk_counter', 'step_frame_cnt', 'pad',
        'pad_data', 'pad_trig', 'pad_rel', 'disp_buf', 'rand_seed', 'cd_state2'}
RAND_FIELDS = {'schedule_buf', 'schedule_cursor'}
PRINT = threading.Lock()


def log(msg):
    with PRINT:
        print(msg, flush=True)


def frame_compare(seg_dir, phase, report, start='album_loaded'):
    """first divergence from the START milestone on (album_loaded; a seeded
    start's `entered`, pc/tools/seeded_starts.py), aligned on frame_counter"""
    pcm, psm = report.get('pc', {}), report.get('retail', {})
    if start not in pcm or start not in psm:
        return dict(status='no-alignment')
    pc_t = os.path.join(seg_dir, f'{phase}.pc.trace')
    ps_t = os.path.join(seg_dir, f'{phase}.ps1.trace')
    pc = tc.by_game_frame(tc.parse_trace(pc_t, **LIMITS))
    ps = tc.by_game_frame(tc.parse_trace(ps_t, **LIMITS))
    g0p, g0s = pcm[start]['game'], psm[start]['game']
    out = dict(status='same', pc_start=g0p, retail_start=g0s)
    a0, b0 = pc.get(g0p), ps.get(g0s)
    if a0 and b0:
        out['rand_seed_aligned'] = a0.get('rand_seed') == b0.get('rand_seed')
        out['gate_fields'] = sorted(k for k in a0 if k not in SKIP and k in b0 and a0[k] != b0[k])
    n = 0
    end = min(max(pc) - g0p, max(ps) - g0s)
    lag_first = None
    for rel in range(0, end + 1):
        a, b = pc.get(g0p + rel), ps.get(g0s + rel)
        if not a or not b:
            continue
        n += 1
        # VBlanks the game frame took beyond one, since the load, port minus
        # reference: non-zero once one side dropped a frame the other did not
        lag = (a['frame'] - a0['frame']) - (b['frame'] - b0['frame']) if a0 and b0 else 0
        if lag and lag_first is None:
            lag_first = dict(rel=rel, lag=lag, pc_steps=[a['now_step'], a['now_sub_step'],
                                                          a['now_sub_sub_step']])
        diff = sorted(k for k in a if k not in SKIP and k in b and a[k] != b[k])
        if not diff:
            continue
        steps_a = (a['now_step'], a['now_sub_step'], a['now_sub_sub_step'])
        steps_b = (b['now_step'], b['now_sub_step'], b['now_sub_sub_step'])
        if set(diff) <= RAND_FIELDS and steps_a[0] == 48:
            cls = 'rand:schedule'
        elif lag:
            # the pad's presses land by VBlank on both machines, so after a
            # dropped frame they reach another game frame: the hand stops
            # elsewhere, a menu takes another line (the timing track)
            cls = 'timing'
        elif a.get('rand_seed') != b.get('rand_seed'):
            # the kernel rand differs here: a rand-decided value (a fortune,
            # an event's pick); `path` / `state` need equal seeds
            cls = 'rand:path' if steps_a != steps_b else 'rand:state'
        elif steps_a != steps_b:
            cls = 'path'
        else:
            cls = 'state'
        out.update(status='diverged', rel=rel, pc_game=g0p + rel, retail_game=g0s + rel,
                   fields=diff, cls=cls, pc_steps=list(steps_a), retail_steps=list(steps_b),
                   rand_seed_equal_there=a.get('rand_seed') == b.get('rand_seed'), lag=lag)
        break
    out['lag_first'] = lag_first
    out['frames_compared'] = n
    return out


# the RAM save_game_restore fills (season_no .. mouse_pad_st), and each
# album field inside it by name
RESTORE_BLOCK = (0x800EE79D, 0x800EF6CC + 8)


def ram_gate(seg_dir, phase, label='album_loaded'):
    """the album gate from the 2 MB RAM dumps of the LABEL shot (port:
    PC_PARITY_RAM, reference: -dumpram): the album fields (tokimeki_album.
    FIELDS, by PS1 address) that differ, and the differing byte count over
    the whole block save_game_restore writes.  None without both dumps."""
    pc_p = os.path.join(seg_dir, 'shots', phase, label + '.ram')
    ps_p = os.path.join(seg_dir, 'shots-retail', phase, label + '.ram')
    if not (os.path.exists(pc_p) and os.path.exists(ps_p)):
        return None
    with open(pc_p, 'rb') as fh:
        a = fh.read()
    with open(ps_p, 'rb') as fh:
        b = fh.read()

    def span(addr, n):
        o = addr - 0x80000000
        return a[o:o + n], b[o:o + n]
    fields = []
    for name, (_, fmt, addr, _) in sorted(alb.FIELDS.items()):
        n = fmt if isinstance(fmt, int) else struct.calcsize(fmt)
        x, y = span(addr, n)
        if x != y:
            fields.append(name)
    x, y = span(RESTORE_BLOCK[0], RESTORE_BLOCK[1] - RESTORE_BLOCK[0])
    return dict(fields=fields, block_bytes=len(x),
                block_differing=sum(1 for i in range(len(x)) if x[i] != y[i]))


def run_segment(item, out, binary, retail, timeout, rebuild):
    sid = item['id']
    d = os.path.join(out, 'segments', sid)
    res_path = os.path.join(d, 'result.json')
    if os.path.exists(res_path) and not rebuild:
        with open(res_path) as fh:
            return json.load(fh)
    os.makedirs(d, exist_ok=True)
    cmd = [sys.executable, os.path.join(HERE, 'playthrough.py'), item['segment'], '--bin', binary,
           '--out', d, '--start-card', item['card'], '--timeout', str(timeout),
           '--max-frames', str(item.get('max_frames', 40000))]
    if retail:
        # --shots --vram: RAM dumps at each shot on both machines (the
        # album gate) and the displayed frames (report.json `shots`)
        cmd += ['--retail', '--shots', '--vram']
    t0 = time.time()
    log(f'[suite] {sid}: running')
    env = {k: v for k, v in os.environ.items() if k != 'REACH_LIST_FILE'}
    with open(os.path.join(d, 'playthrough.log'), 'w') as fh:
        r = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, env=env)
    rep_path = os.path.join(d, 'report.json')
    rep = {}
    if os.path.exists(rep_path):
        with open(rep_path) as fh:
            rep = json.load(fh)
    res = dict(id=sid, group=item['group'], date=item['date'], covers=item.get('covers', []),
               rc=r.returncode, seconds=round(time.time() - t0),
               pc_failures=rep.get('failures', []), retail_failures=rep.get('retail_failures', []),
               folds=rep.get('folds', {}), branches=rep.get('branches', {}),
               milestones_pc=len(rep.get('pc', {})), milestones_retail=len(rep.get('retail', {})))
    gate = rep.get('comparison', {}).get('album_loaded')
    res['album_gate'] = gate['differing_fields'] if gate else None
    res['ram_gate'] = ram_gate(d, sid) if retail else None
    res['shots'] = rep.get('shots', {})
    if retail and rep:
        try:
            res['frames'] = frame_compare(d, sid, rep)
        except (OSError, ValueError) as e:
            res['frames'] = dict(status=f'error: {e}')
    gate_ok = res['album_gate'] == [] and (res['ram_gate'] is None or not res['ram_gate']['fields'])
    res['pass'] = (r.returncode == 0 and not res['pc_failures'] and not res['retail_failures']
                   and gate_ok if retail else r.returncode == 0)
    with open(res_path, 'w') as fh:
        json.dump(res, fh, indent=1)
    fr = res.get('frames', {})
    log(f'[suite] {sid}: {"PASS" if res["pass"] else "FAIL"} in {res["seconds"]} s; '
        f'gate {res["album_gate"]}; frames {fr.get("status")} {fr.get("rel", "")} '
        f'{",".join(fr.get("fields", []))} {fr.get("cls", "")}')
    return res


def run_reach(item, out, reach_bin, timeout, rebuild):
    sid = item['id']
    d = os.path.join(out, 'reach', sid)
    done = os.path.join(d, 'done')
    if os.path.exists(done) and not rebuild:
        return
    os.makedirs(d, exist_ok=True)
    lists = os.path.join(out, 'lists')
    os.makedirs(lists, exist_ok=True)
    env = dict(os.environ, REACH_LIST_FILE=os.path.join(lists, f'{sid}.%p.txt'))
    cmd = [sys.executable, os.path.join(HERE, 'playthrough.py'), item['segment'], '--bin', reach_bin,
           '--out', d, '--start-card', item['card'], '--timeout', str(timeout),
           '--max-frames', str(item.get('max_frames', 40000))]
    with open(os.path.join(d, 'playthrough.log'), 'w') as fh:
        r = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, env=env)
    with open(done, 'w') as fh:
        fh.write(f'{r.returncode}\n')
    log(f'[suite] {sid}: reach run rc {r.returncode}')


def fmt_frames(fr):
    if not fr:
        return '-'
    st = fr.get('status')
    if st == 'diverged':
        return (f'+{fr["rel"]} ({fr["pc_game"]}) {",".join(fr["fields"][:3])}'
                f'{"..." if len(fr["fields"]) > 3 else ""} [{fr["cls"]}] '
                f'steps {"/".join(map(str, fr["pc_steps"]))}'
                + ('' if fr['pc_steps'] == fr['retail_steps']
                   else ' vs ' + '/'.join(map(str, fr['retail_steps']))))
    if st == 'same':
        return f'same over {fr.get("frames_compared")} frames'
    return st


def summary(out, recompare=False):
    seg = os.path.join(out, 'segments')
    rows = []
    for sid in sorted(os.listdir(seg)) if os.path.isdir(seg) else []:
        p = os.path.join(seg, sid, 'result.json')
        if os.path.exists(p):
            with open(p) as fh:
                r = json.load(fh)
            rep_path = os.path.join(seg, sid, 'report.json')
            if recompare and os.path.exists(rep_path) and r.get('frames'):
                with open(rep_path) as fh:
                    rep = json.load(fh)
                try:
                    r['frames'] = frame_compare(os.path.join(seg, sid), sid, rep)
                except (OSError, ValueError) as e:
                    r['frames'] = dict(status=f'error: {e}')
                r['ram_gate'] = ram_gate(os.path.join(seg, sid), sid)
                with open(p, 'w') as fh:
                    json.dump(r, fh, indent=1)
            rows.append(r)
    with open(os.path.join(out, 'summary.json'), 'w') as fh:
        json.dump(rows, fh, indent=1)
    lines = ['| segment | date | result | album gate | first divergence (rel game frame) | '
             'branches pc / retail | folds |', '|---|---|---|---|---|---|---|']
    for r in rows:
        fails = r['pc_failures'] + [f'retail: {x}' for x in r['retail_failures']]
        res = 'PASS' if r['pass'] else 'FAIL'
        if fails:
            res += ': ' + '; '.join(fails)[:160]
        gate = 'same' if r['album_gate'] == [] else ('-' if r['album_gate'] is None
                                                     else ','.join(r['album_gate']))
        rg = r.get('ram_gate')
        if rg:
            gate += (f'; RAM {rg["block_differing"]}/{rg["block_bytes"]} B'
                     + (' (' + ','.join(rg['fields']) + ')' if rg['fields'] else ''))
        br = r.get('branches', {})
        brs = ' '.join(f'{k}={v}' for k, v in sorted(br.get('pc', {}).items()))
        brr = ' '.join(f'{k}={v}' for k, v in sorted(br.get('retail', {}).items()))
        folds = sum(r.get('folds', {}).values())
        lines.append(f'| {r["id"]} | {r["date"]} | {res} | {gate} | {fmt_frames(r.get("frames"))} | '
                     f'{brs or "-"} / {brr or "-"} | {folds} |')
    n = len(rows)
    npass = sum(1 for r in rows if r['pass'])
    classes = {}
    for r in rows:
        c = (r.get('frames') or {}).get('cls') or (r.get('frames') or {}).get('status', '-')
        classes[c] = classes.get(c, 0) + 1
    head = [f'# Checkpoint suite: {npass}/{n} PASS', '',
            'First-divergence classes: ' + ', '.join(f'{k} {v}' for k, v in sorted(classes.items())),
            '']
    text = '\n'.join(head + lines) + '\n'
    with open(os.path.join(out, 'summary.md'), 'w') as fh:
        fh.write(text)
    return text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=('run', 'summary'))
    ap.add_argument('names', nargs='*')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/coverage-suite'))
    ap.add_argument('--checkpoints', default=os.path.join(ROOT, 'build/checkpoints'))
    ap.add_argument('--bin', default=PC_BIN)
    ap.add_argument('--reach-bin')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--timeout', type=int, default=1800, help='seconds per phase')
    ap.add_argument('--no-retail', action='store_true')
    ap.add_argument('--rebuild', action='store_true', help='re-run segments that have a result')
    ap.add_argument('--recompare', action='store_true',
                    help='summary: redo the frame comparison and RAM gate of every segment')
    a = ap.parse_args(argv)
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    if a.cmd == 'summary':
        print(summary(out, a.recompare))
        return 0
    r = subprocess.run([sys.executable, os.path.join(HERE, 'checkpoints.py'), 'build', *a.names,
                        '--out', a.checkpoints, '--bin', os.path.abspath(a.bin)])
    if r.returncode:
        return r.returncode
    with open(os.path.join(a.checkpoints, 'index.json')) as fh:
        items = json.load(fh)
    jobs = [('seg', it) for it in items]
    if a.reach_bin:
        jobs += [('reach', it) for it in items]

    def one(job):
        kind, it = job
        try:
            if kind == 'seg':
                run_segment(it, out, os.path.abspath(a.bin), not a.no_retail, a.timeout, a.rebuild)
            else:
                run_reach(it, out, os.path.abspath(a.reach_bin), a.timeout, a.rebuild)
        except Exception as e:      # one job's failure must not stop the others
            log(f'[suite] {it["id"]} ({kind}): {type(e).__name__}: {e}')
    with ThreadPoolExecutor(max(1, a.jobs)) as ex:
        list(ex.map(one, jobs))
    print(summary(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
