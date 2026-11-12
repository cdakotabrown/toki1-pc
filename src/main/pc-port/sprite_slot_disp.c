/*
 * PC-PORT OVERRIDE of sprite_slot_disp / func_8004863C (moved verbatim from
 * src/main/semi-matched on 2026-09-17 when the decomp body was rewritten to be
 * faithful to the PS1 binary).  This copy keeps the PC-only mmap guards
 * (PC_MESH_PTR_OK), the debug_log tracing and the direct class_max loop bound
 * that the native build depends on.  Every @DivergesFrom note below still
 * applies.  The PS1-faithful body lives in src/main/semi-matched/sprite_slot_disp.c.
 */
/*
 * Function: sprite_slot_disp + func_8004863C
 * Address: 0x80047A00
 * Size: 3280 bytes (820 instructions)
 * Region: Game (0x80041000-0x80085FFF)
 * Compiler: IDO 5.2
 * Status: CONFIRMED UNMATCHABLE FROM C — see config/c_unmatchable.txt
 *
 * Complex sprite/animation rendering function.
 *
 * BLOCKER: Jump table in library data region
 * ==========================================
 * Target uses TWO switch jump tables (cases fv=1,2,4,8 out of range 0-32):
 *   Table 1 at 0x800b613c: 33 entries (tpage computation for af!=0 path)
 *   Table 2 at 0x800b61c0: 33 entries (tpage computation for af==0 path)
 * Both tables are in the LIBRARY DATA REGION (0x80086000+), NOT game region.
 *
 * IDO 5.2 requires 5+ case labels for jump table generation:
 *   - With 4 cases (1,2,4,8): IDO generates if-else chains → 748 insns (sprite_slot_disp body)
 *   - Target has 783 insns (35-instruction gap from missing jr+dispatch overhead)
 *   - With 5th dummy case: IDO generates 9-entry table (range 0-8) in game .rodata
 *     but target has 33-entry tables (range 0-32) at library addresses 0x800b613c/61c0
 *   - Compiled `lui at,0x0; lw t8,0(at)` relocation cannot match target's
 *     `lui at,0x800b; lw t8,24892(at)` hardcoded address in .text
 *   - Even with relocation fixup, wrong .rodata at wrong address breaks SHA1
 *
 * SECONDARY FINDINGS (analysis conducted 2026-03-27):
 * ====================================================
 * Switch cases (CORRECTED): fv=1,2,4,8 (NOT 0,4,30,31 as in WIP)
 *   Table 1 dispatch handlers (af!=0, v1&0x80!=0):
 *     case 1: rd.tpage = (s0[3]&0xF0)>>4<<4
 *     case 2: rd.tpage = (s0[3]&0xE0)>>4<<4
 *     case 4: rd.tpage = (s0[3]&0xC0)>>4<<4
 *     case 8: rd.tpage = (s0[3]&0x80)>>4<<4
 *     then, for EVERY af != 0 arm (af&0x60 == 0x20/0x40 included):
 *     rd.clutv = (af&0x1F)+480.  [Corrected 2026-10-30 from the byte-perfect
 *     matched body; this copy used to skip clutv for cases 1/2/4, for f5
 *     bit 7 clear and for af&0x60 0x20/0x40, leaving it stale.]
 *   Table 2 dispatch handlers (af==0):
 *     case 1: rd.tpage = (s0[3]&0xF0)>>4<<4
 *     case 2: rd.tpage = (s0[3]&0xE0)>>4<<4
 *     case 4: rd.tpage = (s0[3]&0xC0)>>4<<4
 *     case 8: rd.tpage = (s0[3]&0x80)>>4<<4
 *     default: rd.tpage = 0
 *     then: rd.clutv = ((s0[3]&0xF00)>>8) + (f5&0x80 ? 496 : 480)
 *
 * Division type: frame_counter is u32 → use divu (not div)
 *   WIP uses `(s32)frame_counter` which generates div; correct type keeps divu
 *
 * Base pointer at sp+236: volatile s32 sp236 = (s32)screen_w forces stack
 *   allocation AND triggers cross-prologue scheduling (confirmed via testing)
 *
 * func_8004863C (tail function): matchable separately with:
 *   - Declare s32 counter first (gets v0), then u8 *ptr (gets v1)
 *   - @CompileFlags: -Wo,-loopunroll,0
 *   - @PostProc: swap_registers a0 a1 (IDO puts -1 in a1, target has it in a0)
 *
 * Stack layout (target: 240 bytes / IDO natural: 224 bytes):
 *   sp+236 (IDO+220): base ptr = (s32)screen_w, volatile s32
 *   sp+200-235 (IDO+184-219): rd struct (36 bytes)
 *   sp+196 (IDO+180): sp_c4 (s32, frame_idx saved)
 *   sp+192 (IDO+176): sp_c0 (s32, anim_idx saved)
 *   sp+144 (IDO+128): counter (s32, loop index)
 *   sp+142 (IDO+126): s2 (u16, palette persists across calls)
 *   sp+141 (IDO+125): bri (u8, brightness)
 *   sp+124 (IDO+108): sp_7c (volatile s32 ptr, = (s32)(vd+8))
 *   sp+16,20: stack args 5,6 for func_800B4850
 *
 * @CompileFlags: -Wo,-loopunroll,0
 * Status(wave25): honest 1092/820 instructions (delta +272), 1.3% exact; the PC-port debug scaffolding is itself a large part of the surplus
 * @Measured: 2026-09-06 11/820 exact (1.3%), compiled 1090 vs target 820 words, Fixup 1, OVERRUN +2 -- OVERRUN
  *
 * 2026-09-17 WRONG-GLOBALS SWEEP: checked, NOT a naming defect.
 * audit_main_relocs reports the address(es) below as absent because retail
 * reaches them through a HELD BASE REGISTER while IDO folds the displacement
 * into a %hi/%lo pair of its own.  The C names the right storage; the folded
 * address is a real sub-object of a base the window does materialise.  This is
 * a codegen-shape difference, not a wrong address.
 * Specifically 0x801275F3, 0x801282B3 and 0x801286F3 --
 * class_w + 1091 / + 4355 / + 5443.  Retail materialises class_w once
 * (0x80047Axx, `addiu $aN,29104`) and indexes it.
*/
#include "types.h"
#include <stdint.h>

