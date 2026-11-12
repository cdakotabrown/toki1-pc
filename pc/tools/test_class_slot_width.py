# SPDX-License-Identifier: GPL-3.0-or-later
"""class_w slot writers store at the console's 68-byte layout.

Main-binary writers are src/main/matched/<f>.c with an override in
src/main/pc-port/<f>.c; overlay writers (key `OV/<f>`) are
src/overlay/OV/<f>.c with an override in pc/src/overlay/OV/<f>.c, which
generate_overlay_modules.py substitutes into the overlay's module.

Several matched initialisers declare their own view of a class_w sprite
slot with the three data handles (+12, +16, +52) as native pointers.  On
LP64 that record is 88 bytes, so the matched C strides and lays out the
slots wrongly.  The pc-port overrides write through the PS1-width
ClassSlot (pc/include/game/class_slot.h).

For each FUNC this builds one host binary that holds
  * the MATCHED function compiled as the PC build used to compile it,
    writing into its own class_w (`class_w_wide`, host-width records), and
  * the pc-port OVERRIDE, writing into the 68-byte class_w,
and runs both in forked children from the same seeded global state.

The layout comes from the matched struct itself: every member's offset
and size under -m64 (host) and -m32 (ILP32, the console's layout), read
with offsetof() from the compiler, so no offset table is written by hand.
The oracle is the matched output moved member by member from host to PS1
offsets (a pointer member keeps its low 32 bits; the high half must be
0), with bytes no member covers taken from class_clear_n's image.

  negative control: the matched output read at the 68-byte stride
                    DIFFERS from the oracle (the defect is real);
  fix:              the override's class_w EQUALS the oracle in every
                    scenario, and writes nothing past slot 159.

Since the 2026-11-07 main struct pass, the matched C may ALSO write slots
through the PS1-width view, `((CLASS_W *)class_w)[n].field`.  Those writes
are already at the console's offsets, so the matched build sends them to a
separate 68-byte buffer (`class_w_ps1`), run twice with 0x00 and 0xFF fills:
a byte is written where the two runs agree, and those bytes are laid over
the oracle last (the matched writers do these stores after the slot loop).
"""
import os
from pathlib import Path
import re
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import DECOMP, decomp  # noqa: E402

CFLAGS = ['-std=gnu17', '-fpermissive', '-w', '-funsigned-char',
          '-fno-strict-aliasing', '-O2', '-D_GNU_SOURCE', '-DPC_PORT',
          '-include', 'pc/include/game/types.h', '-I', 'pc/include/game',
          '-I', 'pc/include', '-I', decomp('include')]

# function -> (matched slot typedef, scenarios, per-scenario state set-up
#              [, options]); options: 'base' = the symbol the records are
# indexed from when it is not class_w (cw96_active = class_w slot 96),
# 'stubs' = C definitions of callees whose result must be deterministic or
# that fill caller buffers (the generic stub is a function of its first two
# integer arguments only).
RAND = ('int rand(void) { static unsigned s = 7u; s = s * 1103515245u + 12345u;'
        ' return (int)((s >> 16) & 0x7fff); }')
