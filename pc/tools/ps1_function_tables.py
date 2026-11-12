# SPDX-License-Identifier: GPL-3.0-or-later
"""Widen retail function-table copies at the PC generation boundary.

Only recognizes a struct containing one fixed array of function pointers,
copied by value from an extern data object. Other structures stay untouched.
The PS1 source and its 4-byte cells remain authoritative.
"""
import re


def adapt_function_tables(content):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*',
                    lambda m: ' ' * len(m.group()), content, flags=re.S)
    pointer_types = set(re.findall(
        r'^typedef\s+[^;{}]+?\(\s*\*\s*(\w+)\s*\)\s*\([^;{}]*\)\s*;',
        masked, re.M))
    edits = []
    # Direct calls through an extern retail table also have four-byte cells;
    # there need not be a preceding local aggregate copy.
    for table in re.finditer(
            r'^extern\s+(void|s32)\s*\(\s*\*\s*(\w+)\s*\[\s*\]\s*\)\s*\(([^()]*)\)\s*;',
            masked, re.M):
        ret, name, params = table.groups()
        for call in re.finditer(rf'\b{re.escape(name)}\s*\[([^\]\n]+)\](?=\s*\()', masked):
            edits.append((call.start(), call.end(),
                          f'(({ret} (*)({params}))overlay_resolve(((const u32 *){name})[{call[1]}]))'))
    # The same table declared through a pointer typedef, `extern HandlerFn
    # D_80155F10[];` (BUNKAKEN bunkaken_main1's per-slot handler: the call
    # took two 4-byte cells as one pointer and jumped to 0x000049a5_00004a50;
    # pc/tools/explorer.py, 2026-10-31).
    for table in re.finditer(r'^extern\s+(\w+)\s+(\w+)\s*\[\s*\d*\s*\]\s*;', masked, re.M):
        ptr, name = table.groups()
        if ptr not in pointer_types:
            continue
        for call in re.finditer(rf'\b{re.escape(name)}\s*\[([^\]\n]+)\](?=\s*\()', masked):
            edits.append((call.start(), call.end(),
                          f'(({ptr})overlay_resolve(((const u32 *){name})[{call[1]}]))'))
    # Some dispatchers store the copied table as integer words and cast its
    # address to Fn*. Those cells already have the right width; widen each
    # selected word rather than indexing the buffer with an eight-byte stride.
    raw_types = set()
    for struct in re.finditer(r'typedef\s+struct\s*\{([^{}]*)\}\s*(\w+)\s*;', masked):
        body, name = struct.groups()
        if '*' not in body and not any(re.search(rf'\b{t}\b', body) for t in pointer_types):
            raw_types.add(name)
    raw_locals = set()
    for name in raw_types:
        raw_locals.update(re.findall(rf'^\s*{name}\s+(\w+)\s*;', masked, re.M))
    for cast in re.finditer(
            r'\(\s*\(\s*(\w+)\s*\*\s*\)\s*&\s*(\w+)\s*\)\s*\[([^\]]+)\]', masked):
        pointer, local, index = cast.groups()
        if pointer in pointer_types and local in raw_locals:
            edits.append((cast.start(), cast.end(),
                          f'(({pointer})overlay_resolve(((const u32 *)&{local})[{index}]))'))
    for cast in re.finditer(
            r'\(\s*\(\s*(void|s32)\s*\(\s*\*\s*\*\s*\)\s*\(([^()]*)\)\s*\)\s*&\s*(\w+)\s*\)\s*\[([^\]]+)\]', masked):
        ret, params, local, index = cast.groups()
        if local in raw_locals:
            edits.append((cast.start(), cast.end(),
                          f'(({ret} (*)({params}))overlay_resolve(((const u32 *)&{local})[{index}]))'))
    # A byte-array table copy called through a pointer-to-pointer cast of a
    # byte offset (SHOUGATU shougatu_main8: `(*(s32 (**)(s32))(tbl.b + i * 4))(60)`
    # over `typedef struct { u8 b[192]; } StepTbl`): the cast reads eight
    # bytes, two PS1 cells, as one host pointer -- the 97 Christmas party
    # (scene 82 sub 8) called it and crash-looped (checkpoint
    # date_megumi_skating, 2026-11-01).  Read the one cell at that offset.
    for cast in re.finditer(
            r'\(\s*\*\s*\(\s*(void|s32)\s*\(\s*\*\s*\*\s*\)\s*\(([^()]*)\)\s*\)'
            r'\s*\(\s*(\w+)\.(\w+)\s*\+\s*([^()]+?)\s*\)\s*\)', masked):
        ret, params, local, member, offset = cast.groups()
        if local in raw_locals:
            edits.append((cast.start(), cast.end(),
                          f'(({ret} (*)({params}))overlay_resolve('
                          f'*(const u32 *)({local}.{member} + {offset})))'))
    # A word of an integer table copy cast straight to a function pointer
    # (ENDING ending_main0: `fn = (void (*)(s32))tbl.data[t1]`): the s32 cell
    # holds a PS1 address, which the cast would sign-extend into a host
    # pointer to nowhere.
    for cast in re.finditer(
            r'\(\s*(void|s32)\s*\(\s*\*\s*\)\s*\(([^()]*)\)\s*\)\s*(\w+)\.(\w+)\s*\[([^\]]+)\]',
            masked):
        ret, params, local, member, index = cast.groups()
        if local in raw_locals:
            edits.append((cast.start(), cast.end(),
                          f'(({ret} (*)({params}))overlay_resolve((u32){local}.{member}[{index}]))'))
    # A table is a struct whose only member is an array of function
    # pointers: `typedef struct { Fn ptrs[N]; } T;` (Fn a pointer typedef),
    # the member spelled inline, `void (*f[31])(s32);`, and either one as
    # `struct Tag { ... };` (the type is then `struct Tag`).  The inline and
    # tagged spellings were not recognised until 2026-10-31: the copy read
    # 8-byte host cells over the 4-byte PS1 cells and the dispatch called
    # two PS1 words as one pointer (SHOUGATU shougatu_main4 / main53e /
    # main6, SHUGAKU, DATE's FnTable dispatchers; pc/tools/explorer.py).
    tables = []
    for match in re.finditer(
            r'^(?:typedef\s+struct|struct\s+(\w+))\s*\{\s*'
            r'(?:(\w+)\s+(\w+)\s*\[\s*(\d+)\s*\]'
            r'|(void|s32|int|u32|s16|u16|u8|s8)\s*\(\s*\*\s*(\w+)\s*\[\s*(\d+)\s*\]\s*\)'
            r'\s*\(([^()]*)\))\s*;\s*\}\s*(\w*)\s*;',
            masked, re.M):
        tag, ptr, m1, n1, ret, m2, n2, params, name = match.groups()
        if bool(tag) == bool(name):
            continue
        table_type = name or 'struct ' + tag
        if ptr is not None:
            if ptr in pointer_types:
                tables.append((ptr, m1, n1, table_type))
        else:
            tables.append(('%s (*)(%s)' % (ret, params.strip()), m2, n2, table_type))
    for pointer, member, count, table_type in tables:
        if not 0 < int(count) <= 4096:
            raise ValueError('function table size exceeds generation budget')
        type_re = r'\s+'.join(map(re.escape, table_type.split()))
        globals_ = set(re.findall(
            rf'^extern\s+{type_re}\s+(\w+)\s*;', masked, re.M))
        locals_ = set(re.findall(
            rf'^\s*{type_re}\s+(\w+)\s*;', masked, re.M))
        for assignment in re.finditer(r'\b(\w+)\s*=\s*(\w+)\s*;', masked):
            target, source = assignment.groups()
            if target not in locals_ or source not in globals_:
                continue
            replacement = f'''do {{
        const u32 *tk_fn_src = (const u32 *)(const void *)&{source};
        s32 tk_fn_i;
        for (tk_fn_i = 0; tk_fn_i < {count}; tk_fn_i++) {{
            {target}.{member}[tk_fn_i] = ({pointer})overlay_resolve(tk_fn_src[tk_fn_i]);
        }}
    }} while (0);'''
            edits.append((assignment.start(), assignment.end(), replacement))
        # Address-named externs may denote absorbed heads with no standalone
        # module definition. Resolve comparisons through the same map as the
        # copied cells, preserving function identity on the host.
        for comparison in re.finditer(
                rf'(\b\w+\.{member}\s*\[[^\]]+\]\s*[!=]=\s*)(func_([0-9A-Fa-f]{{8}}))\b',
                masked):
            if comparison.group(0).split('.')[0] not in locals_:
                continue
            replacement = (comparison.group(1) +
                           f'({pointer})overlay_resolve(0x{comparison.group(3)}u)')
            edits.append((comparison.start(), comparison.end(), replacement))
    if not edits:
        return content
    for start, end, replacement in sorted(set(edits), reverse=True):
        content = content[:start] + replacement + content[end:]
    return 'extern void *overlay_resolve(u32);\n' + content
