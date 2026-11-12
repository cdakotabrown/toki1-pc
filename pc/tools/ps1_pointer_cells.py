# SPDX-License-Identifier: GPL-3.0-or-later
"""Adapt explicit pointer-cell accesses in retail overlay records.

Only *(T **)(address) is recognized. Native local pointers and declarations
are unchanged. Cells remain u32, with widening at reads and narrowing at
stores; pointer arithmetic retains sizeof(T). Unknown C forms fail generation
rather than silently changing a store into an rvalue.
"""
import re

POINTEE = r'(?:(?:const|volatile|unsigned|signed|struct)\s+)*[A-Za-z_]\w*'
# *(T **)(addr), *(T ***)(addr): group 1 is the cell's own type minus one `*`
CELL = re.compile(r'\*\s*\(\s*(' + POINTEE + r'(?:\s*\*)*)\s*\*\s*\*\s*\)\s*\(')
TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)


def mask(text):
    return TOKENS.sub(lambda m: ' ' * len(m.group()), text)


def close_paren(text, start):
    depth = 1
    for i in range(start, len(text)):
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return i
    raise ValueError('unterminated PS1 pointer-cell address')


def close_bracket(text, start):
    """index of the `]` closing the subscript opened just before start"""
    depth = 1
    for i in range(start, len(text)):
        if text[i] in '[(':
            depth += 1
        elif text[i] in '])':
            depth -= 1
            if depth == 0:
                if text[i] != ']':
                    break
                return i
        elif text[i] in ';{}':
            break
    raise ValueError('unterminated PS1 pointer-cell subscript')


def adapt_pointer_cells(content, tables=None, cells=True):
    """`content` with PS1 pointer cells read and written 4 bytes wide.

    tables: the extern pointer arrays to adapt (None: every `extern T
    *name[]`); cells: also adapt `*(T **)(address)`.  The main-binary
    build adapts only named tables (pc/tools/adapt_ps1_cells.py)."""
    if tables is not None:
        tables = set(tables)
    return _adapt(content, tables, cells)


