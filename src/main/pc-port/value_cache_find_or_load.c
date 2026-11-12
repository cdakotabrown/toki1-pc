/*
 * value_cache_find_or_load -- two-entry cache of the decoded background
 * pictures (PC override of src/main/matched/value_cache_find_or_load.c).
 *
 * Statement for statement the matched function.  The divergence is the
 * CALL: the matched C calls cd_read_start(10, 0x80162000) with two
 * arguments, but cd_read_start (0x80046610) takes three and forwards the
 * third to cd_command_issue(6, len, ...) as the transfer length.  Retail
 * passes the key there: the function's second instruction is
 * `move $a2,$a0` and nothing rewrites $a2 before either `jal 0x80046610`
 * (0x800577E0 / 0x80057848), so the read is cd_read_start(10, 0x80162000,
 * val) -- the table value (weekly_bg_sector_tbl[], step34_load_tbl0[],
 * ...) is both the cache key and the byte count of the background's
 * bitstream.  IDO happened to keep val in $a2 for the two-argument call;
 * on the host the third argument register holds whatever the caller left
 * there, so the queued read had a garbage length.  Measured in scene 64
 * (SCENE_NAV=64): the read never completed (week_day_init0 waited in
 * step_mode 1 for 1,200 frames), while retail decodes the picture to
 * (320,416,320,88) and reaches now_sub_step 1.
 *
 * @DivergesFrom: matched value_cache_find_or_load -- explicit third
 * cd_read_start argument (the value retail leaves in $a2).
 */
#include "common.h"

extern void cd_read_start(s32 a0, s32 a1, s32 a2);

extern s32 date_effect_init_tbl0;
extern s32 date_effect_init_tbl1;
extern u8 bg_change_tbl1;
extern s32 date2_intro_script_tbl0;
extern s32 date2_intro_script_tbl1;
extern u8 bg_change_tbl0;

s32 value_cache_find_or_load(s32 arg0, s32 arg1) {
    s32 val;
    s32 idx;

    val = arg0;

    if (arg1 == 1) {
        if (arg0 == date_effect_init_tbl0) {
            return 0;
        }
        if (val == date_effect_init_tbl1) {
            return 1;
        }
        cd_read_start(10, 0x80162000, val);
        /* (&tbl1)[-idx]: idx 0 -> tbl1, idx 1 -> tbl0 */
        idx = bg_change_tbl1;
        (&date_effect_init_tbl1)[-idx] = val;
        return -1;
    } else {
        if (val == date2_intro_script_tbl0) {
            return 0;
        }
        if (val == date2_intro_script_tbl1) {
            return 1;
        }
        cd_read_start(10, 0x80162000, val);
        idx = bg_change_tbl0;
        (&date2_intro_script_tbl1)[-idx] = val;
        return -1;
    }
}
