#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate per-overlay unified C modules for the PC port.

Each overlay gets a single .c file that:
1. Includes all overlay functions
2. Renames overlay-local function calls to use the overlay prefix
3. Exports one entry function: overlay_<NAME>_entry()
4. Renames local typedefs and struct tags to be unique per function
5. Declares each global once (its most common shape) and gives a file that
   declares another shape a typed view of the same object (EXTERN_DECL_RE);
   a global DEFINED by a file with a file-local type is defined as
   <name>__def with the real name an alias of it
6. Declares each external function once, prototype-less, and each module
   function once; a file whose own prototype differs calls through a cast
   view of the callee (PROTO_RE), and a file calling a module function with
   no prototype at all keeps its implicit-declaration contract the same way
7. Preserves #define D_xxx macros

The rules are name-agnostic: a landed name from config/symbol_names.json
gets exactly the treatment its D_/func_ address name had (2026-10-15; the
earlier D_/func_-pattern rules broke the module after the renames).

Usage:
    python3 pc/tools/generate_overlay_modules.py            # every overlay + the header
    python3 pc/tools/generate_overlay_modules.py RENSYU     # only these overlays (header untouched)

Overlay functions may carry a real name (tools/overlay/apply_overlay_symbols.py
lands them; src/overlay/<OV>/<name>.c): their address comes from the overlay's
name table and the per-overlay prefix is applied to the real name
(`<OV>_<name>`).

Output:
    pc/src/overlay_<NAME>.c for each overlay
    pc/include/overlay_entries.h with all entry declarations
