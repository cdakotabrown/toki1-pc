#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""gen_lang_hooks.py -- route the English patch's hooks on the PC.

On the console the English patch (config/patch/english.json) overwrites
the first words of each hooked retail function with `j <patch function>`.
The PC runs native C, so the same routing is done at LINK time:

  * main-binary hooks: `ld --wrap=<native name>`.  Every call to the
    function from another object (and every function-pointer reference,
    e.g. pc/src/psx_main_map.c) reaches __wrap_<name>, a 3-instruction stub:

        cmpb $0, g_hal_lang_en(%rip) ; jne lang_en_<name> ; jmp __real_<name>

    Japanese jumps (not calls) to the retail function, so it runs with the
    caller's own return address and arguments: bit-identical, including
    pc/hal/cpu_cost.c's caller-keyed charges.  English jumps to the
    hand-written entry in pc/src/lang_patch_entries.c, which narrows the
    PS1 pointer arguments (hal_ps1_ptr) and calls the patch function.
  * patch symbol pins: config/patch/symbols.json names retail symbols as
    they were called when the patch was written; a name the tree has since
    renamed gets a linker PROVIDE (data: its PS1 address in the RAM block;
    function: the native function at that address).

Every hook and call-site redirect of english.json must have a ROUTE below
(routed, or pending with the reason); the generator fails otherwise, so a
new patch hook cannot be silently missing on the PC.

usage: gen_lang_hooks.py OUTDIR   (writes lang_hooks.c, wrap.flags,
                                   patch_pins.ld, routing.json)
       gen_lang_hooks.py --ovl-filter MODULE.c   (pc/Makefile, in place)

Overlay hooks: the module calls a hooked function inside its own
translation unit, which no link-time wrap reaches.  --ovl-filter rewrites
the adapted module source the build compiles (never pc/src/overlay_*.c):
every reference except the definition becomes <sym>__langhook, a stub of
the same 3 instructions; hal_overlay_lookup uses g_lang_ovl_redirect for
calls that arrive by address.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# The patch profile, its symbol pins and src/patch are toki1-english's, the
# function registry toki1-decomp's (pc/tools/pc_paths.py; pc/Makefile runs
# this only when the English part is present).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pc_paths import DECOMP, ENGLISH, english_path  # noqa: E402
PROFILE = str(english_path('config/patch/english.json')) if ENGLISH is not None else None

# The native name the PC links for a retail address when it differs from
# the registry name (the Sony BIOS thunks: pc/include/game/sony_bios_alias.h).
NATIVE_ALIAS = {0x800B4890: 'func_800B4890', 0x800B48A0: 'func_800B48A0',
                0x800B48B0: 'func_800B48B0'}