# a pointer cell: the matched C reads 8 bytes, the override 4; keep the
# high half zero so both read the same PS1 handle
CELL = '#define CELL(sym, v) (*(unsigned long *)(sym) = (unsigned long)(v))\n'
FUNCS = {
    'config_screen_init': ('Slot', 48,
                           'now_month[0] = 1 + k % 12;'
                           ' now_year[0] = 95 + (k / 12) % 4;'),
    # icon_mode/icon_num alias screen_w+0xF6C/+0xF6D on the console
    # (layout_aliases.ld); here they are separate, so keep them in step.
    'icon_init': ('Slot68', 6,
                  'icon_mode[0] = screen_w[3948] = k % 3;'
                  ' icon_num[0] = screen_w[3949] = 16;'
                  ' if (k >= 3) memset(icon_xy, 0, 64);'),
    'parameter_show_init': ('SpriteEntry', 8, ''),
    'schedule_mark_on': ('Chr68', 16, ''),
    'NAME_ENT/name_ent_grid_init': ('Rec68', 4,
                                    'card_sys_present[0] = k & 1;',
                                    {'base': 'cw96_active'}),
    # k % 4 == 3: one validated table word is wrong -> the early return
    'DATE/date_spring_init': (
        'DateObj', 8,
        'CELL(D_8015B5EC, t_ec); CELL(D_8015B5F0, t_f0);'
        ' CELL(D_8015B5FC, t_fc); CELL(D_8015B600, t_600);'
        ' CELL(D_8015B5F4, 0x801E1234u); CELL(D_8015B5F8, 0x801E5678u);'
        ' CELL(D_8015B60C, 0x801F1000u); CELL(D_8015B610, 0x801F2000u);'
        ' if (k % 4 == 3) t_fc[2] = 0;'
        ' date_pattern[0] = (k & 1) ? 2 : (u8)k;',
        {'stubs': RAND + '\n' + CELL +
         'static int t_ec[1] = {0x801E4000};'
         ' static int t_f0[2] = {0x801E8000, 0x801EC000};'
         ' static int t_fc[4] = {0x801F0540, 0x801F0558, 0x801F0570, 0x801F058C};'
         ' static int t_600[2] = {0x801F0B64, 0x801F0B80};'}),
    'OPTION/option_screen_init': ('OptEnt', 4, '', {'stubs': RAND}),
    'TEL/tel_girl_status_init': (
        'Rec', 8, '',
        {'stubs': 'int abs(int x) { return x < 0 ? -x : x; }\n'
                  'void get_grp(u8 *score, short *rank) { int i;'
                  ' for (i = 0; i < 16; i++) { score[i] = (u8)((i * 13 + scen_k * 7) % 80);'
                  ' rank[i] = (i + scen_k) % 5 ? i * 40 : 600; } }'}),
    'TAIIKU/taiiku_sprite_init': ('Rec68', 4, ''),
    'TAIIKU/taiiku_plate_init': ('Actor', 6,
                                 'for (s = 0; s < 4; s++)'
                                 ' D_80149BC4[s] = (u8)((k + s) % (k < 3 ? 4 : 5));'),
    'TAIIKU/taiiku_sprite_init2': ('Rec', 4, 'D_8014A300[0] = k & 1;'),
}

# .set-aliased PS1 addresses (pc/src/ps1_addr_aliases.c, linked in).  The
# harness maps the console's 2 MB of RAM at 0x80000000 (seeded), so these
# handles -- and PS1 addresses a writer stores and reads back, e.g.
# name_ent_grid_init's tpage table -- can be dereferenced.
ALIASED = set(re.findall(r'\.globl\s+(\w+)',
                         (ROOT / 'pc/src/ps1_addr_aliases.c').read_text()))
LIBC = {'class_clear_n', 'class_clear_n_wide', 'memcpy', 'memset', 'strlen'}