"""

import os
import re
import sys
import tempfile
from pathlib import Path
from collections import defaultdict
from ps1_function_tables import adapt_function_tables
from ps1_pointer_cells import adapt_pointer_cells, adapt_chains, adapt_cell_stores, view_types
from audit_switch_tables import relabel_switches
from ps1_source_edits import apply_edits, plan_keys
from ps1_resident_defs import (externalize_pinned_bases, pin_definition,
                               pin_view_definitions, view_address)

BASE_DIR = Path(__file__).resolve().parent.parent.parent
# The decompiled overlay C, the symbol ledger and tools/overlay are
# toki1-decomp's (pc/tools/pc_paths.py; the same checkout in the monorepo).
# The output and the PC overrides are this checkout's.
from pc_paths import DECOMP  # noqa: E402
sys.path.insert(0, str(DECOMP / "tools" / "overlay"))
import overlay_names  # noqa: E402  (a landed overlay name -> its address)
OVERLAY_SRC = DECOMP / "src" / "overlay"
PC_OVERLAY_OVERRIDE = BASE_DIR / "pc" / "src" / "overlay"  # Override directory
PC_SRC = BASE_DIR / "pc" / "src"
PC_INCLUDE = BASE_DIR / "pc" / "include"

# ---------------------------------------------------------------------------
# Main-binary symbol names.  The dedup / strip rules below were written for
# address names (D_XXXXXXXX, func_XXXXXXXX).  The renames (2026-09-27..)
# landed real names for ~1,300 data and ~1,300 function symbols
# (config/symbol_names.json), and a landed name must get exactly the treatment
# its address name had: a global's extern is deduplicated across the module, a
# main function's forward declaration is dropped (each overlay file declares
# it with its own idea of the prototype, and one TU cannot hold two).  The
# libc-named thunks (strlen, memcpy, ...) are in the function set too: the
# alias header maps them to one func_800Bxxxx symbol per game TU.
import json as _json
sys.path.insert(0, str(BASE_DIR / "pc" / "tools"))
import gen_sony_bios_alias as _alias  # noqa: E402


def _landed_main_names():
    doc = _json.load(open(DECOMP / "config" / "symbol_names.json"))["symbols"]
    data, funcs = set(), set()
    for key, e in doc.items():
        if ":" in key or not e.get("landed"):
            continue
        n = e["name"]
        if re.match(r"^(?:func|D)_[0-9A-Fa-f]{8}$", n):
            continue
        (funcs if e.get("kind") == "func" else data).add(n)
    funcs |= {n for n, _ in _alias.aliases()}
    return data, funcs


_LANDED_DATA, _LANDED_FUNCS = _landed_main_names()


def _landed_overlay_data(ovl_name):
    """{address: {landed data names}} for one overlay's own symbols (keys
    "<OV>:0xADDR"), for externalize_pinned_bases."""
    doc = _json.load(open(DECOMP / "config" / "symbol_names.json"))["symbols"]
    out = {}
    for key, e in doc.items():
        ov, sep, addr = key.partition(":")
        if not sep or ov != ovl_name or not e.get("landed") or e.get("kind") == "func":
            continue
        try:
            out.setdefault(int(addr, 16), set()).add(e["name"])
        except ValueError:
            continue
    return out


_TXT_RE = re.compile(r'\bTXT_([0-9A-F]{8})\b')
_TXT_INCLUDE_RE = re.compile(r'(?m)^#include "overlay_text/[A-Z0-9_]+\.h"\n')


def literals_to_ps1_data(ovl_name, content):
    """Matched overlay C writes a message string literal as `TXT_<ADDR>`, which
    the PS1 build expands from the gitignored include/overlay_text/<OV>.h
    (game text read from the disc; tools/overlay/gen_overlay_text.py).  The
    port keeps PS1 data in the PS1 image -- a 32-bit PS1 address, translated
    by the English pack in place -- so each TXT_<ADDR> becomes the PS1 object
    at ADDR: its landed name, else D_<ADDR>, exactly the extern the matched C
    used before the literal."""
    if 'TXT_' not in content:
        return content
    landed = _landed_overlay_data(ovl_name)
    names = []

    def sub(m):
        addr = int(m.group(1), 16)
        name = (sorted(landed.get(addr, ())) or [f"D_{addr:08X}"])[0]
        names.append(name)
        return name

    content = _TXT_INCLUDE_RE.sub('', content)
    content = _TXT_RE.sub(sub, content)
    decl = "".join(f"extern u8 {n}[];\n" for n in dict.fromkeys(names))
    incs = list(re.finditer(r'(?m)^#include[^\n]*\n', content))
    pos = incs[-1].end() if incs else 0
    return content[:pos] + decl + content[pos:]


def _alt(names):
    return "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))


# a main-binary global: address name or landed name
DATA_NAME = r"(?:D_[0-9A-Fa-f]{8}[a-z]?" + ("|" + _alt(_LANDED_DATA) if _LANDED_DATA else "") + r")"
# a main-binary function whose forward declarations are dropped
FUNC_NAME = r"(?:func_[0-9A-Fa-f]{8}|FUN_[0-9A-Fa-f]{8}" + ("|" + _alt(_LANDED_FUNCS) if _LANDED_FUNCS else "") + r")"


def extract_func_addr(name, ovl_name=None):
    """Address of an overlay function: func_XXXXXXXX, or (once
    apply_overlay_symbols.py has landed a real name) the overlay's own
    name table entry."""
    if ovl_name:
        return overlay_names.func_addr(ovl_name, name)
    m = re.match(r'func_([0-9A-Fa-f]{8})', name)
    if m:
        return int(m.group(1), 16)
    return None


def type_specificity(decl):
    """Score how specific a type declaration is. Higher = more specific."""
    score = 0
    # 2D array is most specific (e.g., u8 D_XXX[10][56])
    if re.search(r'\[\d+\]\s*\[\d+\]', decl):
        score += 100
    # Sized 1D array (e.g., u8 D_XXX[100])
    elif re.search(r'\[\d+\]', decl):
        score += 50
    # Unsized array (e.g., u8 D_XXX[])
    elif '[]' in decl:
        score += 25
    # Pointer type
    if '*' in decl:
        score += 10
    return score








# ---------------------------------------------------------------------------
# Global variables: one canonical declaration per module, typed VIEWS for the
# files that declare the same global in another shape.
#
# Every overlay file carries its own `extern` for each main-binary global it
# touches, and the shapes disagree: `u8 screen_w[]` in one file, `u8
# screen_w[][56]` in the next, `s32 girl2_flag` here and `union { s32 w; u8 b; }
# girl2_flag` there, `volatile s16` against `short`.  One translation unit can
# hold only one declaration per name.  The old generator normalised every
# primitive extern to a single lossy form (`extern s32 X;`) and struck the rest,
# which broke any file that used the other shape (`.w` on an s32, `[i][456]` on
# a 1-D array).  Now the most common hoistable shape is declared once at the
# module head, and a file that declared shape k instead gets, in place of its
# extern,
#     typedef <T> name__tk <dims>;
#     #define  name__vk (*(name__tk *)PS1_VIEW(name))
# with its references to `name` rewritten to `name__vk`: same object, that
# file's own type, no second declaration.  Shapes that reference a file-local
# typedef can only ever be views.  PS1_VIEW(name) is `&name` laundered
# through an empty asm (pc/include/ps1_view.h): a view's shape often runs
# past the canonical object into the next PS1 symbol's bytes (records split
# across two symbols), and GCC would otherwise assume the access cannot
# touch that other symbol and reorder it around a store to it.
EXTERN_DECL_RE = re.compile(r'\bextern\s+((?:[^;{}]|\{[^{}]*\})+?);', re.S)


def mask_comments(text):
    """text with every /* comment */ replaced by spaces of the same length, so
    that spans found in the mask index the original."""
    return re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)


def _top_level_split(body, sep=','):
    parts, depth, cur = [], 0, ''
    for ch in body:
        if ch in '{[(':
            depth += 1
        elif ch in '}])':
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    parts.append(cur)
    return parts


def parse_var_extern(decl):
    """`extern <declaration>;` -> [(name, shape)] for a VARIABLE declaration
    (one entry per declarator: `extern u32 a, b[2];` gives two), where shape is
    the declaration text with the declared identifier replaced by `@`
    (`u8 @[][56]`, `union { s32 w; u8 b; } @`, `void (*@[])(void)`).  None for
    a function declaration or anything this parser does not understand (left
    in the file untouched)."""
    body = decl.strip()
    if not body.startswith('extern'):
        return None
    body = ' '.join(body[len('extern'):].strip().rstrip(';').split())
    # function pointer (array) variable: T (*name[dims])(args)
    m = re.match(r'^(.*?\(\s*\*\s*)([A-Za-z_]\w*)(\s*(?:\[[^\]]*\])*\s*\)\s*\(.*\))$', body, re.S)
    if m:
        return [(m.group(2), m.group(1) + '@' + m.group(3))]
    depth = 0
    for ch in body:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        elif ch == '(' and depth == 0:
            return None
    parts = _top_level_split(body)
    m = re.match(r'^(.*?)(\s*\b)([A-Za-z_]\w*)(\s*(?:\[[^\]]*\]\s*)*)$', parts[0].strip(), re.S)
    if not m or not m.group(1).strip():
        return None
    out = [(m.group(3), m.group(1) + m.group(2) + '@' + ''.join(m.group(4).split()))]
    base = m.group(1).rstrip(' *').strip()
    for part in parts[1:]:
        m2 = re.match(r'^\s*(\**)\s*([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)$', part, re.S)
        if not m2:
            return None
        out.append((m2.group(2), base + ' ' + m2.group(1) + '@' + ''.join(m2.group(3).split())))
    return out


def var_externs(content):
    """[(span, [(name, shape), ...])] for every variable extern in content
    (comments masked, so prose that says `extern` is not a declaration)."""
    out = []
    for m in EXTERN_DECL_RE.finditer(mask_comments(content)):
        parsed = parse_var_extern(m.group(0))
        if parsed:
            out.append((m.span(), parsed))
    return out


def shape_rank(shape):
    """tie-break between equally common shapes: sized arrays, then wider
    dimensionality, then the shorter spelling."""
    dims = shape[shape.index('@'):]
    return (len(re.findall(r'\[\d+\]', dims)), dims.count('['), -len(shape))


# types the module header provides; a file-local typedef of one of these
# names still gets renamed (the file defines its own), these are never
# collected as *local* when merely used
_GLOBAL_TYPES = {'u8', 'u16', 'u32', 's8', 's16', 's32', 'void', 'int', 'char', 'short', 'long',
                 'unsigned', 'signed', 'float', 'double'}


def get_local_type_names(content):
    """Names of the types this file defines: typedef names (any form) and
    struct/union tags, which the module renames per function so that two
    files' private `Spr` or `Tab14` do not collide in one translation unit."""
    code = mask_comments(content)
    types = set()
    # typedef struct [Tag] { ... } Name;  (one brace level)
    for m in re.finditer(r'typedef\s+(?:struct|union)\s*(\w+)?\s*\{(?:[^{}]|\{[^{}]*\})*\}\s*(\*?\s*\w+)\s*((?:\[[^\]]*\]\s*)*);', code):
        if m.group(1):
            types.add(m.group(1))
        types.add(m.group(2).lstrip('* '))
    # typedef T (*Name)(args);
    for m in re.finditer(r'typedef\s+[^;(]+\(\s*\*\s*(\w+)\s*\)\s*\([^)]*\)\s*;', code):
        types.add(m.group(1))
    # typedef T Name;  /  typedef T Name[N];   (no braces, no parens)
    for m in re.finditer(r'typedef\s+([^;{}()]+?)\s*\b(\w+)\s*((?:\[[^\]]*\]\s*)*);', code):
        types.add(m.group(2))
    # struct Tag { ... };  (a tag defined without typedef)
    for m in re.finditer(r'\b(?:struct|union)\s+(\w+)\s*\{', code):
        types.add(m.group(1))
    return {t for t in types if t not in _GLOBAL_TYPES}