# address -> (status, how).  status: 'wrap' (lang_en_<native> in
# pc/src/lang_patch_entries.c), 'callsite' (pc-port code calls the patch
# function itself when English is on), 'pending' (not routed yet: why).
ROUTES = {
    # main hooks
    0x8004EBB0: ('wrap', 'set_kanji_string -> text_vwf.c set_kanji_string'),
    0x8004F690: ('wrap', 'load_string -> text_vwf.c load_string'),
    0x8004F238: ('wrap', 'disp_string -> text_vwf_draw.c disp_string'),
    0x8004F04C: ('wrap', 'k_disp_inc -> pt_k_disp_inc'),
    0x80081398: ('wrap', 'msg_str_disp -> pt_schedule_analyze'),
    0x80082F60: ('wrap', 'msg_part_reorder -> pt_choice_shuffle'),
    0x800512AC: ('wrap', 'sndisp -> pt_field_message'),
    0x80051430: ('wrap', 'sndi -> pt_field_line'),
    0x8005121C: ('wrap', 'gnsx -> pt_speaker_label'),
    0x8007F08C: ('wrap', 'serifu_cmd_exec -> pt_event_message'),
    0x8006625C: ('wrap', 'basyo_init -> pt_basyo_init (class_w handles as PS1 words)'),
    0x80084B14: ('wrap', 'girl_menu_set -> pt_vacation_day_init1'),
    0x8007837C: ('wrap', 'telephone_choice_menu_init -> pt_telephone_choice_menu_init'),
    0x800833DC: ('wrap', 'menu2_init -> pt_menu2_init'),
    0x800831D8: ('wrap', 'menu3_init -> pt_menu3_init'),
    0x80051928: ('wrap', 'get_p_name -> pt_get_p_name'),
    0x800B4890: ('wrap', 'strcpy (BIOS thunk, game objects) -> pt_strcpy'),
    0x800B48A0: ('wrap', 'strcat (BIOS thunk, game objects) -> pt_strcat'),
    0x800B48B0: ('wrap', 'strncat (BIOS thunk, game objects) -> pt_strncat'),
    # main call sites (album_info_disp is a PC override: src/main/pc-port/album_info_disp.c)
    0x80069AA8: ('callsite', 'album_info_disp.c: pt_name_hold'),
    0x80069AD0: ('callsite', 'album_info_disp.c: pt_name_put'),
    0x80069F34: ('callsite', 'album_info_disp.c: pt_name_hold'),
    0x80069F64: ('callsite', 'album_info_disp.c: pt_name_put'),
    0x80069B08: ('callsite', 'album_info_disp.c: pt_strncat_nick'),
    0x80069FA4: ('callsite', 'album_info_disp.c: pt_strncat_nick'),
    # overlay hooks: the overlay modules call these inside ONE translation
    # unit, which a link-time wrap cannot reach.  --ovl-filter rewrites the
    # module's calls (not the definition) to <sym>__langhook, a stub of the
    # same shape; hal_overlay_lookup sends address dispatch the same way.
    ('NAME_ENT', 0x801330E8): ('ovl', 'name_ent_hira_disp -> pt_name_page_latin'),
    ('NAME_ENT', 0x80133E60): ('ovl', 'name_ent_tab_index_set -> pt_name_band'),
    ('NAME_ENT', 0x80134254): ('ovl', 'name_ent_glyph_write -> pt_name_key'),
    ('NAME_ENT', 0x80132B78): ('ovl', 'name_ent_key_cursor_set -> pt_name_cursor'),
    ('NAME_ENT', 0x80138D08): ('ovl', 'name_ent_record_set -> pt_name_commit'),
    ('SHOUGATU', 0x80133B48): ('ovl', 'shougatu_serifu_step -> pt_shougatu_greeting'),
    ('BUNKA_SD', 0x8013320C): ('ovl', 'bunka_sd_rollcall_step -> pt_bunka_sd_rollcall'),
    ('BUNKA_SD', 0x80132FF8): ('ovl', 'bunka_sd_result_step -> pt_bunka_sd_announcer'),
    ('BUNKASAI', 0x801527B0): ('ovl', 'bunkasai_main8 -> pt_bunkasai_photo'),
    ('DATE2', 0x801395A8): ('ovl', 'date2_sched_menu_disp -> pt_date2_lyrics'),
    ('DATE2', 0x801385CC): ('ovl', 'date2_song_menu_init -> pt_date2_song_menu'),
    ('ENDING', 0x8013BE20): ('ovl', 'ending_song_line_disp -> pt_ending_song'),
    # the exam board's name join: one strncat call inside etc_week_rank_disp
    ('ETC', 0x80147BBC): ('ovlsite', 'etc_week_rank_disp: strncat(name, p_mei, 8) -> pt_full_name_cat'),
}


def load_registry():
    fns = json.load(open(os.path.join(DECOMP, 'config/level3_functions.json')))['functions']
    return {int(f['address'], 16): f['name'] for f in fns}


def native_name(reg, addr):
    return NATIVE_ALIAS.get(addr) or reg.get(addr)


def routes(profile, reg):
    """[(key, retail name, native name, target, status, how)] for every hook."""
    out = []
    main = profile['main']
    for h in main['module']['hooks']:
        a = int(h['addr'], 16)
        out.append((a, h['function'], native_name(reg, a), h.get('target', h['function'])))
    for c in main.get('callsites', []):
        a = int(c['at'], 16)
        out.append((a, 'callsite@' + c['at'], None, c['target']))
    for ov, o in profile.get('overlays', {}).items():
        for h in o.get('hooks', []):
            a = int(h['addr'], 16)
            out.append(((ov, a), ov + '/' + h['function'], None, h['target']))
        for c in o.get('callsites', []):
            a = int(c['at'], 16)
            out.append(((ov, a), ov + '/callsite@' + c['at'], None, c['target']))
    res = []
    for key, name, native, target in out:
        if key not in ROUTES:
            raise SystemExit('gen_lang_hooks: english.json hook %s (%s) has no PC route; add one '
                             'to ROUTES (routed or pending with the reason)' % (name, key))
        st, how = ROUTES[key]
        if st == 'wrap' and not native:
            raise SystemExit('gen_lang_hooks: %s: no native function at that address' % name)
        res.append((key, name, native, target, st, how))
    return res


# Sony BIOS names that are also the host C library's: the patch objects are
# compiled with sony_bios_alias.h (they call func_800B48xx), and a PROVIDE
# for these would capture the HAL's libc references.
LIBC_NAMES = {'strcpy', 'strcat', 'strncat', 'strlen', 'strncpy', 'printf', 'memcpy', 'memset'}


