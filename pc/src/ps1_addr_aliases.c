/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ps1_addr_aliases.c — absolute-address aliases for PS1 sprite-mesh
 * pointer-table symbols.
 *
 * See pc/src/main.c for the matching mmap(0x800C0000, 0x10000) that
 * backs these addresses. sprite_slot_disp (sprite emit driver) reads
 * these as `(u32)&basyo_init_tbl1` etc. — the .set here resolves those
 * to their PS1 absolute addresses at link time so the loads land
 * inside the mapped region.
 */

__asm__(
    ".globl basyo_init_tbl1\n"
    ".set   basyo_init_tbl1, 0x800CFC5C\n"
    /* LIBGTE rcossin_tbl: 4096 (sin | cos << 16) words, retail data loaded
     * from the EXE.  pc/hal/gte.c RotMatrix indexes it like FGO_01. */
    ".globl rcossin_tbl\n"
    ".set   rcossin_tbl, 0x800E4C00\n"
    /* LIBGTE MSC01's square-root table: 192 s16 entries (SquareRoot0/12). */
    ".globl gte_sqrt_tbl\n"
    ".set   gte_sqrt_tbl, 0x800E45D0\n"
    /* Address-only uses (2026-10-03: serifu_run stores &now_serifu /
     * &now_serifu_in, StoreImage passes &libgpu_storeimage_name_str -- the matched C used to
     * spell them as raw constants; the value stored is unchanged). */
    ".globl now_serifu\n"
    ".set   now_serifu, 0x800D0648\n"
    ".globl now_serifu_in\n"
    ".set   now_serifu_in, 0x800D064C\n"
    ".globl libgpu_storeimage_name_str\n"
    ".set   libgpu_storeimage_name_str, 0x800E3B68\n"
    ".globl basyo_init_tbl2\n"
    ".set   basyo_init_tbl2, 0x800CFF14\n"
    ".globl _sys_default_tpage_set_tbl2\n"
    ".set   _sys_default_tpage_set_tbl2, 0x800CFC30\n"
    ".globl _sys_default_tpage_set_tbl3\n"
    ".set   _sys_default_tpage_set_tbl3, 0x800CFC34\n"
    ".globl _sys_default_tpage_set_tbl4\n"
    ".set   _sys_default_tpage_set_tbl4, 0x800CFC38\n"
    ".globl _sys_default_tpage_set_tbl5\n"
    ".set   _sys_default_tpage_set_tbl5, 0x800CFC3C\n"
    ".globl _sys_default_tpage_set_tbl1\n"
    ".set   _sys_default_tpage_set_tbl1, 0x800CC4D0\n"
    /* Short CLUT/GPU tables iterated by hud_palette_init (boot_data_load).
     * Loop bodies `while (p < &hud_palette_init_tbl4)` and `while (p != &hud_palette_init_tbl2)`
     * require the symbols to be contiguous in the 0x800C0000 mmap. Init
     * values are written by init_ps1_clut_tables() post-mmap. */
    ".globl hud_palette_init_tbl0\n"
    ".set   hud_palette_init_tbl0, 0x800CC520\n"
    ".globl hud_palette_init_tbl1\n"
    ".set   hud_palette_init_tbl1, 0x800CC700\n"
    ".globl hud_palette_init_tbl2\n"
    ".set   hud_palette_init_tbl2, 0x800CC720\n"
    ".globl icon_init_tbl2\n"
    ".set   icon_init_tbl2, 0x800CC740\n"
    ".globl hud_palette_init_tbl3\n"
    ".set   hud_palette_init_tbl3, 0x800CC7C0\n"
    ".globl hud_palette_init_tbl4\n"
    ".set   hud_palette_init_tbl4, 0x800CC840\n"
    /* Logo TIM header fields used by hizuke_init for slots 16-23.
     * Backed by the 0x800D0000 mmap in pc/src/main.c, seeded from
     * _game_data_blob + 0x4A000. */
    ".globl D_800DFC30\n"
    ".set   D_800DFC30, 0x800DFC30\n"
    ".globl D_800DFC5C\n"
    ".set   D_800DFC5C, 0x800DFC5C\n"
    ".globl D_800DFF14\n"
    ".set   D_800DFF14, 0x800DFF14\n"
    ".globl D_800DFF54\n"
    ".set   D_800DFF54, 0x800DFF54\n"
);