def rename_local_types(content, func_suffix):
    """Rename locally-defined typedefs to be unique per function."""
    local_types = get_local_type_names(content)

    for type_name in local_types:
        if type_name in ('u8', 'u16', 'u32', 's8', 's16', 's32', 'void', 'RECT', 'int', 'char'):
            continue
        new_type_name = f"{type_name}{func_suffix}"
        content = re.sub(rf'\b{type_name}\b', new_type_name, content)

    return content, local_types


def extract_defines(content, local_type_names=None):
    """Extract #define D_XXXXXXXX macros from content.

    If local_type_names is provided, skip macros that reference those types
    (they must stay inline with their typedef).
    """
    defines = {}
    for m in re.finditer(r'#define\s+(D_[0-9A-Fa-f]{8}[a-z]?)\s+(.+)', content):
        name = m.group(1)
        value = m.group(2).strip()

        # Skip macros that reference local types
        if local_type_names:
            uses_local_type = False
            for type_name in local_type_names:
                if re.search(rf'\b{type_name}\b', value):
                    uses_local_type = True
                    break
            if uses_local_type:
                continue

        defines[name] = f"#define {name} {value}"
    return defines








POINTER_CELL_SHAPE = re.compile(r'^[\w ]+\*+\s*(volatile\s+)?@$')


def canonical_cell_shape(shape, has_definition=False):
    pointer = POINTER_CELL_SHAPE.match(shape.strip())
    if not has_definition and pointer:
        return ('volatile ' if pointer[1] else '') + 'u32 @'
    return shape


def global_declaration(name, shape):
    return 'extern ' + shape.replace('@', name) + ';'


def apply_global_views(content, canonical, all_defines, view_ids):
    """Rewrite one file's variable externs against the module's canonical
    declarations (see the comment above EXTERN_DECL_RE)."""
    decls = var_externs(content)
    if not decls:
        return content
    edits = []       # (span, replacement), applied back to front so spans stay valid
    renames = {}     # name -> view name
    folded = {}      # view name -> (store setter, cell-address macro)
    for span, items in decls:
        pieces = []
        for name, shape in items:
            if name not in canonical and name not in all_defines:   # not a global this pass knows: keep its declaration
                pieces.append("extern " + shape.replace('@', name) + ";")
                continue
            if canonical.get(name) == shape:   # declared at the module head
                continue
            # (a module macro stands for the object; `&name` of the macro's
            # lvalue is its address, so the view below works for it as well)
            ids = view_ids[name]
            # an anonymous struct/union is a distinct type at every spelling, so
            # such a shape gets a fresh typedef per file; a named shape is shared
            key = shape if '{' not in shape else (shape, len(ids))
            k = ids.setdefault(key, len(ids) + 1)
            tname, vname = f"{name}__t{k}", f"{name}__v{k}"
            # PS1 pointer cells are 4 bytes: a `T *` view must LOAD 4 bytes
            # and zero-extend, not read a native 8-byte pointer (which folds
            # the neighbouring cell into the high half -- the mv_state.h
            # class; measured as name_ent_fade_in's palette NULL-walk).
            # Value shapes (scalars, arrays, structs without pointers) read
            # through the plain cast as before.
            if POINTER_CELL_SHAPE.match(shape.strip()):
                # load: fold; store: the statement-level `view = x;` form is
                # rewritten below into the truncating setter (a macro cannot
                # be both a folding rvalue and an lvalue cell)
                sname = f"{name}__s{k}"
                aname = f"{name}__a{k}"
                word = 'volatile u32' if POINTER_CELL_SHAPE.match(shape.strip())[1] else 'u32'
                pieces.append("typedef " + shape.replace('@', tname)
                              + f";\n#define {vname} (({tname})(unsigned long)*({word} *)PS1_VIEW({name}))"
                              + f"\n#define {sname}(v) (*({word} *)PS1_VIEW({name}) = (u32)(unsigned long)(v))"
                              + f"\n#define {aname} (({tname} *)PS1_VIEW({name}))")
                folded[vname] = (sname, aname)
            else:
                pieces.append("typedef " + shape.replace('@', tname) + f";\n#define {vname} (*({tname} *)PS1_VIEW({name}))")
            renames[name] = vname
        rep = ("\n" + "\n".join(pieces) + "\n") if pieces else ""
        edits.append((span, rep))
    for (a, b), rep in sorted(edits, reverse=True):
        content = content[:a] + rep + content[b:]
    if renames:
        # every reference except the view's own typedef/#define lines
        out = []
        for line in content.split('\n'):
            if re.match(r'(typedef\b.*\b\w+__t\d+\b|#define \w+__[vsa]\d+[( ])', line):
                out.append(line)
                continue
            for name, vname in renames.items():
                line = re.sub(rf'\b{re.escape(name)}\b', vname, line)
            m = re.match(r'^(\s*)(\w+__v(\d+)) = (.+);$', line)
            if m and m.group(2) in folded:
                line = f'{m.group(1)}{folded[m.group(2)][0]}({m.group(4)});'
            # &view alone = the cell's address (needs __a); &view[n].f binds
            # to the member through the pointer and stays valid with the fold
            for vname, (sname, aname) in folded.items():
                line = re.sub(rf'&{re.escape(vname)}\b(?![\[.>-])', aname, line)
            out.append(line)
        content = '\n'.join(out)
    return content


