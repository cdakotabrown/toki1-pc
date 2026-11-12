# SPDX-License-Identifier: GPL-3.0-or-later
"""Background packet ABI and actual native Makefile header dependencies.

The real matched packet writers and HAL ApplyMatrix are linked. Instrumented
builds exercise caller-saved register clobbering, as in the TRACE build.
BG_DISPATCH_SOURCE selects the legacy dispatcher for negative controls.
"""
import os
from pathlib import Path
import shlex
import tempfile
import unittest
from safe_process import run

ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pc_paths import decomp  # noqa: E402

HARNESS = r"""
#include "bg_render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

s16 GsORGOFSX = 160, GsORGOFSY = 120;

void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *fn, void *site)
{
    static char text[80];
    /* A legal C ABI hook can overwrite every caller-saved register. */
    snprintf(text, sizeof(text), "%p/%p", fn, site);
}
void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *fn, void *site)
{
    (void)fn; (void)site;
}

static u32 xy(int x, int y)
{
    return (u32)(u16)x | ((u32)(u16)y << 16);
}

int main(int argc, char **argv)
{
    BgCellParams p;
    MATRIX m;
    u32 storage[11], expected[9];
    int mode, direct, i, x, y, x1, y1, u1, v1;
    assert(argc == 3);
    mode = atoi(argv[1]);
    direct = atoi(argv[2]);
    memset(&p, 0, sizeof(p));
    memset(&m, 0, sizeof(m));
    for (i = 0; i < 11; i++) storage[i] = 0xa5a5a5a5u;
    p.code = 0x2c; p.r = 128; p.g = 64; p.b = 32;
    p.tpage = 0x98; p.clut = 0x7bc0;
    p.u = 10; p.v = 20; p.x = 100; p.y = 200;
    p.w = 32; p.h = 16; p.mx = 5; p.my = 7;
    p.useMatrix = mode;
    m.m[0][0] = mode ? 8192 : 4096;
    m.m[1][1] = m.m[2][2] = 4096;
    x = mode ? 270 : 265; y = 327;
    x1 = x + (mode ? 64 : 32); y1 = y + 16;
    u1 = mode ? 41 : 42; v1 = mode ? 35 : 36;
    expected[0] = 0x2c204080u;
    expected[1] = xy(x, y);
    expected[2] = 0x7bc0140au;
    expected[3] = xy(x1, y);
    expected[4] = 0x00980000u | (20u << 8) | u1;
    expected[5] = xy(x, y1);
    expected[6] = 0xa5a50000u | ((u32)v1 << 8) | 10u;
    expected[7] = xy(x1, y1);
    expected[8] = 0xa5a50000u | ((u32)v1 << 8) | u1;
#ifndef BG_LEGACY_DISPATCH
    if (direct) {
        const BgPacketHandlerInfo *info;
        info = &bg_packet_handlers[mode ? BG_PACKET_TRANSFORMED
                                      : BG_PACKET_NORMAL];
        assert(info->payload_words == 9);
        assert(info->uses_matrix == (mode != 0));
        assert(!strcmp(info->name, mode ? "_mk_xpndsp" : "_mk_normsp"));
        info->emit((u8 *)(storage + 1), &p, &m);
    } else
#endif
    {
        _mk_spr_packet((u8 *)(storage + 1), &p, &m, NULL);
    }
    for (i = 0; i < 9; i++) {
        if (storage[i+1] != expected[i]) {
            fprintf(stderr, "word %d: %08x != %08x\n", i,
                    storage[i+1], expected[i]);
            return 1;
        }
    }
    assert(storage[0] == 0xa5a5a5a5u);
    assert(storage[10] == 0xa5a5a5a5u);
    return 0;
}
"""


class BackgroundSdkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        harness = folder / 'bg.c'
        harness.write_text(HARNESS)
        cls.binaries = []
        cls.legacy = 'BG_DISPATCH_SOURCE' in os.environ
        source = Path(os.environ.get('BG_DISPATCH_SOURCE',
                     ROOT / 'src/main/pc-port/_mk_spr_packet.c')).resolve()
        extra = shlex.split(os.environ.get('BG_SDK_CFLAGS', ''))
        common = ['cc', '-std=gnu17', '-fpermissive', '-w',
                  '-ffunction-sections', '-fdata-sections',
                  '-Ipc/include/game', '-Ipc/include', '-I' + decomp('include'), *extra,
                  *(['-DBG_LEGACY_DISPATCH'] if cls.legacy else [])]
        for optimization in ('-O0', '-O2'):
            for instrument in (False, True):
                label = optimization[1:] + ('-trace' if instrument else '')
                obj = folder / (label + '.o')
                run([*common, optimization,
                     *(['-finstrument-functions'] if instrument else []),
                     '-c', str(source), '-o', str(obj)],
                    cwd=ROOT, timeout=30).require_ok()
                binary = folder / label
                run([*common, optimization, str(harness), str(obj),
                     decomp('src/main/matched/_mk_normsp.c'),
                     decomp('src/main/matched/_mk_xpndsp.c'), 'pc/hal/gte.c',
                     'pc/tests/gte_test_tables.c',
                     '-Wl,--gc-sections', '-lm', '-o', str(binary)],
                    cwd=ROOT, timeout=30).require_ok()
                cls.binaries.append(binary)

    def test_dispatch_and_typed_metadata_emit_exact_packets(self):
        for binary in self.binaries:
            for mode in (0, 1, -1, 2):
                for direct in ((0,) if self.legacy else (0, 1)):
                    with self.subTest(build=binary.name, mode=mode,
                                      metadata=direct):
                        run([str(binary), str(mode), str(direct)],
                            timeout=10).require_ok()

    def test_cpp_client_links_to_c_hal_and_packet_workers(self):
        if self.legacy:
            self.skipTest('legacy dispatch has no public metadata')
        folder = Path(self.temp.name)
        client = folder / 'client.cpp'
        client.write_text(r'''#include "bg_render.h"
#include "hal.h"
#include "bg_render.h"
#include <cassert>
#include <cstring>
static_assert(sizeof(bool) == 1, "C++ bool must remain a language type");
s16 GsORGOFSX = 160, GsORGOFSY = 120;
int main()
{
    BgCellParams params = {};
    MATRIX matrix = {};
    u32 packet[9] = {};
    InitGeom();
    params.code = 0x2c;
    params.r = params.g = params.b = 128;
    params.w = params.h = 1;
    matrix.m[0][0] = matrix.m[1][1] = matrix.m[2][2] = 4096;
    for (int mode = 0; mode < BG_PACKET_HANDLER_COUNT; ++mode) {
        params.useMatrix = mode;
        _mk_spr_packet(reinterpret_cast<u8 *>(packet), &params, &matrix, 0);
        assert(packet[0] == 0x2c808080u);
        u32 via_metadata[9] = {};
        const BgPacketHandlerInfo &info = bg_packet_handlers[mode];
        assert(info.payload_words == BG_PACKET_PAYLOAD_WORDS);
        info.emit(reinterpret_cast<u8 *>(via_metadata), &params, &matrix);
        assert(!std::memcmp(packet, via_metadata, sizeof(packet)));
    }
    return 0;
}
''')
        extra = shlex.split(os.environ.get('BG_SDK_CFLAGS', ''))
        objects = [folder / 'O2.o']
        for index, source in enumerate((decomp('src/main/matched/_mk_normsp.c'),
                                      decomp('src/main/matched/_mk_xpndsp.c'),
                                      'pc/hal/gte.c',
                                      'pc/tests/gte_test_tables.c')):
            obj = folder / ('cpp-worker-' + str(index) + '.o')
            run(['cc', '-std=gnu17', '-w', '-O2', '-ffunction-sections',
                 '-fdata-sections', '-Ipc/include/game', '-Ipc/include',
                 '-I' + decomp('include'), *extra, '-c', source, '-o', str(obj)],
                cwd=ROOT, timeout=30).require_ok()
            objects.append(obj)
        binary = folder / 'cpp-client'
        run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
             # Psy-Q's integer names overlap GCC's complex math builtins.
             '-fno-builtin-csin', '-fno-builtin-ccos',
             '-Ipc/include/game', '-Ipc/include', *extra, str(client),
             *map(str, objects), '-Wl,--gc-sections', '-lm', '-o', str(binary)],
            cwd=ROOT, timeout=30).require_ok()
        run([str(binary)], timeout=10).require_ok()

    def test_shared_header_compiles_alone_in_c89(self):
        folder = Path(self.temp.name)
        source = folder / 'header.c'
        source.write_text('#include "bg_render.h"\n'
                          'int main(void) { return 0; }\n')
        run(['cc', '-std=c89', '-Wall', '-Wextra', '-Werror',
             '-Ipc/include/game', '-Ipc/include', str(source),
             '-o', str(folder / 'header')], cwd=ROOT, timeout=30).require_ok()