MAIN = r"""
extern void FUNC(void);
extern void FUNC_matched(void);
extern void class_clear_n(int);

static u8 clr_img[68];
__attribute__((aligned(16))) u8 class_w_ps1[160 * 68 + 4096];
static int cleared_w[160];
static u8 *shared;
int scen_k;
STUBS

/* Every other global the function touches, and PS1 RAM, must end up the
 * same in both runs. */
#define SNAPSZ (NDATA * 65536 + 0x200000)
static void snap(u8 *dst)
{
    int i;
    for (i = 0; i < NDATA; i++) memcpy(dst + i * 65536, DATA[i], 65536);
    memcpy(dst + NDATA * 65536, (void *)0x80000000UL, 0x200000);
}

/* Host-width twin of class_clear_n for the matched build: class_clear_n's
 * PS1 image moved member by member to the matched struct's host offsets. */
void class_clear_n_wide(int n)
{
    int k;
    if (n >= 160) return;
    cleared_w[n] = 1;
    for (k = 0; k < NMAP; k++) {
        u8 *d = class_w_wide + n * HSZ + MAP[k][2];
        memset(d, 0, MAP[k][3]);
        memcpy(d, clr_img + MAP[k][0], MAP[k][1]);
    }
}

static void run(int k, int which, int fill)
{
    pid_t p = fork();
    if (p == 0) {
        int s;
        seed(1000u + (unsigned)k);
        scen_k = k;
        SCENARIO
        memset(class_w, 0, sizeof(class_w));
        memset(class_w_wide, 0, sizeof(class_w_wide));
        memset(class_w_ps1, fill, sizeof(class_w_ps1));
        if (which) {
            FUNC();
            memcpy(shared, class_w, sizeof(class_w));
            snap(shared + OUTSZ + SNAPSZ);
        } else {
            FUNC_matched();
            memcpy(shared + sizeof(class_w), class_w_wide, sizeof(class_w_wide));
            memcpy(shared + sizeof(class_w) + sizeof(class_w_wide), cleared_w,
                   sizeof(cleared_w));
            memcpy(shared + OUTSZ + 2 * SNAPSZ + (fill ? sizeof(class_w_ps1) : 0),
                   class_w_ps1, sizeof(class_w_ps1));
            if (!fill) snap(shared + OUTSZ);
        }
        (void)s;
        _exit(0);
    }
    waitpid(p, 0, 0);
}

int main(void)
{
    int k, s, m, i, raw_bad = 0, ovr_bad = 0, hi_bad = 0, data_bad = 0;
    static u8 oracle[160 * 68];
    shared = mmap(0, OUTSZ + 2 * SNAPSZ + 2 * sizeof(class_w_ps1), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mmap((void *)0x80000000UL, 0x200000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
        != (void *)0x80000000UL) {
        printf("no PS1 RAM mapping\n");
        return 1;
    }
    memset(class_w, 0, sizeof(class_w));
    class_clear_n(1);
    memcpy(clr_img, class_w + 68, 68);
    for (k = 0; k < NSCEN; k++) {
        u8 *nar = shared, *wid = shared + sizeof(class_w);
        int *cw = (int *)(shared + sizeof(class_w) + sizeof(class_w_wide));
        int sc_raw = 0, sc_ovr = 0;
        u8 *pa, *pb;
        memset(shared, 0, OUTSZ + 2 * SNAPSZ + 2 * sizeof(class_w_ps1));
        run(k, 0, 0xFF);
        run(k, 0, 0);
        run(k, 1, 0);
        pa = shared + OUTSZ + 2 * SNAPSZ;
        pb = pa + sizeof(class_w_ps1);
        for (s = 0; s < 160; s++) {
            u8 *o = oracle + s * 68;
            if (cw[s]) memcpy(o, clr_img, 68); else memset(o, 0, 68);
            for (m = 0; m < NMAP; m++) {
                memcpy(o + MAP[m][0], wid + s * HSZ + MAP[m][2], MAP[m][1]);
                for (i = MAP[m][1]; i < MAP[m][3]; i++)
                    if (wid[s * HSZ + MAP[m][2] + i]) hi_bad++;
            }
        }
        for (i = 0; i < 160 * 68; i++)
            if (pa[i] == pb[i]) oracle[i] = pa[i];
        for (i = 0; i < 160 * 68; i++) {
            if (wid[i] != oracle[i]) sc_raw++;
            if (nar[i] != oracle[i]) {
                if (!sc_ovr) printf("scenario %d: slot %d +%d is %02x, console %02x\n",
                                    k, i / 68, i % 68, nar[i], oracle[i]);
                sc_ovr++;
            }
        }
        for (i = 160 * 68; i < 160 * 68 + 4096; i++) if (nar[i]) sc_ovr++;
        if (memcmp(shared + OUTSZ, shared + OUTSZ + SNAPSZ, SNAPSZ)) {
            for (i = 0; i < SNAPSZ; i++)
                if (shared[OUTSZ + i] != shared[OUTSZ + SNAPSZ + i]) break;
            printf("scenario %d: other data differs at snapshot +%d\n", k, i);
            data_bad++;
        }
        raw_bad += sc_raw != 0;
        ovr_bad += sc_ovr != 0;
        if (k == 0) printf("scenario 0: matched %d bytes differ, override %d\n", sc_raw, sc_ovr);
    }
    printf("RESULT scenarios=%d raw_bad=%d ovr_bad=%d hi_bad=%d data_bad=%d\n",
           NSCEN, raw_bad, ovr_bad, hi_bad, data_bad);
    return 0;
}
"""


def sh(cmd):
    r = run(cmd, cwd=ROOT, timeout=120)
    if not r.ok:
        raise RuntimeError('%s\n%s' % (' '.join(map(str, cmd)), r.stdout[-3000:]))
    return r.stdout