def strip_defines(content, keep=()):
    """Remove #define D_XXXXXXXX macros (exact match only, not D_XXXXXXXX_SUFFIX),
    except the names in `keep`."""
    lines = []
    for line in content.split('\n'):
        # Only strip if the define name is EXACTLY D_XXXXXXXX (with optional single lowercase letter)
        # Not D_XXXXXXXX_SUFFIX or similar
        m = re.match(r'\s*#define\s+(D_[0-9A-Fa-f]{8}[a-z]?)\s', line)
        if not m or m.group(1) in keep:
            lines.append(line)
    return '\n'.join(lines)





# ---------------------------------------------------------------------------
# Functions: one head declaration per external function, a forward declaration
# per module function, and per-file CAST VIEWS where a file's own prototype
# differs.  Overlay files declare the same callee with different prototypes
# (`s32 f(s16)` here, `void f(s32)` there, `Spr *prim_alloc()` against
# `u8 *prim_alloc()`), and two incompatible prototypes cannot share a TU.  So
# every prototype line is removed; the module head declares each external
# function once, prototype-less (`extern void *name();` when any file expects
# a pointer back, else `extern s32 name();` / `extern void name();`), and each
# module function once from its definition; and a file whose prototype had
# parameters or a pointer return gets
#     #define name__f<ADDR> ((RET (*)(PARAMS))name)
# with its references rewritten -- the call is made through that file's own
# idea of the callee's type, exactly as when the file is compiled alone.
PROTO_RE = re.compile(
    r'^[ \t]*(?:extern[ \t]+)?((?!static\b|typedef\b|return\b|else\b|case\b|goto\b|sizeof\b)'
    r'[A-Za-z_][\w \t\*]*?)[ \t]*\b([A-Za-z_]\w*)[ \t]*\(([^;{}()]*)\)[ \t]*;[ \t]*$', re.M)
INT_CLASS = {'void', 's32', 'u32', 's16', 'u16', 's8', 'u8', 'int', 'char', 'short', 'long',
             'unsigned', 'signed', 'unsigned int', 'unsigned short', 'unsigned char', 'long int'}


def prototypes(content):
    """[(span, ret, name, params)] for every prototype line (comments masked)."""
    out = []
    for m in PROTO_RE.finditer(mask_comments(content)):
        ret = ' '.join(m.group(1).split())
        if '(' in ret or not ret:
            continue
        out.append((m.span(), ret, m.group(2), ' '.join(m.group(3).split())))
    return out


def head_return_type(rets):
    """`void *` when any file expects a pointer back, else s32 -- never void:
    a file that uses the result of a callee another file declared void must
    still compile (the value is whatever the register holds, as on PS1)."""
    if any('*' in r for r in rets):
        return 'void *'
    return 's32'


def apply_function_views(content, own_name, suffix):
    """Remove every prototype line of one file; give the file a cast view of
    each callee it declared with parameters or a pointer return."""
    protos = prototypes(content)
    if not protos:
        return content
    edits, views = [], {}
    for span, ret, name, params in protos:
        if name == own_name:
            edits.append((span, ''))
            continue
        simple = params in ('', 'void') and '*' not in ret and ret in INT_CLASS
        if simple:
            edits.append((span, ''))
            continue
        alias = f'{name}__f{suffix}'
        views[name] = alias
        edits.append((span, f'\n#define {alias} ((' + ret + ' (*)(' + params + '))' + name + ')\n'))
    for (a, b), rep in sorted(edits, reverse=True):
        content = content[:a] + rep + content[b:]
    if views:
        out = []
        for line in content.split('\n'):
            if re.match(r'#define \w+__f[0-9A-F]{8} ', line):
                out.append(line)
                continue
            for name, alias in views.items():
                # not `&name` (address of the real symbol) and not an already-viewed token
                line = re.sub(rf'(?<![\w&]){re.escape(name)}\b(?!__[fv])', alias, line)
            out.append(line)
        content = '\n'.join(out)
    return content


DEFN_RE = re.compile(r'^(?!extern\b|typedef\b|return\b|#)(static[ \t]+)?([A-Za-z_](?:[\w \*]|\{[^{}]*\})*?)[ \t]*\b([A-Za-z_]\w*)[ \t]*((?:\[[^\]]*\][ \t]*)*)[ \t]*(?:=|;)', re.M)


def file_scope_definitions(content, known):
    """[(span, name, shape, is_static)] for the file-scope variable
    DEFINITIONS of names some file externs (or address/landed globals):
    `TaiikuHdr D_8014A5D0;`, `u8 D_8014A5C8[4] = {...};`.  The span covers
    the whole statement up to and including its `;`."""
    out = []
    code = mask_comments(content)
    for m in DEFN_RE.finditer(code):
        t, name, dims = ' '.join(m.group(2).split()), m.group(3), ''.join(m.group(4).split())
        if '(' in t or t.split()[0] in ('break', 'continue', 'goto', 'return', 'else', 'case'):
            continue
        if name not in known and not re.match(DATA_NAME + '$', name):
            continue
        i, depth = m.start(), 0
        while i < len(code):
            if code[i] in '{(':
                depth += 1
            elif code[i] in '})':
                depth -= 1
            elif code[i] == ';' and depth == 0:
                break
            i += 1
        out.append(((m.start(), i + 1), name, t + ' @' + dims, bool(m.group(1))))
    return out


