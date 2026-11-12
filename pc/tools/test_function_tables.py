# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from pathlib import Path
import tempfile

from safe_process import run

import os
import re

from ps1_function_tables import adapt_function_tables


def table_copies(text):
    """`local = NAME;` assignments whose local is declared (in the 4 KB
    before) as a struct holding an array of function pointers"""
    ptrs = set(re.findall(r'typedef\s+[^;{}]+?\(\s*\*\s*(\w+)\s*\)\s*\([^;{}]*\)\s*;', text))
    types = set()
    for m in re.finditer(r'(?:typedef\s+struct\s*(\w*)|struct\s+(\w+))\s*\{([^{}]*)\}\s*(\w*)\s*;', text):
        body = m.group(3)
        if re.search(r'\(\s*\*\s*\w+\s*\[', body) or any(
                re.search(r'\b%s\s+\w+\s*\[' % t, body) for t in ptrs):
            for n in (m.group(4), m.group(1) and 'struct ' + m.group(1),
                      m.group(2) and 'struct ' + m.group(2)):
                if n:
                    types.add(' '.join(n.split()))
    out = []
    for m in re.finditer(r'^\s*(\w+)\s*=\s*(\w+)\s*;', text, re.M):
        before = text[max(0, m.start() - 4000):m.start()]
        decl = list(re.finditer(r'^\s*((?:struct\s+)?\w+)\s+%s\s*;' % re.escape(m.group(1)),
                                before, re.M))
        if decl and ' '.join(decl[-1].group(1).split()) in types:
            out.append(m.group(0).strip())
    return out


