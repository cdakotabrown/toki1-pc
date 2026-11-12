/* search_tpage -- 4bpp cel texture-page cache (PC-port override of
 * src/main/matched/search_tpage.c, retail 0x80048B68).
 *
 * The algorithm and both tables are the retail ones; the body is shared
 * with search_tpage8 in pc/include/game/tpage_cache.h.
 *
 * @DivergesFrom: none in behaviour.  The matched C reads the slot's key
 * table through `s32 *keys` in a struct; on a 64-bit host that field sits
 * at +56 and is 8 bytes wide, so the matched body read the next slot field
 * as a pointer and faulted (scene 54, once sprite_slot_disp stopped
 * skipping slots whose data lives outside 0x800C0000-0x800E0000).
 */
#include "types.h"
#include "tpage_cache.h"

s32 search_tpage(s32 arg0, s32 arg1, u8 arg2) {
    return tpage_cache_search((s16)arg0, 6, arg1, arg2);
}