/* PC-port guard: a slot's header / cel / animation pointers are 32-bit
 * PS1 handles.  On PS1 every handle reads main RAM (stale slots read stale
 * data); on the host only the 2 MB PS1 RAM block that pc/src/main.c maps
 * at 0x80000000 is readable, so a handle outside it (an uninitialised or
 * stale slot) is skipped instead of faulting.
 *
 * 2026-10-30: the window used to be 0x800C0000-0x800E0000, from before the
 * full RAM block was mapped.  It rejected every slot whose data the game
 * loads from CD, e.g. the title character (class_w slot 96, cal_cw_*, set
 * by title_data_read with hdr 0x801EAB20 inside TITLE.NBN at 0x80180000):
 * the slot was skipped every frame, so its 8bpp cels were never uploaded
 * (search_tpage8) or sorted (GsSortSprite).  The narrow window also hid
 * a writer bug: the matched basyo_init declared the slot handles as
 * native pointers, wrote 88-byte LP64 records over the 68-byte slots and
 * left slot 9 corrupt in scene 54; src/main/pc-port/basyo_init.c now
 * stores 32-bit handles at the PS1 offsets.
 *
 * @DivergesFrom: target sprite_slot_disp has no such check (on PS1 the
 *   full main RAM is mapped, so out-of-range reads just return stale
 *   data instead of SEGV). */
#define PC_MESH_PTR_OK(p) \
    ((uintptr_t)(p) >= 0x80000000u && (uintptr_t)(p) < 0x80200000u)

typedef struct {
    s32 base;
    s16 posX, posY, dimX, dimY, palette;
    u8  texU, texV;
    s16 tpage, clutv;
    u8  r, g, b, pad;
    s16 scaleX, scaleY, flipX, flipY;
    s32 timing;
} RD;

