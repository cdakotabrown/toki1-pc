/*
 * cdq_command_process -- CD command-queue pump (PC override of
 * src/main/matched/cdq_command_process.c).
 *
 * Statement for statement the matched function; the ONE divergence is
 * addressing (src/main/pc-port/mv_state.h doctrine -- the pointer width is
 * a property of the host, not of the game):
 *
 *   cdq_cur_addr is an `s32` holding a PS1 address (0x8019xxxx by the case-6
 *   arithmetic).  The footer check's `*(u32 *)cdq_cur_addr` sign-extends it
 *   to 0xFFFFFFFF8019A7FC on the host and SIGSEGV (measured:
 *   cdq_command_process+0x708, crash-looping after every completed load).
 *   Here the handle is declared u32 and zero-extended at the deref -- the
 *   address the console's `lw` pairing produces.
 *
 * Like the matched TU, the Sony libc names resolve to the BIOS thunks
 * (sony_bios_alias.h): retail's " CDQ.EXE %x\n" is a kernel A(0x3F) printf
 * that delays every queued read (measured, regtest host Interpreter:
 * 21,331 ticks from CdSync's return to CdControlB(Setmode) in the frame that
 * issues a read, nearly all of it this printf; the BIOS printf model in
 * pc/hal/bios_printf.c predicts ~21,000).  pc-port overrides are compiled without
 * the force-included alias header, so this file used to call the host's
 * libc printf, which takes no PS1 time.
 *
 * The matched header follows.
 *
 * cdq_command_process -- CD command-queue pump (" CDQ.EXE").
 * Address: 0x800457FC
 * Size: 2852 bytes (713 instructions)
 * Region: Game (IDO 5.2), -Wc,-pic0 (retail frame is 80; pic1 gives 96)
 *
 * A 48-slot circular queue of 24-byte records at cdq_que, head/tail/
 * flags/phase at cdq_head, status block at cdq_result.  phase -1 = issue
 * the head command (table 0x800B6030), phase 1 = wait for its completion
 * (table 0x800B60A0); an empty queue polls for a disk error instead.
 *
 * Status: BYTE-PERFECT candidate 713/713, Body 713 == Target 713, Fixup 0
 *   (2026-09-19), natural re-derivation under -cckr, no hooks, no paints.
 *   The old "one word short" (the case-27 completion `b; nop`) and the
 *   "case-tail transpose" walls were source-order bugs:
 * - phase is promoted to a register ($v1) and stored back at each exit, so
 *   the li sits where the ASSIGNMENT is: `phase = -1;` comes BEFORE
 *   `head++` in the issue arms and the footer-OK arms, AFTER the head wrap
 *   in the footer-error / seek-back arms, before the wrap in the idle arm.
 * - The completion case 6 is `if (ret == 0) {...} else if (ret < 0) {...}`
 *   (not a goto); its join is what leaves the case-27 `b` unfilled.
 * - The " CDQ.EXE %x\n" printf takes head and top (a1/a2 at the jal; top
 *   is spilled in the delay slot).
 * - Callees undeclared (implicit int): cd_mark_disp returning int forbids
 *   v0 for the phase web; printf implicit int puts cdq_command_process_tbl0 in v1.
 * - `phase = 1; break;` on one line in issue case 27 (as1 line tie).
 * - cdq_head / cdq_result are DEFINED here (same-TU data: as1 shares one
 *   `lui $at` across the paired stores).
 * - Frame: unused scalars `pad` (first) and `mask` give retail's slots
 *   (top 72, lo24 68, buffers 56/48, i 44); the dead `w = lo24;` keeps
 *   lo24 materialised before the calls (sw t7,68(sp)).
 * @SymbolStart: 0x800457FC
 * @SymbolSize: 2852
 */
/* @CompileFlags: -Wc,-pic0 -cckr */
/* @PostProc: strip_leading_instructions --count auto */
#include "types.h"
#ifdef PC_PORT
#include "sony_bios_alias.h"   /* printf -> the BIOS A(0x3F) trampoline */
#endif

typedef struct {
    u8 head;            /* +0 queue head index (0..47) */
    u8 tail;            /* +1 queue tail index */
    u8 flags;           /* +2 completion flags */
    s8 phase;           /* +3 -1 issue, 1 wait-complete */
} CdqCtl;
CdqCtl cdq_head = { 0 };

