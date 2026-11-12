# SPDX-License-Identifier: GPL-3.0-or-later
"""Pin an overlay file's definition of a PS1 object to its PS1 address.

An overlay C file may DEFINE a global that lives in the overlay's own data
(`CamCE D_8015015C_s = {0, 0};`, `DateAngles D_8015DDB0[11] = {0};`).  On
the console that definition is the EXN's bytes at that address; on the PC
the module's names resolve into the emulated PS1 RAM, where the CD HAL
loads the same EXN (pc/ld/psx_globals.syms, --just-symbols).  A name the
.syms file carries is pinned there by the linker even though the module
defines it.  Two spellings are not:

  * `<name>__def`: generate_overlay_modules.py renames a definition whose
    type is file-local and aliases the real name to it; the real name
    resolves into PS1 RAM, the `__def` object stays a host variable, so
    the defining file and every other file's view see different memory;
  * an address-named VIEW (`D_8015015C_s`, `D_8015015C_v`): no .syms entry,
    so the definition is a host variable next to the PS1 object the other
    files use.

pin_definition() turns such a definition into an `extern` declaration and
equates the symbol to D_<ADDR> (`.set`, local to the module), so every
spelling is the PS1 object.  It does so only for an all-zero initializer
(the EXN bytes at those addresses are zero; a non-zero initializer would
have to be checked against the EXN and is left alone, reported to the
caller).
"""
import re

DEFN = r'^(?!extern\b|typedef\b|return\b|#)(static[ \t]+)?([A-Za-z_](?:[\w \*]|\{[^{}]*\})*?)[ \t]*\b(%s)[ \t]*((?:\[[^\]]*\][ \t]*)*)[ \t]*(=|;)'
# a view's name carries its address: D_8015015C_s, D_8015015C_v, srn_w_a
VIEW_NAME = re.compile(r'^D[A-Za-z]*_(8[0-9A-Fa-f]{7})(?:_\w+|[a-z]\w*)$')
ZERO_INIT = re.compile(r'^[\s{},0]*$')


def _mask_comments(text):
    return re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)


def view_address(name):
    """The PS1 address an address-named view carries, or None."""
    m = VIEW_NAME.match(name)
    return int(m.group(1), 16) if m else None


def find_definition(content, name):
    """(start, end, declarator text, initializer text or None) of the
    file-scope definition of `name`, or None."""
    code = _mask_comments(content)
    for m in re.finditer(DEFN % re.escape(name), code, re.M):
        depth, i = 0, m.start()
        while i < len(code):
            if code[i] in '{(':
                depth += 1
            elif code[i] in '})':
                depth -= 1
            elif code[i] == ';' and depth == 0:
                break
            i += 1
        decl = content[m.start():m.end(4)].rstrip()
        init = content[m.end():i] if m.group(5) == '=' else None
        return m.start(), i + 1, decl, init
    return None


def pin_definition(content, name, addr):
    """content with `name`'s definition made an extern pinned at addr.
    Returns (content, pinned)."""
    found = find_definition(content, name)
    if not found:
        return content, False
    start, end, decl, init = found
    if init is not None and not ZERO_INIT.match(init):
        return content, False
    decl = re.sub(r'^static[ \t]+', '', decl)
    # a LOCAL equate of the PS1 address name (which psx_globals.syms
    # resolves): references assemble as relocations against D_<ADDR>; an
    # absolute value known in this TU would be encoded as a sign-extended
    # disp32, which cannot reach 0x80000000
    pinned = ('extern %s; /* the PS1 object at 0x%08X (EXN data) */\n'
              '__asm__(".set %s, D_%08X");'
              % (decl, addr, name, addr))
    return content[:start] + pinned + content[end:], True


def externalize_definition(content, name, addr):
    """content with the file-scope definition of the PS1 name `name` (the
    D_<ADDR> a pinned view equates to) turned into an extern declaration.
    The `.set` of a pinned view resolves against a definition in its own
    module: with one there, the view was the module's host copy on Linux
    (the assembler binds the local equate to the local definition) while
    every reference to the name went to PS1 RAM, and on Windows both were
    the host copy (pc/tools/pe_rebind_relocs.py).  Only an all-zero
    initializer is externalized.  Returns (content, changed)."""
    found = find_definition(content, name)
    if not found:
        return content, False
    start, end, decl, init = found
    if init is not None and not ZERO_INIT.match(init):
        return content, False
    decl = re.sub(r'^static[ \t]+', '', decl)
    text = ('extern %s; /* the PS1 object at 0x%08X (EXN data): psx_globals.syms; '
            'a definition here would capture the pinned views */' % (decl, addr))
    return content[:start] + text + content[end:], True


def pin_view_definitions(content):
    """Pin every file-scope definition of an address-named view (a module's
    own D_<ADDR> definitions: externalize_pinned_bases).
    Returns (content, [names pinned], [names left: non-zero initializer])."""
    pinned, left = [], []
    code = _mask_comments(content)
    names = sorted({m.group(3) for m in re.finditer(DEFN % r'D[A-Za-z]*_8[0-9A-Fa-f]{7}\w+', code, re.M)
                    if view_address(m.group(3)) is not None})
    for name in names:
        content, ok = pin_definition(content, name, view_address(name))
        (pinned if ok else left).append(name)
    return content, pinned, left


PIN = re.compile(r'__asm__\("\.set \w+, D_(8[0-9A-F]{7})"\)')


def externalize_pinned_bases(module, landed=None):
    """A whole overlay module (pc/src/overlay_<OV>.c): every D_<ADDR> that a
    pinned view or __def equates to and that some file of the module
    defines (ETC: etc_main1 pins D_8015015C_s/_v, etc_model_init2 defines
    D_8015015C[2]) becomes an extern -- see externalize_definition.
    `landed` maps an address to the real names the naming track landed for
    it (config/symbol_names.json): after a rename the module defines the
    object under that name (ETC: etc_roll_rot = 0x8015015C), and it must be
    externalized just the same.
    Returns (module, [names externalized], [names left: non-zero init])."""
    done, left = [], []
    for addr in sorted({int(m.group(1), 16) for m in PIN.finditer(_mask_comments(module))}):
        for name in ['D_%08X' % addr] + sorted((landed or {}).get(addr, ())):
            module, ok = externalize_definition(module, name, addr)
            if ok:
                done.append(name)
            elif find_definition(module, name):
                left.append(name)
    return module, done, left