extern u32 get_brightness(void);
extern u32 search_tpage(s32, s32, s32);
extern u32 search_tpage8(s32, s32, s32);
extern void func_800B4850(u8 *, u8, u8, u8, u8, u8);
extern void GsSortSprite(RD *, s32, s32);
extern u8 screen_w[];
extern s32 class_max;
extern u32 frame_counter;
extern u8 now_step, now_sub_step, now_sub_sub_step, step_mode;
extern u8 sprite_slot_disp_str0[];
extern s32 activeBuff;
extern u8 class_w[];
extern u8 ot_work[];

extern void debug_log(const char *, ...);
extern void sprite_slot_anim_step(u8 *s, const s16 *row, s32 fi);   /* pc/src/sprite_slot_anim.c */

/* C89: these PC-port trace counters were declared mid-block (a C99
 * construct IDO rejects). Hoisted to file scope — a block-scope
 * static and a file-scope static have identical lifetime and
 * zero-initialisation, so behaviour is unchanged. */
static int trace_count = 0;
static int exit1 = 0;
static int trace2 = 0;
static int slot_seen[160];
static int body_log = 0;
static int disp_log = 0;
static int emit_log = 0;
static int exit2 = 0;

void sprite_slot_disp(void) {
    u8 *base;
    RD rd;
    s32 sp_c4;
    s32 sp_c0;
    s32 counter;
    u16 s2;
    u8 bri;
    uintptr_t sp_7c; /* PC port: must be pointer-wide, not s32 */
    u8 *s1;
    s16 *s0;
    s32 s3, s4, s5, s6, s7, s8;

    if (trace_count < 3) {
        debug_log("sprite_slot_disp ENTER: class_max=%d", class_max);
        trace_count++;
    }

    base = screen_w;
    bri = (u8)get_brightness();
    search_tpage(0x7FFF, counter, 0);
    counter = 0;
    if (class_max < 0) {
        if (exit1 < 3) { debug_log("sprite_slot_disp: early exit (class_max < 0)"); exit1++; }
        return;
    }
    s2 = *(u16 *)((u8 *)&counter + 2); /* lhu s2,142(sp) - loads s2 from its stack slot */
    s1 = class_w;
    s4 = 4096;

    /* DEBUG: Sample specific-slot state at entry */
    {
        extern u8 now_step;
        if ((now_step == 50 || now_step == 64) && trace2 < 3) {
            u8 *b = class_w;
            debug_log("[EMIT-ENTRY] scene=%d slot16[3]=0x%02X slot64[3]=0x%02X slot80[3]=0x%02X",
                      (int)now_step, b[16*68+3], b[64*68+3], b[80*68+3]);
            trace2++;
        }
    }

    do {
        s32 a1f;
        a1f = s1[3];
        if (!(a1f & 0x80)) {
            goto cont;
        }
        /* DEBUG slot trace: one-shot per slot index on scene 50/64 */
        {
            extern u8 now_step;
            if (now_step == 50 || now_step == 64) {
                s32 slot_idx = (s32)(s1 - class_w) / 68;
                if (slot_idx >= 0 && slot_idx < 160 && !slot_seen[slot_idx]) {
                    slot_seen[slot_idx] = 1;
                    debug_log("[SLOT] scene=%d slot=%d mode=%u a1f=0x%02X a0f=0x%02X tpage=0x%04X tim_a=0x%08X tim_b=0x%08X tim_c=0x%08X",
                              (int)now_step, slot_idx,
                              (u32)s1[1], (u32)a1f, (u32)s1[2],
                              (u32)(*(u16 *)(s1 + 20)),
                              (u32)(*(u32 *)(s1 + 12)),
                              (u32)(*(u32 *)(s1 + 16)),
                              (u32)(*(u32 *)(s1 + 52)));
                }
            }
        }
        if ((a1f & 0x40) == 0x40) {
            /* The blink: retail (0x80047A8C..AC) `divu frame_counter, rate + 1;
             * mfhi; beqz -> next slot` hides the slot on the frames the
             * remainder is ZERO, as the matched body's `frame_counter %
             * (rate + 1) != 0` draw test says.  This copy skipped on a
             * non-zero remainder (shown one frame in rate + 1 instead of
             * hidden one in rate + 1); fixed 2026-11-06. */
            u32 t4 = (u32)s1[6] + 1;
            if ((frame_counter % t4) == 0) {
                goto cont;
            }
        }
        {
            s32 a0f = s1[2];
            if (a0f & 0x80) {
                goto cont;
            }
            {
                /* PC-port: sprite slots store pointers as 4-byte u32, but on
                 * x86-64 native pointers are 8 bytes. Read as u32 and zero-
                 * extend so we don't spill into the next field. */
                s32 *pt7 = (s32 *)(uintptr_t)(*(u32 *)(s1 + 52));
                s32 t8v;
                if (!pt7 || !PC_MESH_PTR_OK(pt7)) {
                    goto cont;
                }
                t8v = *pt7;
                if (t8v >= 0) { /* bgez = bit31 clear */
                    func_800B4850(sprite_slot_disp_str0, now_step, now_sub_step, now_sub_sub_step, step_mode, 0);
                    goto ld;
                }
                {
                    s32 *pv1 = (s32 *)(uintptr_t)(*(u32 *)(s1 + 12));
                    s32 t3v;
                    if (!pv1) {
                        goto cont;
                    }
                    t3v = *pv1;
                    if (!(t3v & (s32)0x80000000)) {
                        func_800B4850(sprite_slot_disp_str0, now_step, now_sub_step, now_sub_sub_step, step_mode, 0);
                        goto ld;
                    }
                    {
                        s16 *vd;
                        {
                            (void)t3v;
                        }
                        if (a0f & 0x40) {
                            u32 raw = (u32)pv1[*(s16 *)(s1 + 24)];
                            vd = (s16 *)(uintptr_t)raw;
                        } else {
                            s32 ai = *(s16 *)(s1 + 22);
                            s32 fi = *(s16 *)(s1 + 24);
                            s32 *tbase = (s32 *)(uintptr_t)(*(u32 *)(s1 + 16));
                            s32 *trow;
                            u32 raw;
                            if (!tbase || !PC_MESH_PTR_OK(tbase)) {
                                goto cont;
                            }
                            trow = (s32 *)(uintptr_t)((u32)tbase[ai]);
                            if (!trow || !PC_MESH_PTR_OK(trow)) {
                                goto cont;
                            }
                            raw = (u32)pv1[*(s16 *)((u8 *)trow + fi * 4 + 2)];
                            vd = (s16 *)(uintptr_t)raw;
                            sp_c0 = ai;
                            sp_c4 = fi;
                        }
                        if (!vd || !PC_MESH_PTR_OK(vd)) {
                            goto cont;
                        }
                        s5 = vd[1];
                        s6 = vd[2];
                        if (vd[0] == 0) {
                            goto ar;
                        }
                        s3 = vd[0] - 1;
                        if (s3 < 0) {
                            goto ar;
                        }
                        sp_7c = (uintptr_t)vd + 8;
                        s7 = *(s16 *)(s1 + 38) + s5;
                        s8 = *(s16 *)(s1 + 42) + s6;
                        {
                            extern u8 now_step;
                            if (now_step == 64 && body_log < 200) {
                                s32 slot_idx = (s32)(s1 - class_w) / 68;
                                body_log++;
                                debug_log("[BODY] slot=%d vd[0-2]=%d,%d,%d s1[38,42]=%d,%d s7=%d s8=%d",
                                          slot_idx, (int)vd[0], (int)vd[1], (int)vd[2],
                                          (int)*(s16*)(s1+38), (int)*(s16*)(s1+42),
                                          (int)s7, (int)s8);
                            }
                        }
                        do {
                            s32 t2i;
                            if ((s1[3] & 0x8) && s3 >= 4) {
                                t2i = 3;
                            } else {
                                t2i = s3;
                            }
                            s0 = (s16 *)(sp_7c + t2i * 14);
                            rd.base = *(s32 *)(s1 + 56);
                            if (!(s1[3] & 0x1)) {
                                if (s0[4] & 0x2) {
                                    rd.posX = (s16)s7;
                                    rd.scaleX = (s16)(s0[5] - s5 + s0[0]);
                                    rd.flipX = -4096;
                                } else {
                                    rd.posX = (s16)s7;
                                    rd.flipX = (s16)s4;
                                    rd.scaleX = (s16)(s5 - s0[0]);
                                }
                                if (s0[4] & 0x1) {
                                    rd.posY = (s16)s8;
                                    rd.scaleY = (s16)(s0[6] - s6 + s0[1]);
                                    rd.flipY = -4096;
                                } else {
                                    rd.posY = (s16)s8;
                                    rd.flipY = (s16)s4;
                                    rd.scaleY = (s16)(s6 - s0[1]);
                                }
                            } else {
                                if (s0[4] & 0x2) {
                                    rd.posX = (s16)s7;
                                    rd.scaleX = (s16)(s0[5] - s5 + s0[0]);
                                    rd.flipX = (s16)s4;
                                } else {
                                    rd.posX = (s16)s7;
                                    rd.flipX = -4096;
                                    rd.scaleX = (s16)(s5 - s0[0]);
                                }
                                if (s0[4] & 0x1) {
                                    rd.posY = (s16)s8;
                                    rd.scaleY = (s16)(s0[6] - s6 + s0[1]);
                                    rd.flipY = (s16)s4;
                                } else {
                                    rd.posY = (s16)s8;
                                    rd.flipY = (s16)s4;
                                    rd.scaleY = (s16)(s6 - s0[1]);
                                }
                            }
                            if (s1[3] & 0x4) {
                                rd.posX = (s16)(rd.posX - s5);
                                rd.posY = (s16)(rd.posY - s6);
                            }
                            if (s4 == *(s16 *)(s1 + 26) || s4 == *(s16 *)(s1 + 28)) {
                                rd.dimX = s0[5];
                                rd.dimY = s0[6];
                            } else {
                                rd.dimX = (s16)(s0[5] + 1);
                                rd.dimY = (s16)(s0[6] + 1);
                            }
                            {
                                extern u8 now_step;
                                if (now_step == 64 && disp_log < 500) {
                                    s32 slot_idx = (s32)(s1 - class_w) / 68;
                                    disp_log++;
                                    debug_log("[DISP] slot=%d s1[3]=0x%02X rd.pos=(%d,%d) s0[0,1]=(%d,%d) s5,s6=%d,%d",
                                              slot_idx, (u32)s1[3], (int)rd.posX, (int)rd.posY,
                                              (int)s0[0], (int)s0[1], (int)s5, (int)s6);
                                }
                            }
                            if ((s1[4] & 0x8) == 0x8) {
                                if ((rd.base & 0x03000000) == 0) {
                                    s2 = (u16)search_tpage8(s0[2], counter, 2);
                                } else if ((rd.base & 0x03000000) == 0x01000000) {
                                    s2 = (u16)search_tpage8(s0[2], counter, 1);
                                }
                                rd.palette = (s16)(s2 & 0x1F);
                                {
                                    s32 tv = (u16)s0[2] & 0xFF;
                                    rd.texU = (u8)((tv & 0xF) << 3);
                                    rd.texV = (u8)(((tv >> 4) << 3) + ((s2 & 0x20) << 2));
                                }
                            } else {
                                if ((rd.base & 0x03000000) == 0) {
                                    s2 = (u16)search_tpage(s0[2], counter, 2);
                                } else if ((rd.base & 0x03000000) == 0x01000000) {
                                    s2 = (u16)search_tpage(s0[2], counter, 1);
                                }
                                rd.palette = (s16)(s2 & 0x1F);
                                {
                                    s32 tv = (u16)s0[2] & 0x3F;
                                    rd.texU = (u8)((tv & 0x7) << 4);
                                    rd.texV = (u8)(((tv >> 3) << 4) + ((s2 & 0x20) << 2));
                                }
                            }
                            {
                                u8 af = s1[67];
                                if (af != 0) {
                                    /* matched sprite_slot_disp: every af != 0 slot takes its
                                     * CLUT row from af, whatever page arm ran (the old copy
                                     * jumped past it for af & 0x60 == 0x20/0x40 and for
                                     * f5 cases 1/2/4 or f5 bit 7 clear, leaving rd.clutv stale) */
                                    rd.tpage = 0;
                                    if ((af & 0x60) == 0x20) {
                                        rd.tpage = 256;
                                    } else if ((af & 0x60) == 0x40) {
                                        rd.tpage = 288;
                                    } else if (s1[5] & 0x80) {
                                        switch (s1[5] & 0x7F) {
                                        case 1:
                                            rd.tpage = (s16)(((s0[3] & 0xF0) >> 4) << 4);
                                            break;
                                        case 2:
                                            rd.tpage = (s16)(((s0[3] & 0xE0) >> 4) << 4);
                                            break;
                                        case 4:
                                            rd.tpage = (s16)(((s0[3] & 0xC0) >> 4) << 4);
                                            break;
                                        case 8:
                                            rd.tpage = (s16)(((s0[3] & 0x80) >> 4) << 4);
                                            break;
                                        }
                                    }
                                    rd.clutv = (s16)((af & 0x1F) + 480);
                                } else {
                                    u8 f5 = s1[5];
                                    u32 fv = f5 & 0x7F;
                                    if (fv < 33) {
                                        switch (fv) {
                                        case 1:
                                            rd.tpage = (s16)(((s0[3] & 0xF0) >> 4) << 4);
                                            break;
                                        case 2:
                                            rd.tpage = (s16)(((s0[3] & 0xE0) >> 4) << 4);
                                            break;
                                        case 4:
                                            rd.tpage = (s16)(((s0[3] & 0xC0) >> 4) << 4);
                                            break;
                                        case 8:
                                            rd.tpage = (s16)(((s0[3] & 0x80) >> 4) << 4);
                                            break;
                                        default:
                                            rd.tpage = 0;
                                            break;
                                        }
                                    } else {
                                        rd.tpage = 0;
                                    }
                                    if (f5 & 0x80) {
                                        rd.clutv = (s16)(((s0[3] & 0xF00) >> 8) + 496);
                                    } else {
                                        rd.clutv = (s16)(((s0[3] & 0xF00) >> 8) + 480);
                                    }
                                }
                            }
                            if (s1[3] & 0x20) {
                                rd.r = bri;
                                rd.g = bri;
                                rd.b = bri;
                            } else {
                                u8 rv = s1[64];
                                if (rv || s1[65] || s1[66]) {
                                    rd.r = rv;
                                    rd.g = s1[65];
                                    rd.b = s1[66];
                                } else {
                                    rv = s1[7];
                                    rd.r = rv;
                                    rd.g = rv;
                                    rd.b = rv;
                                }
                            }
                            rd.timing = *(s32 *)(s1 + 32);
                            if (s1[3] & 0x8) {
                                if (!(rd.timing == 0 && rd.flipX == s4 && rd.flipY == s4)) {
                                    rd.dimX = (s16)((u16)rd.dimX - 1);
                                    rd.dimY = (s16)((u16)rd.dimY - 1);
                                }
                            }
                            if (*(s16 *)(s1 + 26) != (s16)s4 || *(s16 *)(s1 + 28) != (s16)s4) {
                                s16 sx = rd.flipX;
                                s16 sy = rd.flipY;
                                rd.flipX = (s16)(*(s16 *)(s1 + 26) * ((sx >= 0) ? (sx >> 12) : ((sx + 4095) >> 12)));
                                rd.flipY = (s16)(*(s16 *)(s1 + 28) * ((sy >= 0) ? (sy >> 12) : ((sy + 4095) >> 12)));
                            }
                            {
                                s32 m2 = activeBuff;
                                m2 = (m2 << 2) + m2;
                                m2 = m2 << 2;
                                /* DEBUG: trace rd values before emit */
                                {
                                    extern u8 now_step;
                                    s32 slot_idx = (s32)(s1 - class_w) / 68;
                                    if (now_step == 64 && slot_idx >= 32 && slot_idx <= 52 && emit_log < 5) {
                                        emit_log++;
                                        debug_log("[EMIT] slot=%d mode=%d af=%d rd.tpage=%d rd.pos=(%d,%d) rd.dim=(%d,%d) rd.UV=(%d,%d)",
                                                  slot_idx, (int)s1[1], (int)s1[67], (int)rd.tpage,
                                                  (int)rd.posX, (int)rd.posY, (int)rd.dimX, (int)rd.dimY,
                                                  (int)rd.texU, (int)rd.texV);
                                    }
                                }
                                if (s1[1] == 0) {
                                    GsSortSprite(&rd, m2 + (s32)ot_work, 0);
                                } else {
                                    GsSortSprite(&rd, m2 + (s32)ot_work, (u16)(s1[1] - 1));
                                }
                            }
                            s3--;
                        } while (s3 >= 0);
                    }
                }
            }
        }
    ar:
        {
            u8 a0f = s1[2];
            if ((a0f & 0x40) || (a0f & 0x1) != 1) {
                goto pa;
            }
            {
                u8 f6 = s1[6];
                s32 a1sc = sp_c0 * 4;
                s32 *aB;
                /* PC-port: slots store pointers as 4-byte u32 — read, zero-extend. */
                {
                    s32 *s16tbl = (s32 *)(uintptr_t)(*(u32 *)(s1 + 16));
                    u32 aBraw;
                    if (!s16tbl || !PC_MESH_PTR_OK(s16tbl)) {
                        goto cont;
                    }
                    if ((f6 & 0x80) && (f6 & 0x7F)) {
                        *(s16 *)(s1 + 8) += (f6 & 0x7F);
                    } else if (f6 != 0) {
                        if ((frame_counter % (u32)f6) == 0) {
                            *(s16 *)(s1 + 8) += 1;
                        }
                    }
                    aBraw = *(u32 *)((u8 *)s16tbl + a1sc);
                    aB = (s32 *)(uintptr_t)aBraw;
                    if (!aB || !PC_MESH_PTR_OK(aB)) {
                        goto cont;
                    }
                }
                /* The frame/tick step of the matched body: pc/src/sprite_slot_anim.c
                 * (2026-11-01: this block kept f2 in a local and dropped the
                 * reverse-play stop, so RPG_BAT's closing message window never
                 * finished; see that file). */
                sprite_slot_anim_step(s1, (const s16 *)aB, sp_c4);
            }
        }
    pa:
        if (s1[3] & 0x10) {
            if (*(s32 *)(s1 + 44)) {
                *(s32 *)(s1 + 36) += *(s32 *)(s1 + 44);
            }
            if (*(s32 *)(s1 + 48)) {
                *(s32 *)(s1 + 40) += *(s32 *)(s1 + 48);
            }
        }
    cont:
    ld:
        {
            /* PC port fix: On PS1, base+4248 = class_max due to contiguous memory.
             * On PC, read the variable directly.
             * The bound is INCLUSIVE: retail 0x800485FC `slt at, class_max,
             * i + 1; beqz at, loop` runs slot class_max too (the matched
             * body's `i <= sv->count`).  This copy stopped one slot early
             * (`class_max > i + 1`), so slot class_max was never drawn:
             * TACO's OPERATIONS screen sets class_max = 1 (taco_load3) and
             * its tutorial text is slot 1 (mes_win_cw), so the pages never
             * showed (2026-11-06). */
            s32 lv1 = class_max;
            s32 cnt2 = counter + 1;
            s1 += 68;
            if (lv1 >= cnt2) {
                counter = cnt2;
                continue;
            }
        }
        *(u16 *)((u8 *)&counter + 2) = s2;
        if (exit2 < 3) { debug_log("sprite_slot_disp: loop break (lv1 <= cnt2)"); exit2++; }
        break;  /* Exit loop when lv1 <= cnt2 */
    } while (1);  /* Loop until all entries processed */
}

void func_8004863C(void) {
    u8 *v1 = screen_w;
    s32 v0 = 0;
    do {
        if ((v0 & 0xF) < 5) {
            *(s32 *)(v1 + 4648) = 0;
        } else {
            *(s32 *)(v1 + 4648) = -1;
        }
        if (((v0 + 1) & 0xF) < 5) {
            *(s32 *)(v1 + 4652) = 0;
        } else {
            *(s32 *)(v1 + 4652) = -1;
        }
        if (((v0 + 2) & 0xF) < 5) {
            *(s32 *)(v1 + 4656) = 0;
        } else {
            *(s32 *)(v1 + 4656) = -1;
        }
        if (((v0 + 3) & 0xF) < 5) {
            *(s32 *)(v1 + 4660) = 0;
        } else {
            *(s32 *)(v1 + 4660) = -1;
        }
        v0 += 4;
        v1 += 16;
    } while (v0 != 64);
}