typedef struct {
    u8  cmd;            /* +0 */
    u8  pad0[3];
    s32 f4;             /* +4 */
    u32 f8;             /* +8 */
    u8  pad1[11];
    u8  f23;            /* +23 */
} QueueEntry;           /* stride 24 */
extern QueueEntry cdq_que[];

typedef struct {
    s32 res;            /* +0 */
    u8  status;         /* +4 */
    u8  count;          /* +5 */
} CdqStat;
CdqStat cdq_result = { 0 };

extern u8  cd_mode;
extern u32 cdq_cur_addr;   /* PS1-width pointer handle (see header) */
extern u8  cd_result_buf;
extern u8  cdq_stat_buf;

extern u8  cdq_footer_chk;
extern u16 cdq_command_process_tbl0;
extern s32 cd_read_sync_poll_tbl0;

void cdq_command_process(void)
{
    s32 pad;
    u32 top;
    u32 lo24;
    u32 mask;
    u8 buf56[8];
    u8 buf48[8];
    s32 i;
    s32 ret;
    u32 w;

    if (cdq_head.head != cdq_head.tail) {
        cd_mark_disp();
        if (cdq_head.phase == -1) {
            switch (cdq_que[cdq_head.head].cmd) {
            case 0:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_result.status = 0;
                if (CdControl(9, 0, &cdq_stat_buf) == 1) {
                    cdq_head.phase = 1;
                }
                break;
            case 3:
                SsSetMVol(127, 127);
                cdq_head.flags = 0;
                cdq_result.status = 0;
                if (CdControl(3, 0, 0) == 1) {
                    cdq_head.phase = -1;
                    cdq_head.head++;
                }
                break;
            case 13:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_result.status = 0;
                if (CdControl(13, &cdq_que[cdq_head.head].f8, &cdq_stat_buf) == 1) {
                    cdq_head.phase = -1;
                    cdq_head.head++;
                }
                break;
            case 9:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_result.status = 0;
                if (CdControl(9, 0, &cdq_stat_buf) == 1) {
                    cdq_head.phase = 1;
                }
                break;
            case 14:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_result.status = 0;
                if (CdControl(14, &cdq_que[cdq_head.head].f8, &cdq_stat_buf) == 1) {
                    cdq_head.phase = -1;
                    cdq_head.head++;
                }
                break;
            case 2:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_result.status = 0;
                CdIntToPos(cdq_que[cdq_head.head].f4, buf56);
                if (CdControl(2, buf56, &cdq_stat_buf) == 1) {
                    cdq_head.phase = -1;
                    cdq_head.head++;
                }
                break;
            case 21:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_result.status = 0;
                CdIntToPos(cdq_que[cdq_head.head].f4, buf56);
                if (CdControlF(21, buf56) == 1) {
                    cdq_head.phase = 1;
                }
                break;
            case 27:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_result.status = 0;
                CdReadyCallback(0);
                CdDataCallback(0);
                StUnSetRing();
                CdIntToPos(cdq_que[cdq_head.head].f4, buf56);
                while (CdControlB(2, buf56, 0) == 0) {
                }
                while (CdRead2(200) == 0) {
                }
                cdq_result.status = 0;
                cdq_head.phase = 1; break;
            case 10:
                cdq_head.flags = 0;
                cdq_result.status = 0;
                while (CdControl(10, 0, 0) == 0) {
                }
                cdq_head.head++;
                cdq_head.phase = -1;
                break;
            case 6:
                if (CdSync(1, buf48) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_result.status = 0;
                w = cdq_que[cdq_head.head].f8;
                lo24 = (w & 0xFFFFFF) + 0x80000000;
                top = w >> 24;
                cdq_cur_addr = (w & 0xFFFFFF) + (top << 11) + 0x7FFFFFFC;
                w = lo24;
                printf(" CDQ.EXE %x\n", cdq_head.head, top);
                CdReadyCallback(0);
                CdDataCallback(0);
                cd_mode = 132;
                while (CdControlB(14, &cd_mode, 0) == 0) {
                }
                CdIntToPos(cdq_que[cdq_head.head].f4, buf56);
                while (CdControlB(2, buf56, 0) == 0) {
                }
                StUnSetRing();
                while (CdRead(top, lo24, 128) == 0) {
                }
                cdq_head.phase = 1;
                break;
            }
        } else if (cdq_head.phase == 1) {
            switch (cdq_que[cdq_head.head].cmd) {
            case 0:
                if (CdSync(1, &cdq_stat_buf) == 0) {
                    break;
                }
                if (CdControl(1, 0, &cdq_stat_buf) != 1) {
                    break;
                }
                if (cdq_stat_buf & 0x40) {
                    break;
                }
                cdq_result.status = 2;
                cdq_head.head++;
                cdq_head.phase = -1;
                break;
            case 9:
                if (CdSync(1, &cdq_stat_buf) == 0) {
                    break;
                }
                if (CdControl(1, 0, &cdq_stat_buf) != 1) {
                    break;
                }
                if (cdq_stat_buf & 0x40) {
                    break;
                }
                cdq_result.status = 2;
                cdq_head.head++;
                cdq_head.phase = -1;
                break;
            case 21:
                if (CdControl(1, 0, &cdq_stat_buf) != 1) {
                    break;
                }
                if (cdq_stat_buf & 0x40) {
                    break;
                }
                cdq_result.status = 2;
                cdq_head.head++;
                cdq_head.phase = -1;
                break;
            case 6:
                if (CdSync(1, &cdq_stat_buf) == 0) {
                    break;
                }
                ret = CdReadSync(1, &cd_result_buf);
                cd_read_sync_poll_tbl0 = ret;
                if (ret > 0) {
                    break;
                }
                if (ret == 0) {
                    if (cdq_footer_chk != 0) {
                        if ((*(u32 *)(unsigned long)cdq_cur_addr & 0xFFFF0000) != 0x10120000) {
                            cdq_head.flags = 0;
                            printf("FOOTER ERROR");
                            cdq_command_process_tbl0++;
                            if ((cdq_command_process_tbl0 & 3) == 3) {
                                printf("RESET!");
                                CdControl(10, 0, 0);
                                for (i = 0; i != 512; i++) {
                                    VSync(0);
                                }
                            }
                            if (cdq_head.head != 0) {
                                cdq_head.head--;
                            } else {
                                cdq_head.head = 47;
                            }
                            cdq_head.phase = -1;
                        } else {
                            cdq_head.phase = -1;
                            cdq_head.head++;
                            cdq_result.status = 2;
                            cdq_head.flags |= 1;
                            cdq_result.count++;
                        }
                    } else {
                        cdq_head.phase = -1;
                        cdq_head.head++;
                        cdq_result.status = 2;
                        cdq_head.flags |= 1;
                        cdq_result.count++;
                    }
                } else if (ret < 0) {
                    if (cdq_head.head != 0) {
                        cdq_head.head--;
                    } else {
                        cdq_head.head = 47;
                    }
                    cdq_result.status = 0;
                    cdq_head.flags = 0;
                    cdq_head.phase = -1;
                }
                break;
            case 27:
                if (CdSync(1, &cdq_stat_buf) == 5) {
                    printf("SEEK_ERROR RETRY");
                    cdq_head.phase = -1;
                    cd_mode_set();
                    break;
                }
                while (CdControl(1, 0, &cdq_stat_buf) == 0) {
                }
                if ((cdq_stat_buf & 0x20) == 0) {
                    break;
                }
                cdq_head.flags = 0;
                cdq_head.phase = -1;
                if (cdq_que[cdq_head.head].f23 != 255) {
                    cdq_result.status = 4;
                }
                cdq_head.head++;
                break;
            }
        }
    } else if (CdSync(1, 0) == 5) {
        if (get_CD_l_com() == 27) {
            printf(" DISK ERROR XA STOP ");
            cdq_head.phase = -1;
            if (cdq_head.head != 0) {
                cdq_head.head--;
            } else {
                cdq_head.head = 47;
            }
            cd_mode_set();
        } else {
            printf("NOP ");
            CdControl(1, 0, 0);
        }
    } else if (cdq_result.status == 4) {
        cdq_result.res = check_head_position();
    }

    if (cdq_head.head >= 48) {
        cdq_head.head = 0;
    }
}
