/* search_tpage8 -- 8bpp cel texture-page cache (PC-port override of
 * src/main/matched/search_tpage8.c, retail 0x80048DD0).
 *
 * The algorithm and both tables are the retail ones; the body is shared
 * with search_tpage in pc/include/game/tpage_cache.h, which documents the
 * single host adaptation (the slot's 32-bit key-table handle).
 *
 * 2026-10-30: this override formerly kept private static copies of the
 * state/key tables (written when screen_w was not yet a 16 KiB block at its
 * PS1 address).  Those copies never saw tpage_buf_clear, the page locks or
 * the per-frame reset, so a miss allocated page 0 (the framebuffer) instead
 * of the first free page, and pages were never released.
 */
#include "types.h"
#include "tpage_cache.h"

s32 search_tpage8(s32 arg0, s32 arg1, u8 arg2) {
    /* Retail entry narrows the first argument to s16 before the body. */
    return tpage_cache_search((s16)arg0, 8, arg1, arg2);
}