def _adapt(content, tables, do_cells):
    # Extern pointer arrays are packed words too. Express each indexed
    # cell through the same load/store adapter, retaining its pointed type.
    # The subscript is matched with its brackets balanced, so an index that
    # itself subscripts (`t[s[i].k]`) is carried whole.  `&t[i]` is the
    # ADDRESS of a cell, not a cell access: it is left as written (its
    # consumer walks cells through a `T **` and needs an override that
    # steps 4 bytes -- adapting the subscript would take the address of an
    # rvalue).  `&t[i][j]` is the address of an element the cell points
    # at: the cell is read like any other.
    code = mask(content)
    cells = {}
    spans = []
    for declaration in re.finditer(
            r'^extern\s+(' + POINTEE + r')\s*\*\s*(\w+)\s*\[[^\]]*\]\s*;',
            code, re.M):
        typ, name = declaration.groups()
        if tables is not None and name not in tables:
            continue
        cells[name] = typ
        spans.append(declaration.span())
    if cells:
        access_re = re.compile(r'\b(' + '|'.join(map(re.escape, cells)) + r')\s*\[')
        # Rightmost access first: an access inside another one's subscript
        # starts later, so it is rewritten before the subscript enclosing it
        # is measured again.
        while True:
            code = mask(content)
            found = None
            for access in access_re.finditer(code):
                if any(a <= access.start() < b for a, b in spans):
                    continue
                before = code[max(0, access.start() - 16):access.start()].rstrip()
                if before.endswith('&') and not before.endswith('&&'):
                    # `&t[i]` is a cell address (left alone); `&t[i][j]` /
                    # `&t[i]->f` / `&t[i].f` take the address of what the
                    # cell points at, so the cell itself is read
                    tail = code[close_bracket(code, access.end()) + 1:].lstrip()
                    if not tail.startswith(('[', '.', '->')):
                        continue
                found = access
            if found is None:
                break
            name = found.group(1)
            end = close_bracket(code, found.end())
            replacement = f'*({cells[name]} **)((u32 *){name} + ({content[found.end():end]}))'
            delta = len(replacement) - (end + 1 - found.start())
            spans = [(a + delta if a > found.start() else a, b + delta if b > found.start() else b)
                     for a, b in spans]
            content = content[:found.start()] + replacement + content[end + 1:]
    if not do_cells and not cells:
        return content
    code = mask(content)
    output = []
    cursor = 0
    own = re.compile(r'\(u32 \*\)(?:' + '|'.join(map(re.escape, cells)) + r')\b') \
        if cells else None
    while match := CELL.search(code, cursor):
        if not do_cells and not (own and own.match(code, match.end())):
            # only the table accesses rewritten above
            output.append(content[cursor:match.end()])
            cursor = match.end()
            continue
        output.append(content[cursor:match.start()])
        end = close_paren(code, match.end())
        address = _adapt(content[match.end():end], tables, do_cells)
        cell = f'*(u32 *)({address})'
        typ = match[1]
        after = end + 1
        op = re.match(r'\s*(\+=|-=|=(?!=))', code[after:])
        if op:
            # These are statement stores in the retail source. A larger C
            # assignment expression needs explicit treatment, not a guess.
            line = code[code.rfind('\n', 0, match.start()) + 1:match.start()]
            if line.strip():
                raise ValueError('embedded PS1 pointer-cell store needs an override')
            rhs_start = after + op.end()
            depth = 0
            rhs_end = rhs_start
            while rhs_end < len(code):
                c = code[rhs_end]
                if c in '([{':
                    depth += 1
                elif c in ')]}':
                    depth -= 1
                elif c == ';' and depth == 0:
                    break
                rhs_end += 1
            if rhs_end == len(code) or depth:
                raise ValueError('unterminated PS1 pointer-cell store')
            rhs = _adapt(content[rhs_start:rhs_end].strip(), tables, do_cells)
            if op[1] == '=':
                output.append(f'{cell} = (u32)(unsigned long)({rhs})')
            else:
                if typ == 'void':
                    raise ValueError('void pointer arithmetic needs an override')
                output.append(f'{cell} {op[1]} ({rhs}) * sizeof({typ})')
            cursor = rhs_end
        else:
            output.append(f'(({typ} *)(unsigned long)({cell}))')
            cursor = after
    output.append(content[cursor:])
    return ''.join(output)


# ---------------------------------------------------------------------------
# Pointer CHAINS: a subscript of an expression whose type is a pointer to a
# pointer reads a 4-byte PS1 cell.  Two spellings carry the type in the text:
#
#   a cast            ((u32 ***)a3)[i][j]      (serifu_cmd_exec)
#   a generator view  D_8014CE68__v2[38]       (`typedef u8 **D_8014CE68__t2`)
#
# Each subscript taken while the expression still has two or more `*` becomes
# a zero-extended 4-byte load; the last subscript (the pointee) is untouched.
# A store into a cell of the chain raises (it needs an override).

CHAIN_CAST = re.compile(r'\(\s*\(\s*(' + POINTEE + r')\s*(\*{2,})\s*\)')


def _chain(code, content, pos, typ, stars, expr, views, at=None):
    """Consume `[i]` subscripts at `pos` while stars >= 2; return (end,
    replacement) or None when no cell is read."""
    changed = False
    while stars >= 2:
        m = re.match(r'\s*\[', code[pos:])
        if not m:
            break
        start = pos + m.end()
        end = close_bracket(code, start)
        idx = adapt_chains(content[start:end], views)
        stars -= 1
        expr = '((%s %s)(unsigned long)((u32 *)(%s))[%s])' % (typ, '*' * stars, expr, idx)
        pos = end + 1
        changed = True
    if not changed:
        return None
    after = code[pos:].lstrip()
    head = code[:at].rstrip() if at is not None else ''
    deref = head.endswith('*') and not re.search(r'[\w)\]]\s*\*$', head)
    if stars >= 1 and not deref and re.match(r'(=(?!=)|\+=|-=|\+\+|--)', after):
        raise ValueError('store into a PS1 pointer cell through a chain needs an override')
    return pos, expr