def pins_ld(reg):
    pins = json.load(open(english_path('config/patch/symbols.json')))['symbols']
    lines = ['/* GENERATED by pc/tools/gen_lang_hooks.py: the English patch engine\'s',
             ' * pinned retail names (config/patch/symbols.json) that the tree has since',
             ' * renamed.  PROVIDE only defines a name nothing else defines. */']
    for name in sorted(pins):
        a = int(pins[name]['addr'], 16)
        if not re.fullmatch(r'[A-Za-z_]\w*', name) or name in LIBC_NAMES:
            continue
        if a in reg or a in NATIVE_ALIAS:
            nat = native_name(reg, a)
            if nat and nat != name:
                lines.append('PROVIDE(%s = %s);' % (name, nat))
        elif a >= 0x80132000:
            continue            # overlay: the func_/D_ address name resolves itself
        else:
            lines.append('PROVIDE(%s = 0x%08X);' % (name, a))
    # func_<addr> names in the patch C whose main function has since been
    # named (the address is the name, so no pin was ever recorded)
    seen = set()
    src = str(english_path('src/patch/main'))
    for f in sorted(os.listdir(src)):
        if f.endswith('.c'):
            for m in re.finditer(r'\bfunc_([0-9A-Fa-f]{8})\b', open(os.path.join(src, f)).read()):
                a = int(m.group(1), 16)
                nat = native_name(reg, a)
                if a < 0x80132000 and nat and nat != m.group(0) and m.group(0) not in seen:
                    seen.add(m.group(0))
                    # a hooked function: the console's call lands on the
                    # hook, so the alias must reach the wrap stub too
                    if ROUTES.get(a, ('',))[0] == 'wrap':
                        nat = '__wrap_' + nat
                    lines.append('PROVIDE(%s = %s);' % (m.group(0), nat))
    return '\n'.join(lines) + '\n'


SYMTAB = os.path.join(ROOT, 'pc/src/overlay_symtab.c')
SYMTAB_RE = re.compile(r'\{ 0x([0-9A-F]{8})u, \(void \*\)(\w+) \}')

# the ETC call-site redirect: the exact call text inside the module
OVL_SITES = {('ETC', 0x80147BBC): ('ETC_etc_week_rank_disp', 'strncat(name, p_mei, 8)')}


def ovl_symbol(ov, function, addr):
    """The module's native symbol for an overlay hook: OV_<function>, whose
    window (pc/src/overlay_symtab.c) starts at or just before `addr` (a
    head-absorbed window starts up to a few words early)."""
    want = '%s_%s' % (ov, function)
    starts = [int(a, 16) for a, sym in SYMTAB_RE.findall(open(SYMTAB).read()) if sym == want]
    if not any(0 <= addr - a <= 0x40 for a in starts):
        raise SystemExit('gen_lang_hooks: %s/%s: no module function %s at 0x%08X' % (ov, function, want, addr))
    return want


def ovl_hooks(profile):
    """{overlay: [(module symbol, hook addr, patch target)]} for 'ovl' routes."""
    out = {}
    for ov, o in profile.get('overlays', {}).items():
        for h in o.get('hooks', []):
            a = int(h['addr'], 16)
            if ROUTES.get((ov, a), ('',))[0] == 'ovl':
                out.setdefault(ov, []).append((ovl_symbol(ov, h['function'], a), a, h['target']))
    return out


def ovl_filter(path, profile):
    """Rewrite one adapted overlay module source in place: every reference
    to a hooked function except its definition goes to <sym>__langhook
    (a prototype is added beside each existing one); the call-site
    redirects become their stub."""
    ov = os.path.basename(path)[len('overlay_'):-2]
    text = open(path).read()
    for sym, addr, target in ovl_hooks(profile).get(ov, []):
        lines = text.split('\n')
        defs = 0
        for i, line in enumerate(lines):
            if not re.search(r'\b%s\b' % sym, line):
                continue
            head = re.match(r'^[A-Za-z_][\w \*]*\b%s\s*\(' % sym, line)
            if head and not line.rstrip().endswith(';'):
                defs += 1                    # the definition: keep its name
                continue
            if head and line.rstrip().endswith(';'):
                lines[i] = line + '\n' + re.sub(r'\b%s\b' % sym, sym + '__langhook', line)
                continue
            lines[i] = re.sub(r'\b%s\b' % sym, sym + '__langhook', line)
        if defs != 1:
            raise SystemExit('gen_lang_hooks: %s: %d definitions of %s' % (path, defs, sym))
        text = '\n'.join(lines)
    for (sov, addr), (fn, call) in OVL_SITES.items():
        if sov != ov:
            continue
        m = re.search(r'\n[A-Za-z_][\w \*]*\b%s\s*\([^;\n]*\)[^;\n]*\n' % fn, text)
        a = m.start() if m else -1
        b = text.find('\n}', a)
        body = text[a:b]
        if a < 0 or body.count(call) != 1:
            raise SystemExit('gen_lang_hooks: %s: call %r not found once in %s' % (path, call, fn))
        stub = 'lang_ovlsite_%s_%08X' % (sov, addr)
        body = body.replace(call, call.replace('strncat(', stub + '(', 1))
        text = text[:a] + body + text[b:]
        text = 'extern s32 %s();\n' % stub + text
    open(path, 'w').write(text)
    return 0