def definition_signature(content, new_name):
    """(ret, params, is_ansi) of the definition of new_name in content, or None."""
    m = re.search(r'^([A-Za-z_][\w \t\*]*?)[ \t]*\b' + re.escape(new_name) + r'[ \t]*\(([^;{}()]*)\)', mask_comments(content), re.M)
    if not m:
        return None
    ret = ' '.join(m.group(1).split())
    params = ' '.join(m.group(2).split())
    if ret.startswith('extern') or '(' in ret:
        return None
    if params in ('', 'void'):
        return ret, params, True
    parts = [q.strip() for q in params.split(',')]
    ansi = all((' ' in q or '*' in q) for q in parts)
    return ret, params, ansi


def strip_func_forward_decls(content, local_addrs, local_names):
    """Remove forward declarations for overlay-local functions.

    Call this BEFORE renaming functions. Strips:
    - extern void func_XXXXXXXX(...);
    - void func_XXXXXXXX(...);
    - s32 *func_XXXXXXXX(...);
    etc.

    The leading word must not be a statement keyword: `return f();`
    and `else f();` have the shape `<word> name();` too, and stripping
    them deleted the statement (DATE's date_event_sched lost its closing
    `return sub_sub_step_up();`, so the date's sub-sub-step 15 never
    advanced).
    """
    # Strip forward declarations for local functions
    for addr in local_addrs:
        name = local_names[addr]
        # Match: optional 'extern', return type (with optional pointer), function name, params, semicolon
        # Use [ \t]* for trailing whitespace (not \s* which includes newlines)
        pattern = rf'^[ \t]*(?!(?:static|return|else|do|goto|case|default)\b)(?:extern\s+)?[A-Za-z_][\w \t\*]*?[\s\*]+{re.escape(name)}\s*\([^)]*\)\s*;[ \t]*\n?'
        content = re.sub(pattern, '', content, flags=re.MULTILINE)

    # Also strip the forward declarations of main-binary functions (address
    # names func_/FUN_ and landed names alike): each overlay file declares them
    # with its own idea of the prototype, and one TU cannot hold two.
    pattern = r'^[ \t]*(?!(?:static|return|else|do|goto|case|default)\b)(?:extern\s+)?[A-Za-z_][\w \t\*]*?[\s\*]+' + FUNC_NAME + r'\s*\([^)]*\)\s*;[ \t]*\n?'
    content = re.sub(pattern, '', content, flags=re.MULTILINE)

    return content


def strip_includes(content):
    """Remove #include directives."""
    return re.sub(r'#include\s+[<"][^>"]+[>"]\s*\n?', '', content)


def strip_header_comment(content):
    """Remove the initial /* ... */ block comment."""
    content = content.strip()
    if content.startswith('/*'):
        end = content.find('*/')
        if end >= 0:
            content = content[end + 2:].strip()
    return content