def struct_text(src, tname):
    """The slot typedef plus any typedef it names (config's Fix)."""
    defs = [(m.group(1), m.group(0)) for m in re.finditer(
        r'typedef (?:struct|union) \{(?:[^{}]|\{[^{}]*\})*\} (\w+);', src)]
    main = [t for n, t in defs if n == tname][0]
    deps = [t for n, t in defs if n != tname and re.search(r'\b%s\b' % n, main)]
    return '\n'.join(deps + [main])


def members(text, tname):
    body = re.search(r'typedef struct \{((?:[^{}]|\{[^{}]*\})*)\} %s;' % tname,
                     text).group(1)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    names = []
    for decl in filter(None, (d.strip() for d in body.split(';'))):
        for d in decl.split(None, 1)[1].split(','):
            names.append(re.match(r'\**\s*(\w+)', d.strip()).group(1))
    return names


def layout(tmp, text, tname, names, arch):
    lines = ['#include <stddef.h>', text]
    for n in names:
        lines.append('unsigned int O_%s = offsetof(%s, %s);' % (n, tname, n))
        lines.append('unsigned int S_%s = sizeof(((%s *)0)->%s);' % (n, tname, n))
    lines.append('unsigned int O__size = sizeof(%s);' % tname)
    src = tmp / ('lay_%s.c' % arch)
    src.write_text('\n'.join(lines) + '\n')
    asm = sh(['gcc', '-std=gnu17', '-fno-zero-initialized-in-bss', '-' + arch,
              '-S', '-o', '-', '-include', 'pc/include/game/types.h', str(src)])
    vals = re.findall(r'^([OS]_\w+):\n\s+\.(long|zero)\s+(\d+)', asm, re.M)
    return {k: (int(v) if t == 'long' else 0) for k, t, v in vals}


def undefined(obj):
    """Undefined (functions, data) of obj, split by relocation type."""
    funcs, data = set(), set()
    for typ, sym in re.findall(r'(R_X86_64_\w+)\s+(\w+)', sh(['objdump', '-r', obj])):
        if not sym.startswith('.'):
            (funcs if typ == 'R_X86_64_PLT32' else data).add(sym)
    defined = set(re.findall(r'\s[TDBR]\s(\w+)$', sh(['nm', obj]), re.M))
    return funcs - defined, data - defined - funcs


def paths(key):
    """(function name, matched source, override source) of a FUNCS key"""
    if '/' in key:
        ov, func = key.split('/')
        return (func, decomp('src/overlay/%s/%s.c' % (ov, func)),
                'pc/src/overlay/%s/%s.c' % (ov, func))
    return key, decomp('src/main/matched/%s.c' % key), 'src/main/pc-port/%s.c' % key