class FunctionTableTests(unittest.TestCase):
    def test_function_table_copy_and_identity(self):
        source = '''typedef void (*Fn)(int);
typedef struct { Fn ptrs[76]; } Table;
extern Table D_8015BF30;
void dispatch(void) {
    Table table;
    table = D_8015BF30;
    if (table.ptrs[0] == func_80133E78) table.ptrs[0] = other;
    table.ptrs[0](128);
}
'''
        result = adapt_function_tables(source)
        self.assertIn('tk_fn_i < 76', result)
        self.assertIn('table.ptrs[tk_fn_i] = (Fn)overlay_resolve', result)
        self.assertIn('== (Fn)overlay_resolve(0x80133E78u)', result)
        self.assertIn('table.ptrs[0](128)', result)
        self.assertNotIn('table = D_8015BF30;', result)

    def test_inline_and_tagged_members_use_word_cells(self):
        """`void (*f[N])(s32)` spelled in the struct and `struct Tag {..}`
        tables are the same 4-byte PS1 cells (SHOUGATU shougatu_main4 /
        main6, found by pc/tools/explorer.py).  Negative control: without
        the adaptation the copy takes 8-byte host cells, so entry 1 is
        built from words 2 and 3 and the dispatch calls a garbage pointer."""
        inline = '''typedef int s32;
typedef unsigned int u32;
typedef struct {
    void (*f[3])(s32);
} FnTable31;
extern FnTable31 D_80146304;
extern void fade_in_step(s32);
void dispatch(int i) {
    FnTable31 tbl;
    tbl = D_80146304;
    tbl.f[i](128);
    if (tbl.f[i] == fade_in_step) tbl.f[i](1);
}
'''
        tagged = '''typedef int s32;
typedef unsigned int u32;
typedef void (*FnPtr)(s32);
struct FnTable_80141AA0 {
    FnPtr fp[3];
};
extern struct FnTable_80141AA0 D_801468C4;
void dispatch2(int i) {
    struct FnTable_80141AA0 buf;
    buf = D_801468C4;
    buf.fp[i](128);
}
'''
        fixture = '''typedef unsigned int u32;
u32 D_80146304[4] = {10, 20, 30, 40};
u32 D_801468C4[4] = {20, 10, 30, 40};
static int value;
static void one(int x) { value += x; }
void fade_in_step(int x) { value += 1000 * x; }
void *overlay_resolve(u32 a) { return a == 10 ? (void *)one : a == 20 ? (void *)fade_in_step : 0; }
extern void dispatch(int);
extern void dispatch2(int);
int main(void) {
    dispatch(1);                    /* fade_in_step(128) then (1): 129000 */
    dispatch2(1);                   /* one(128) */
    return value == 129128 ? 0 : 1;
}
'''
        a, b = adapt_function_tables(inline), adapt_function_tables(tagged)
        self.assertIn('tbl.f[tk_fn_i] = (void (*)(s32))overlay_resolve', a)
        self.assertIn('buf.fp[tk_fn_i] = (FnPtr)overlay_resolve', b)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'a.c').write_text(a)
            (p / 'b.c').write_text(b)
            (p / 'fixture.c').write_text(fixture)
            run(['gcc', '-std=c89', str(p / 'a.c'), str(p / 'b.c'), str(p / 'fixture.c'),
                 '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()
            # negative control: the source as matched (no adaptation) copies
            # 8-byte cells and does not get the console's result
            (p / 'a.c').write_text(inline)
            (p / 'b.c').write_text(tagged)
            run(['gcc', '-std=c89', str(p / 'a.c'), str(p / 'b.c'), str(p / 'fixture.c'),
                 '-o', str(p / 'raw')]).require_ok()
            self.assertFalse(run([str(p / 'raw')]).ok)

    def test_typedef_extern_table_call(self):
        """`extern HandlerFn D_80155F10[];` + `D_80155F10[i]()` (BUNKAKEN
        bunkaken_main1): each call reads one 4-byte cell."""
        source = '''typedef void (*HandlerFn)(void);
extern HandlerFn D_80155F10[];
void step(int i) { D_80155F10[i](); }
'''
        result = adapt_function_tables(source)
        self.assertIn('((HandlerFn)overlay_resolve(((const u32 *)D_80155F10)[i]))()', result)
        self.assertNotIn(' D_80155F10[i]()', result)

    def test_generated_modules_copy_no_table_by_value(self):
        """No generated overlay module copies a function-pointer table
        struct by value from a PS1 view (`local = D_x__vN;`): every such
        copy goes through the cell-by-cell resolution.  The scan flags the
        unadapted spelling (negative control)."""
        import glob
        root = Path(__file__).resolve().parents[2]
        bad = []
        for p in sorted(glob.glob(str(root / 'pc/src/overlay_*.c'))):
            bad += ['%s:%s' % (os.path.basename(p), x) for x in table_copies(Path(p).read_text(errors='replace'))]
        self.assertEqual(bad, [])
        sample = '''typedef struct {
    void (*f[31])(s32);
} FnTable31_80140890;
s32 SHOUGATU_shougatu_case4_fade_step()
{
    FnTable31_80140890 tbl;
    tbl = D_80146304__v1;
}
'''
        self.assertEqual(table_copies(sample), ['tbl = D_80146304__v1;'])

    def test_data_structs_and_comments_stay_unchanged(self):
        source = '''/* typedef void (*Fn)(int); */
typedef struct { int values[76]; } Table;
extern Table data;
void run(void) { Table table; table = data; }
'''
        self.assertEqual(adapt_function_tables(source), source)

    def test_integer_table_word_cast_to_a_function(self):
        # ENDING ending_main0: the copied table is s32 words; one is cast
        source = '''typedef struct { s32 data[16]; } DispatchTable16;
extern DispatchTable16 D_8013C3A0;
void ending_main0(void) {
    unsigned char t1;
    DispatchTable16 tbl;
    void (*fn)(s32);
    tbl = D_8013C3A0;
    fn = (void (*)(s32))tbl.data[t1];
    fn(128);
}
'''
        result = adapt_function_tables(source)
        self.assertIn('fn = ((void (*)(s32))overlay_resolve((u32)tbl.data[t1]));', result)
        self.assertIn('extern void *overlay_resolve(u32);', result)

    def test_byte_table_offset_call_reads_one_cell(self):
        # SHOUGATU shougatu_main8: a u8[192] table copy called through
        # `(*(s32 (**)(s32))(tbl.b + i * 4))(60)`; the 97 Christmas party
        # crash-looped on the eight-byte read (date_megumi_skating)
        source = '''typedef unsigned int u32;
typedef int s32;
typedef unsigned char u8;
typedef struct { u8 b[12]; } StepTbl;
extern StepTbl D_80146A30;
extern u8 now_sub_sub_step;
s32 shougatu_main8(void)
{
    StepTbl tbl;
    s32 ret;
    tbl = D_80146A30;
    ret = (*(s32 (**)(s32))(tbl.b + now_sub_sub_step * 4))(60);
    return ret;
}
'''
        result = adapt_function_tables(source)
        self.assertIn('((s32 (*)(s32))overlay_resolve(*(const u32 *)(tbl.b + now_sub_sub_step * 4)))(60)',
                      result)
        fixture = '''typedef unsigned int u32;
u32 D_80146A30[3] = {10, 20, 30};
unsigned char now_sub_sub_step = 1;
static int one(int x) { return x + 1; }
static int two(int x) { return x + 2; }
static int three(int x) { return x + 3; }
void *overlay_resolve(u32 a) { return a == 10 ? (void *)one : a == 20 ? (void *)two : a == 30 ? (void *)three : 0; }
extern int shougatu_main8(void);
int main(void) { return shougatu_main8() == 62 ? 0 : 1; }
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'tbl.c').write_text(result)
            (p / 'fixture.c').write_text(fixture)
            run(['gcc', '-std=c89', '-w', str(p / 'tbl.c'), str(p / 'fixture.c'),
                 '-o', str(p / 'test')]).require_ok()
            run([str(p / 'test')]).require_ok()
            # negative control: the unadapted source reads cells 1 and 2
            # as one eight-byte pointer and does not return 62
            (p / 'raw.c').write_text(source)
            run(['gcc', '-std=c89', '-w', str(p / 'raw.c'), str(p / 'fixture.c'),
                 '-o', str(p / 'raw')]).require_ok()
            self.assertFalse(run([str(p / 'raw')]).ok)

    def test_member_declared_in_place(self):
        # SHOUGATU shougatu_main53e: `void (*f[25])(s32);`, no typedef
        source = '''typedef struct {
    void (*f[25])(s32);
} FnTable25;
extern FnTable25 D_80143DE4;
void shougatu_main53e(void) {
    FnTable25 tbl;
    tbl = D_80143DE4;
    tbl.f[now_sub_sub_step](128);
}
'''
        result = adapt_function_tables(source)
        self.assertIn('tk_fn_i < 25', result)
        self.assertIn('tbl.f[tk_fn_i] = (void (*)(s32))overlay_resolve(tk_fn_src[tk_fn_i]);', result)
        self.assertNotIn('tbl = D_80143DE4;', result)

    def test_direct_retail_function_table_call(self):
        source = '''extern void (*D_801564D0[])(void);
void step(unsigned short *p) { D_801564D0[*p](); }
'''
        result = adapt_function_tables(source)
        self.assertIn('((const u32 *)D_801564D0)[*p]', result)
        self.assertIn('overlay_resolve', result)
        self.assertNotIn('D_801564D0[*p]()', result)

    def test_compiled_copy_widens_two_adjacent_cells(self):
        source = '''typedef unsigned int u32;
typedef int s32;
typedef void (*Fn)(int);
typedef struct { Fn ptrs[2]; } Table;
extern Table data;
void dispatch(void) {
    Table table;
    table = data;
    table.ptrs[0](3);
    table.ptrs[1](5);
}
'''
        fixture = '''typedef unsigned int u32;
u32 data[2] = {10, 20};
static int value;
static void one(int x) { value += x; }
static void two(int x) { value += 2*x; }
void *overlay_resolve(u32 a) { return a == 10 ? (void *)one : (void *)two; }
extern void dispatch(void);
int main(void) { dispatch(); return value == 13 ? 0 : 1; }
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'copy.c').write_text(adapt_function_tables(source))
            (p / 'fixture.c').write_text(fixture)
            r = run(['gcc', '-std=c89', str(p / 'copy.c'), str(p / 'fixture.c'),
                     '-o', str(p / 'test')])
            r.require_ok()
            run([str(p / 'test')]).require_ok()

    def test_integer_table_casts_use_word_stride(self):
        source = '''typedef void (*JumpFunc)(void);
typedef struct { unsigned int words[8]; } JumpTable;
void dispatch(void) {
    JumpTable local;
    ((JumpFunc *)&local)[2]();
    fn = ((void (**)(s32))&local)[3];
}
'''
        result = adapt_function_tables(source)
        self.assertIn('((const u32 *)&local)[2]', result)
        self.assertIn('((const u32 *)&local)[3]', result)
        self.assertNotIn('((JumpFunc *)&local)[2]', result)


if __name__ == '__main__':
    unittest.main()