def process_overlay(ovl_name, ovl_dir):
    """Generate a unified C module for one overlay."""
    func_files = sorted(ovl_dir.glob("*.c"))
    if not func_files:
        return None

    override_dir = PC_OVERLAY_OVERRIDE / ovl_name
    override_count = 0

    local_addrs = set()
    local_funcs = {}
    local_names = {}     # address -> the function's name (func_<ADDR> or a landed name)
    for f in func_files:
        addr = extract_func_addr(f.stem, ovl_name)
        if addr:
            local_names[addr] = f.stem
            local_addrs.add(addr)
            override_file = override_dir / f.name
            if override_file.exists():
                local_funcs[addr] = override_file
                override_count += 1
            else:
                local_funcs[addr] = f

    if override_count > 0:
        print(f"  {ovl_name}: Using {override_count} PC overrides from {override_dir}")

    # internal functions: a landed record with `internal_of` is a function the
    # C defines inside another window's file (no .sym entry, no file of its
    # own).  It is a module function all the same: prefixed and called
    # directly (2026-11-10: taco_actor_step6 came out unprefixed, behind a
    # typed-call cast)
    internal_names = []
    for key, e in _json.load(open(DECOMP / "config" / "symbol_names.json"))["symbols"].items():
        ov, sep, _a = key.partition(":")
        if sep and ov == ovl_name and e.get("landed") and e.get("internal_of") and e.get("name"):
            internal_names.append(e["name"])

    # every ps1_source_edits.py plan entry of this overlay must name a matched
    # file the generator reads (a renamed or overridden function makes the
    # entry stale)
    # (only for the real tree: tests feed synthetic overlay directories)
    real = Path(ovl_dir).resolve() == (OVERLAY_SRC / ovl_name).resolve()
    for key in (plan_keys(ovl_name) if real else ()):
        stem = key.split('/', 1)[1]
        if not any(f.stem == stem and f.parent == ovl_dir for f in local_funcs.values()):
            raise ValueError(f"{key}: stale pc/tools/call_args_plan.json / record_width_plan.json entry "
                             f"(no matched src/overlay/{key}.c read for this module)")

    prefix = ovl_name

    # First pass: every shape each global is declared with, the file-scope
    # definitions, the prototypes of every function, and the #defines.
    # A shape that names a file-local typedef cannot be hoisted to the module
    # head; it can only be a view.
    shapes = defaultdict(lambda: defaultdict(int))  # name -> shape -> count
    local_only = set()                              # names declared ONLY via local types
    all_defines = {}                                # name -> #define line
    proto_rets = defaultdict(list)                  # raw function name -> [ret]
    raw_texts, raw_local_types = {}, {}
    for addr in sorted(local_funcs.keys()):
        content = local_funcs[addr].read_text()
        if local_funcs[addr].parent == ovl_dir:
            # matched C: case labels from the retail jump table
            # (pc/tools/switch_labels.json; audit_switch_tables.py verifies it)
            content = relabel_switches(f"{ovl_name}/{local_funcs[addr].stem}", content)
            # the arguments the console passes in registers the matched C
            # leaves unnamed, and PS1-width record members
            # (pc/tools/ps1_source_edits.py: call_args_plan.json /
            # record_width_plan.json)
            content = apply_edits(f"{ovl_name}/{local_funcs[addr].stem}", content)
            # string literals -> the PS1 literal-pool objects
            content = literals_to_ps1_data(ovl_name, content)
        content = adapt_pointer_cells(adapt_function_tables(content))
        raw_texts[addr] = content
        local_types = get_local_type_names(content)
        raw_local_types[addr] = local_types
        for _span, items in var_externs(content):
            for name, shape in items:
                if any(re.search(rf'\b{re.escape(t)}\b', shape) for t in local_types):
                    local_only.add(name)
                    continue
                shapes[name][shape] += 1
        for _span, ret, name, params in prototypes(content):
            proto_rets[name].append(ret)
        # Collect #defines - skip macros that use local types (they must stay inline)
        for name, defn in extract_defines(content, local_types).items():
            if name not in all_defines:
                all_defines[name] = defn

    # The canonical (module-head) declaration: the most common hoistable shape.
    canonical = {}   # name -> shape
    for name, counter in shapes.items():
        best = max(counter.items(), key=lambda kv: (kv[1], shape_rank(kv[0])))
        canonical[name] = best[0]
    for name in local_only - set(canonical):
        canonical[name] = 'u8 @[]'         # every file views it; the head just names the object

    # A global DEFINED by a module file: its definition's shape is the
    # canonical one when hoistable.  When the definition's type is file-local
    # the definition is renamed <name>__def and the real name is an alias of
    # it (declared at the head in the canonical shape), so the other files'
    # views and the definition coexist.
    alias_defs = defaultdict(set)          # addr -> {name}
    dup_defs = defaultdict(set)            # addr -> {name}: a LATER definition of a name (first file wins)
    first_def = {}
    # A landed overlay data name is a global exactly as its D_ name was, even
    # when no file externs it: two files that both DEFINE it (TAIIKU
    # D_80149B70, three definitions) must dedup like the address name did,
    # or the module holds conflicting definitions.
    # (Keep landed names out of this comment: apply_overlay_data.py refuses
    # a name that appears anywhere in the tree's code files.)
    known = set(canonical) | set(shapes)
    for names in _landed_overlay_data(ovl_name).values():
        known |= names
    for addr in sorted(local_funcs.keys()):
        for _span, name, shape, _static in file_scope_definitions(raw_texts[addr], known):
            if name in first_def:
                dup_defs[addr].add(name)
                continue
            first_def[name] = addr
            # a file-local type, or an anonymous aggregate (a distinct type at
            # every spelling): the definition cannot be the head declaration
            if '{' in shape or any(re.search(rf'\b{re.escape(t)}\b', shape) for t in raw_local_types[addr]):
                canonical.setdefault(name, 'u8 @[]')
                alias_defs[addr].add(name)
            else:
                canonical[name] = shape
            all_defines.pop(name, None)

    # Resolve conflicts between #defines and externs
    # If an extern has array type, prefer it over the macro (supports array
    # access); otherwise prefer the macro (simpler access) and drop the externs.
    for name in list(all_defines.keys()):
        if name in canonical:
            if '[' in canonical[name][canonical[name].index('@'):]:
                del all_defines[name]
            else:
                del canonical[name]

    # A PS1 pointer cell occupies one word even when every user happened to
    # agree on a pointer declaration.  Route those users through the same
    # widening/truncating views used for conflicting declarations above.
    for name, shape in list(canonical.items()):
        canonical[name] = canonical_cell_shape(shape, name in first_def)

    final_externs = {name: global_declaration(name, shape) for name, shape in canonical.items()}
    view_ids = defaultdict(dict)   # name -> shape -> k

    # Head declarations of the EXTERNAL functions the files prototype
    local_stems = set(local_names.values())
    func_externs = {}
    for name, rets in proto_rets.items():
        if name in local_stems or name in all_defines:
            continue
        func_externs[name] = f"extern {head_return_type(rets)} {name}();".replace('* ', '*')

    # Process function bodies
    func_bodies, local_decls = {}, {}
    all_local_types = set()

    for addr in sorted(local_funcs.keys()):
        content = raw_texts[addr]

        content = strip_header_comment(content)
        content = strip_includes(content)

        # Strip function forward declarations BEFORE renaming
        content = strip_func_forward_decls(content, local_addrs, local_names)

        func_suffix = f"_{addr:08X}"
        content, _ = rename_local_types(content, func_suffix)
        all_local_types |= {f"{t}{func_suffix}" for t in raw_local_types[addr]}

        # Strip #defines (they're deduplicated globally) -- except one whose
        # value names a global this file declares in a non-canonical shape:
        # that copy must be spelt through this file's view, so it stays here
        # (redefinition of a macro with another body is accepted by GCC).
        file_shapes = {n: sh for _sp, items in var_externs(content) for n, sh in items}
        keep_defines = set()
        for m in re.finditer(r'^[ \t]*#define[ \t]+(D_[0-9A-Fa-f]{8}[a-z]?)[ \t]+(.+)$', mask_comments(content), re.M):
            refs = set(re.findall(r'\b[A-Za-z_]\w*\b', m.group(2)))
            if any(n in file_shapes and file_shapes[n] != canonical.get(n) for n in refs):
                keep_defines.add(m.group(1))
        content = strip_defines(content, keep_defines)

        # Definitions: a name another file already defined becomes an extern
        # here (first definition wins; the shape stays, so the file keeps its
        # own view), and a `static` definition of a name that other files
        # extern loses the `static` (one object per module, as on the PS1 RAM).
        edits = []
        for span, name, shape, is_static in file_scope_definitions(content, known | set(canonical)):
            if name in dup_defs.get(addr, ()):
                edits.append((span, 'extern ' + shape.replace('@', name) + ';'))
            elif is_static:
                a0, b0 = span
                edits.append((span, re.sub(r'^static[ \t]+', '', content[a0:b0])))
        for (a0, b0), rep in sorted(edits, reverse=True):
            content = content[:a0] + rep + content[b0:]

        # The file's remaining `static` objects and helper functions are
        # private to it: two files' `static u8 pad_align[12]` must not meet.
        statics = set()
        for m in re.finditer(r'^[ \t]*static\b[^;{}()=]*?\b([A-Za-z_]\w*)[ \t]*(?:\(|\[|=|;)', mask_comments(content), re.M):
            if m.group(1) not in _GLOBAL_TYPES:
                statics.add(m.group(1))
        for name in sorted(statics):
            content = re.sub(rf'\b{re.escape(name)}\b', f'{name}_{addr:08X}', content)

        # Globals: drop the extern when it is the canonical shape; replace a
        # differing shape by a typed view (see EXTERN_DECL_RE) and point this
        # file's references at the view -- a module macro's object too.
        content = apply_global_views(content, canonical, all_defines, view_ids)
        # subscripts through a view or cast of a pointer-to-pointer read
        # 4-byte PS1 cells (ps1_pointer_cells.adapt_chains)
        content = adapt_cell_stores(content)
        content = adapt_chains(content, view_types(content))
        for name in sorted(alias_defs.get(addr, ())):
            content = re.sub(rf'\b{re.escape(name)}\b', name + '__def', content)
            # an address-named object: the __def is the PS1 object itself
            # (the real name resolves there through psx_globals.syms), so it
            # is pinned at that address rather than becoming a host copy
            ps1 = view_address(name + '__def')
            if ps1 is None:
                # a landed overlay data name gets exactly the treatment its
                # address name had: the __def is pinned at the PS1 object
                # (2026-11-02: date_angle_tbl, ex-D_8015DDB0, came out as an
                # alias of an incomplete array, which clang rejects)
                ps1 = next((a for a, ns in _landed_overlay_data(ovl_name).items()
                            if name in ns), None)
            if ps1 is not None:
                content, pinned = pin_definition(content, name + '__def', ps1)
                if pinned:
                    continue
                print(f"  WARNING: {ovl_name}/{local_names[addr]}: {name} has a "
                      f"non-zero initializer; left as a host object")
            content += ('\n' + final_externs[name][:-1]
                        + f' __attribute__((alias("{name}__def")));\n')
        # address-named views a file defines (D_8015015C_s): the PS1 object
        content, _pinned, left = pin_view_definitions(content)
        for name in left:
            print(f"  WARNING: {ovl_name}/{local_names[addr]}: {name} has a "
                  f"non-zero initializer; left as a host object")
        content = content.strip()

        old_name = local_names[addr]
        new_name = f"{prefix}_{old_name}"

        content = re.sub(
            rf'^void\s+{re.escape(old_name)}\s*\(',
            rf's32 {new_name}(',
            content,
            count=1,
            flags=re.MULTILINE
        )
        content = re.sub(
            rf'^(\w[\w\s\*]*?)\s+{re.escape(old_name)}\s*\(',
            rf'\1 {new_name}(',
            content,
            count=1,
            flags=re.MULTILINE
        )

        for ref_old in [local_names[a] for a in local_addrs] + internal_names:
            ref_new = f"{prefix}_{ref_old}"
            content = re.sub(rf'\b{re.escape(ref_old)}\b', ref_new, content)

        content = re.sub(
            rf'({re.escape(new_name)})\(void\)',
            rf'\1()',
            content
        )

        # Functions: prototypes out, per-file cast views in (see PROTO_RE)
        content = apply_function_views(content, new_name, f"{addr:08X}")

        func_bodies[addr] = content
        sig = definition_signature(content, new_name)
        if not sig and re.search(r'^' + re.escape(new_name) + r'\s*\(', mask_comments(content), re.M):
            local_decls[addr] = f"s32 {new_name}();"      # implicit-int definition
        if sig:
            ret, params, ansi = sig
            uses_local = any(re.search(rf'\b{re.escape(t)}\b', ret + ' ' + params) for t in all_local_types)
            ret_local = any(re.search(rf'\b{re.escape(t)}\b', ret) for t in all_local_types)
            narrow = re.search(r'\b(s16|u16|s8|u8|char|short)\b', params) is not None
            # Prototype-less by preference: other files call this function with
            # whatever argument list they modelled (as they did when every call
            # was an implicit declaration).  A definition with narrow parameters
            # (s16/u8) is incompatible with a prototype-less declaration, so
            # that one gets the real prototype -- unless it names a file-local
            # type, which the head cannot see.
            if ret_local:
                pass
            elif not narrow:
                local_decls[addr] = f"{ret} {new_name}();"
            elif ansi and not uses_local:
                local_decls[addr] = f"{ret} {new_name}({params});"

    # Third pass: a file that calls another module function WITHOUT its own
    # prototype used to get an implicit `int f()` -- any argument list went.
    # The forward declarations above would now check those calls against the
    # callee's real signature, and the overlay files do disagree with each
    # other (a struct by value here, a pointer there; two arguments here,
    # three there).  Such a file calls through a prototype-less cast view,
    #     #define OV_f__c<ADDR> ((RET (*)())OV_f)
    # which is exactly the implicit-declaration contract it had.
    local_new = {a: f"{prefix}_{local_names[a]}" for a in func_bodies}
    head_ret = {}
    for a, decl in local_decls.items():
        head_ret[local_new[a]] = decl.split(local_new[a])[0].strip()
    for addr, body in func_bodies.items():
        own = local_new[addr]
        masked = mask_comments(body)
        present = set(re.findall(rf'(?<![\w&])({re.escape(prefix)}_\w+?)\b(?!__)', masked))
        views = []
        for name in sorted(present):
            if name == own or name not in head_ret or f'{name}__f' in body:
                continue
            alias = f'{name}__c{addr:08X}'
            body = re.sub(rf'(?<![\w&]){re.escape(name)}\b(?!__)', alias, body)
            views.append(f'#define {alias} (({head_ret[name]} (*)()){name})')
        if views:
            func_bodies[addr] = '\n'.join(views) + '\n' + body

    # helper functions a file defines besides its own (not module functions,
    # not external either): never declare them at the head
    for a, body in func_bodies.items():
        for m in re.finditer(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)[ \t]*\([^;{}()]*\)\s*(?:[^;{}()]*;\s*)*\{', mask_comments(body), re.M):
            proto_rets.pop(m.group(1), None)
            local_stems.add(m.group(1))
    func_externs = {n: d for n, d in func_externs.items() if n not in local_stems}

    # Build the output
    lines = []
    lines.append(f"/* overlay_{ovl_name}.c — Unified module for {ovl_name} overlay")
    lines.append(f" * Auto-generated by generate_overlay_modules.py")
    lines.append(f" * {len(local_funcs)} functions")
    lines.append(f" */")
    lines.append(f"#include \"types.h\"")
    lines.append(f"#include <string.h>")
    # every typed view of a global goes through PS1_VIEW: an address the
    # compiler cannot pin to one C object (pc/include/ps1_view.h)
    lines.append(f"#include \"ps1_view.h\"")
    # Retail includes are replaced above, but PC overrides can share layout
    # headers. Keep those once at module scope, outside per-function renaming.
    override_headers = {header for addr, source in local_funcs.items()
                        if source.parent == override_dir
                        for header in re.findall(r'^\s*#include\s+"([^"\n]+)"',
                                                 raw_texts[addr], re.M)
                        if header != 'types.h'}
    # the project's shared layout headers (include/tokimeki/: CLASS_W,
    # SPRITE_REC, ...) are typedef-only and safe at module scope; the matched
    # C that names their fields needs them
    override_headers |= {header for addr in local_funcs
                         for header in re.findall(r'^\s*#include\s+"(tokimeki/[^"\n]+)"',
                                                  raw_texts[addr], re.M)}
    lines.extend(f'#include "{header}"' for header in sorted(override_headers))
    lines.append(f"")

    # Emit #defines first (they may be used by code before externs are relevant)
    if all_defines:
        lines.append(f"/* Macro definitions (from source files) */")
        for name in sorted(all_defines.keys()):
            lines.append(all_defines[name])
        lines.append(f"")

    if final_externs:
        lines.append(f"/* Global variable declarations (canonical shape; other shapes are per-file views) */")
        for name in sorted(final_externs.keys()):
            lines.append(final_externs[name])
        lines.append(f"")

    # address-named functions used as values with no prototype anywhere
    body_text = '\n'.join(func_bodies[a] for a in sorted(func_bodies))
    declared = set(func_externs) | {f"{prefix}_{n}" for n in local_stems} | local_stems
    for tok in sorted(set(re.findall(r'(?<![\w])(func_[0-9A-Fa-f]{8})\b', mask_comments(body_text)))):
        if tok not in declared:
            func_externs[tok] = f"extern s32 {tok}();"

    if func_externs:
        lines.append(f"/* External function declarations (prototype-less; files call through their own cast views) */")
        for name in sorted(func_externs.keys()):
            lines.append(func_externs[name])
        lines.append(f"")

    if local_decls:
        lines.append(f"/* Forward declarations of this module's functions */")
        for addr in sorted(local_decls.keys()):
            lines.append(local_decls[addr])
        lines.append(f"")

    for addr in sorted(func_bodies.keys()):
        old_name = local_names[addr]
        lines.append(f"/* --- {old_name} --- */")
        lines.append(func_bodies[addr])
        lines.append(f"")

    first_addr = min(local_funcs.keys())
    first_name = f"{prefix}_{local_names[first_addr]}"

    lines.append(f"/* Entry point for {ovl_name} overlay */")
    sig = definition_signature(func_bodies[first_addr], first_name)
    if sig is not None and sig[1] not in ('', 'void'):
        # The first function reads parameters (K&R or ANSI) that no PS1
        # caller supplies through this PC-only reference entry (pc/hal/
        # overlay_dispatch.c's table, which nothing calls): calling it bare
        # would run it on whatever the argument registers hold
        # (audit_ps1_call_args.py SHORT-READ).  Show the HAL's
        # unimplemented-scene screen instead.
        lines.append(f"extern void overlay_stub_handler(void);")
        lines.append(f"void overlay_{ovl_name}_entry(void) {{")
        lines.append(f"    overlay_stub_handler();   /* {first_name}({sig[1]}): no caller-free entry */")
    else:
        lines.append(f"void overlay_{ovl_name}_entry(void) {{")
        lines.append(f"    ((void (*)(void)){first_name})();")
    lines.append(f"}}")
    lines.append(f"")

    text = "\n".join(line.rstrip() for line in "\n".join(lines).splitlines()) + "\n"
    # a D_<ADDR> some file defines while another pins a view to it: extern
    # (ps1_resident_defs.externalize_pinned_bases)
    text, _done, left = externalize_pinned_bases(text, _landed_overlay_data(ovl_name))
    for name in left:
        print(f"  WARNING: {ovl_name}: {name} has a non-zero initializer and pinned views; "
              f"left as a host object")
    return text, len(local_funcs)


def main():
    PC_SRC.mkdir(parents=True, exist_ok=True)
    PC_INCLUDE.mkdir(parents=True, exist_ok=True)

    overlay_entries = []
    total_funcs = 0
    only = {a.upper() for a in sys.argv[1:] if not a.startswith('-')}

    for ovl_dir in sorted(OVERLAY_SRC.iterdir()):
        if not ovl_dir.is_dir() or ovl_dir.name.startswith(('_', '.')):
            continue
        if only and ovl_dir.name not in only:
            continue

        ovl_name = ovl_dir.name
        result = process_overlay(ovl_name, ovl_dir)
        if result is None:
            print(f"  {ovl_name:15} — no functions, skipping")
            continue

        content, nfuncs = result
        out_path = PC_SRC / f"overlay_{ovl_name}.c"
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', dir=out_path.parent,
                                             prefix=out_path.name + '.',
                                             suffix='.tmp', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(content)
            os.replace(temporary, out_path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        overlay_entries.append(ovl_name)
        total_funcs += nfuncs
        print(f"  {ovl_name:15} — {nfuncs:4} functions -> {out_path.name}")

    if only:
        print(f"\nGenerated {len(overlay_entries)} overlay module(s) ({total_funcs} functions); "
              "header untouched (partial run)")
        return

    header_lines = [
        "/* overlay_entries.h — Auto-generated overlay entry declarations */",
        "#ifndef OVERLAY_ENTRIES_H",
        "#define OVERLAY_ENTRIES_H",
        "",
    ]
    for name in overlay_entries:
        header_lines.append(f"void overlay_{name}_entry(void);")
    header_lines.append("")
    header_lines.append("#endif")
    header_lines.append("")

    header_path = PC_INCLUDE / "overlay_entries.h"
    header_path.write_text("\n".join(header_lines))

    print(f"\nGenerated {len(overlay_entries)} overlay modules ({total_funcs} functions)")
    print(f"Header: {header_path}")


if __name__ == "__main__":
    main()