def oracle(key, tmp):
    tname, nscen, scenario = FUNCS[key][:3]
    opts = FUNCS[key][3] if len(FUNCS[key]) > 3 else {}
    base = opts.get('base', 'class_w')
    stubs = opts.get('stubs', '')
    func, msrc, osrc = paths(key)
    src = (ROOT / msrc).read_text()
    text = struct_text(src, tname)
    names = members(text, tname)
    o64 = layout(tmp, text, tname, names, 'm64')
    o32 = layout(tmp, text, tname, names, 'm32')
    rows = ['{%d, %d, %d, %d}' % (o32['O_' + n], o32['S_' + n],
                                  o64['O_' + n], o64['S_' + n]) for n in names]
    mo, oo = str(tmp / 'matched.o'), str(tmp / 'override.o')
    # the PS1-width CLASS_W view writes console offsets: keep it off the
    # host-width buffer (class_w -> class_w_wide below)
    msrc_t = tmp / ('m_' + Path(msrc).name)
    mtext = (ROOT / msrc).read_text()
    mview = re.sub(r'\(\s*CLASS_W\s*\*\s*\)\s*class_w\b', '(CLASS_W *)class_w_ps1', mtext)
    if mview != mtext:
        mview = 'extern unsigned char class_w_ps1[];\n' + mview
    msrc_t.write_text(mview)
    sh(['gcc'] + CFLAGS + ['-I', str(ROOT / Path(msrc).parent),
                           '-D%s=%s_matched' % (func, func), '-c', str(msrc_t), '-o', mo])
    sh(['objcopy', '--redefine-sym', '%s=class_w_wide' % base,
        '--redefine-sym', 'class_clear_n=class_clear_n_wide', mo])
    sh(['gcc'] + CFLAGS + ['-c', osrc, '-o', oo])
    if base != 'class_w':
        sh(['objcopy', '--redefine-sym', '%s=class_w' % base, oo])
    f1, d1 = undefined(mo)
    f2, d2 = undefined(oo)
    own = set(re.findall(r'\b(\w+)\s*\([^;{}()]*\)\s*\{', stubs))
    funcs = (f1 | f2) - LIBC - own
    data = (d1 | d2) - ALIASED - funcs - own - {'class_w', 'class_w_wide', 'class_w_ps1'}
    h = ['#include <stdio.h>', '#include <string.h>', '#include <sys/mman.h>',
         '#include <sys/wait.h>', '#include <unistd.h>',
         'typedef unsigned char u8;',
         '#define HSZ %d' % o64['O__size'], '#define NSCEN %d' % nscen,
         'static const int MAP[][4] = {%s};' % ', '.join(rows),
         '#define NMAP %d' % len(rows),
         '__attribute__((aligned(16))) u8 class_w[160 * 68 + 4096];',
         '__attribute__((aligned(16))) u8 class_w_wide[160 * HSZ + 4096];']
    h += ['__attribute__((aligned(16))) u8 %s[65536];' % s for s in sorted(data)]
    h.append('static u8 *const DATA[] = {%s};' % ', '.join(sorted(data) + ['0']))
    h.append('#define NDATA %d' % len(data))
    h.append('#define OUTSZ (sizeof(class_w) + sizeof(class_w_wide) + 4096)')
    # Callees: deterministic small results from the arguments.
    h += ['long %s(long a, long b, long c, long d) '
          '{ return (short)((a * 7 + b * 3 + 1) & 0x1f); }' % s for s in sorted(funcs)]
    h.append('static void seed(unsigned s) { int i;')
    h += ['  for (i = 0; i < 65536; i++) { s = s * 1103515245u + 12345u; '
          '%s[i] = (u8)(s >> 16); }' % s for s in sorted(data)]
    h.append('  for (i = 0; i < 0x200000; i++) { s = s * 1103515245u + 12345u; '
             '((u8 *)0x80000000UL)[i] = (u8)(s >> 16); }')
    h.append('}')
    h.append(MAIN.replace('FUNC', func).replace('SCENARIO', landed_spelling(key.split('/')[0] if '/' in key else '', scenario))
             .replace('STUBS', stubs))
    hc = tmp / 'harness.c'
    hc.write_text('\n'.join(h) + '\n')
    ccn, alias, exe = str(tmp / 'ccn.o'), str(tmp / 'alias.o'), str(tmp / 'oracle')
    sh(['gcc'] + CFLAGS + ['-c', decomp('src/main/matched/class_clear_n.c'), '-o', ccn])
    sh(['gcc'] + CFLAGS + ['-c', 'pc/src/ps1_addr_aliases.c', '-o', alias])
    sh(['gcc', '-O0', '-w', '-no-pie', '-o', exe, str(hc), mo, oo, ccn, alias])
    out = sh([exe])
    m = re.search(r'RESULT scenarios=(\d+) raw_bad=(\d+) ovr_bad=(\d+) hi_bad=(\d+)'
                  r' data_bad=(\d+)', out)
    return o32['O__size'], o64['O__size'], tuple(map(int, m.groups())), out



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

class ClassSlotWidth(unittest.TestCase):
    def test_overrides_match_console_layout(self):
        for func in FUNCS:
            with self.subTest(func=func), tempfile.TemporaryDirectory() as t:
                ps1, host, (n, raw_bad, ovr_bad, hi_bad, data_bad), out = \
                    oracle(func, Path(t))
                self.assertEqual(ps1, 68, func)
                self.assertGreater(host, 68, func)          # the defect exists
                self.assertGreater(raw_bad, 0, out)          # negative control
                self.assertEqual(ovr_bad, 0, out)            # the fix
                self.assertEqual(hi_bad, 0, out)
                self.assertEqual(data_bad, 0, out)           # nothing else changes


if __name__ == '__main__':
    unittest.main()