def view_types(content):
    """{view macro: (pointee, stars)} for the generator's views of
    pointer-to-pointer cells in this text."""
    code = mask(content)
    tdefs = {m.group(3): (m.group(1).strip(), len(m.group(2)))
             for m in re.finditer(r'^typedef\s+(' + POINTEE + r')\s*(\*{2,})\s*(\w+__t\d+)\s*;',
                                  code, re.M)}
    out = {}
    for m in re.finditer(r'^#define\s+(\w+__v\d+)\s+\(\((\w+__t\d+)\)\(unsigned long\)', code, re.M):
        if m.group(2) in tdefs:
            out[m.group(1)] = tdefs[m.group(2)]
    return out


def _unary_view_derefs(content, views):
    """`*V` (unary) of a pointer-to-pointer view is `V[0]`, and `**V` is
    `V[0][0]` (SHOUGATU shougatu_serifu_step's `(**D_80143BB4)[0]`: with
    one level rewritten the second `*` read the table's 4-byte cell 8
    bytes wide)."""
    if not views:
        return content
    code = mask(content)
    edits = []
    for m in re.finditer(r'((?:\*\s*)+)(' + '|'.join(map(re.escape, views)) + r')\b', code):
        stars = [m.start() + i for i, ch in enumerate(m.group(1)) if ch == '*']
        while stars:
            before = code[:stars[0]].rstrip()
            if before and (before[-1].isalnum() or before[-1] in '_)]'):
                stars = stars[1:]                     # a multiplication
                continue
            break
        if not stars:
            continue
        before = code[:stars[0]].rstrip()
        if before.endswith('define') or code[m.end():].lstrip().startswith('['):
            continue
        edits.append((stars[0], m.end(), '%s%s' % (m.group(2), '[0]' * len(stars))))
    for a, b, rep in sorted(edits, reverse=True):
        content = content[:a] + rep + content[b:]
    return content


CELL_STORE = re.compile(
    r'\(\*\((?:s32|u32|u8|s8|u16|s16|void|char|int) \*\*\)&([^;=()\n]+)\) = ([^;\n]+);')


def adapt_cell_stores(content):
    """`(*(T **)&cell) = p;` stores a pointer into a 4-byte PS1 cell
    (NAME_ENT name_ent_cdlevel_disp: class_w[98].hdr).  On LP64 the store
    is 8 bytes and runs into the next field; write the low 32 bits."""
    return CELL_STORE.sub(
        lambda m: '(*(u32 *)&%s) = (u32)(unsigned long)(%s);' % (m.group(1), m.group(2)),
        content)


def adapt_chains(content, views=None):
    """Rewrite pointer-chain subscripts (casts, and `views`) to cell loads."""
    views = views or {}
    content = _unary_view_derefs(content, views)
    code = mask(content)
    edits = []
    for m in CHAIN_CAST.finditer(code):
        if any(a <= m.start() < b for a, b, _ in edits):
            continue
        # ((T **)X) -- the outer parenthesis closes after the operand
        close = close_paren(code, m.start() + 1)
        operand = adapt_chains(content[m.end():close], views)
        typ = re.sub(r'\s+', ' ', m.group(1))
        r = _chain(code, content, close + 1, typ, len(m.group(2)),
                   '(%s %s)(%s)' % (typ, m.group(2), operand.strip()), views, m.start())
        if r:
            edits.append((m.start(), r[0], r[1]))
    if views:
        vre = re.compile(r'\b(' + '|'.join(map(re.escape, views)) + r')\b(?=\s*\[)')
        for m in vre.finditer(code):
            if any(a <= m.start() < b for a, b, _ in edits):
                continue
            typ, stars = views[m.group(1)]
            r = _chain(code, content, m.end(), typ, stars, m.group(1), views, m.start())
            if r:
                edits.append((m.start(), r[0], r[1]))
    for a, b, rep in sorted(edits, reverse=True):
        content = content[:a] + rep + content[b:]
    return content