class HeaderDependencyTests(unittest.TestCase):
    def make(self, source, build, *extra):
        target = str(build / 'obj/fixture.o')
        result = run(['make', '-f', 'pc/Makefile', target,
                      'ALL_SRCS=' + str(source), 'BUILD_DIR=' + str(build),
                      'CFLAGS=-std=c89', *extra], cwd=ROOT, timeout=30)
        result.require_ok()
        return result

    def check_value(self, obj, expected):
        caller = obj.parent / 'check.c'
        binary = obj.parent / 'check'
        caller.write_text('extern int fixture(void);\n'
                          'int main(void) { return fixture() != '
                          + str(expected) + '; }\n')
        run(['cc', str(caller), str(obj), '-o', str(binary)],
            cwd=ROOT, timeout=30).require_ok()
        run([str(binary)], timeout=10).require_ok()

    def test_header_edit_and_deleted_source_tier_rebuild_actual_object(self):
        with tempfile.TemporaryDirectory(prefix='bg-deps-') as directory:
            base = Path(directory)
            build = base / 'build'
            old = base / 'old'
            new = base / 'new'
            old.mkdir(); new.mkdir()
            header = old / 'layout.h'
            header.write_text('#define VALUE 7\n')
            first = old / 'fixture.c'
            first.write_text('#include "layout.h"\n'
                             'int fixture(void) { return VALUE; }\n')
            self.make(first, build)
            obj = build / 'obj/fixture.o'
            self.assertEqual(self.make(first, build, '-q').returncode, 0)
            old_bytes = obj.read_bytes()
            self.check_value(obj, 7)
            # Future mtime avoids relying on filesystem timestamp resolution.
            header.write_text('#define VALUE 29\n')
            newer = obj.stat().st_mtime + 2
            os.utime(header, (newer, newer))
            self.make(first, build)
            self.assertNotEqual(obj.read_bytes(), old_bytes)
            self.check_value(obj, 29)
            # The next source tier does not include the old header/source.
            # Old dependency files must not keep a deleted tier alive.
            second = new / 'fixture.c'
            second.write_text('int fixture(void) { return 91; }\n')
            older = obj.stat().st_mtime - 60
            os.utime(second, (older, older))
            first.unlink(); header.unlink()
            self.make(second, build)
            self.check_value(obj, 91)
            self.assertEqual(self.make(second, build, '-q').returncode, 0)
            self.assertIn(str(second), (build / 'obj/.srcmap').read_text())
            # An object from before dependency tracking must rebuild once.
            dep = build / 'obj' / (str(second.with_suffix('')).replace('/', '_') + '.d')
            dep.unlink()
            self.make(second, build)
            self.assertTrue(dep.exists())
            self.assertEqual(self.make(second, build, '-q').returncode, 0)