def main(argv):
    if ENGLISH is None:
        sys.exit('gen_lang_hooks.py: toki1-english not found (../toki1-english, '
                 'TOKI_ENGLISH_DIR): the English hooks need the English patch')
    if len(argv) == 3 and argv[1] == '--ovl-filter':
        return ovl_filter(argv[2], json.load(open(PROFILE)))
    if len(argv) != 2:
        print(__doc__)
        return 2
    outdir = argv[1]
    os.makedirs(outdir, exist_ok=True)
    profile = json.load(open(PROFILE))
    reg = load_registry()
    rs = routes(profile, reg)
    stubs = ['/* GENERATED by pc/tools/gen_lang_hooks.py -- do not edit. */',
             '#include "pc_asm.h"   /* ELF .type/.size vs the Windows COFF spelling */',
             '/* __wrap_<f>: Japanese jumps to the retail function, English to',
             ' * pc/src/lang_patch_entries.c lang_en_<f>. */']
    flags = []
    for key, name, native, target, st, how in rs:
        if st != 'wrap':
            continue
        stubs.append('__asm__(".text\\n.globl __wrap_{n}\\n" PC_ASM_TYPE_FN("__wrap_{n}")\n'
                     '        "__wrap_{n}:\\n\\tcmpb $0, g_hal_lang_en(%rip)\\n\\tjne lang_en_{n}\\n'
                     '\\tjmp __real_{n}\\n" PC_ASM_SIZE("__wrap_{n}"));'.format(n=native))
        flags.append('-Wl,--wrap=%s' % native)
    # overlay hooks: <sym>__langhook (module calls) and the address redirect
    redirect = []
    for ov, hooks in sorted(ovl_hooks(profile).items()):
        for sym, addr, target in hooks:
            ent = 'lang_en_ovl_%s_%08X' % (ov, addr)
            stubs.append('__asm__(".text\\n.globl {s}__langhook\\n" PC_ASM_TYPE_FN("{s}__langhook")\n'
                         '        "{s}__langhook:\\n\\tcmpb $0, g_hal_lang_en(%rip)\\n\\tjne {e}\\n'
                         '\\tjmp {s}\\n" PC_ASM_SIZE("{s}__langhook"));'.format(s=sym, e=ent))
            stubs.append('extern void %s(void), %s__langhook(void);' % (sym, sym))
            redirect.append('    { "%s", 0x%08Xu, (void *)%s, (void *)%s__langhook },' % (ov, addr, sym, sym))
    for (ov, addr), (fn, call) in sorted(OVL_SITES.items()):
        stub = 'lang_ovlsite_%s_%08X' % (ov, addr)
        stubs.append('__asm__(".text\\n.globl {s}\\n" PC_ASM_TYPE_FN("{s}") "{s}:\\n'
                     '\\tcmpb $0, g_hal_lang_en(%rip)\\n\\tjne lang_en_{s}\\n\\tjmp func_800B48B0\\n"\n'
                     '        PC_ASM_SIZE("{s}"));'.format(s=stub))
    stubs += ['/* hal_overlay_lookup (pc/hal/overlay.c): address dispatch to a hooked',
              ' * overlay function goes through its stub as well. */',
              'typedef struct { const char *ov; unsigned addr; void *fn; void *hook; } LangOvlRedirect;',
              'const LangOvlRedirect g_lang_ovl_redirect[] = {'] + redirect + [
              '    { 0, 0, 0, 0 }', '};']
    open(os.path.join(outdir, 'lang_hooks.c'), 'w').write('\n'.join(stubs) + '\n')
    open(os.path.join(outdir, 'wrap.flags'), 'w').write(' '.join(flags) + '\n')
    open(os.path.join(outdir, 'patch_pins.ld'), 'w').write(pins_ld(reg))
    status = [{'hook': name, 'native': native, 'target': target, 'status': st, 'how': how}
              for key, name, native, target, st, how in rs]
    json.dump(status, open(os.path.join(outdir, 'routing.json'), 'w'), indent=1)
    n = {}
    for s in status:
        n[s['status']] = n.get(s['status'], 0) + 1
    print('gen_lang_hooks: %s' % ', '.join('%d %s' % (v, k) for k, v in sorted(n.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
