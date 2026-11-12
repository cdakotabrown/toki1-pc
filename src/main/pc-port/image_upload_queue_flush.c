/* Native adapter for matched/image_upload_queue_flush.c (0x80043C90).
 * @DivergesFrom: src/main/matched/image_upload_queue_flush.c -- storage
 * extent only. Retail appenders cap this queue at 32 sixteen-byte entries.
 * The matched C's one-entry tail is an IDO representation, not the real
 * allocation; GCC -O2 otherwise removes the loop after its first iteration.
 * Call order, coordinate conversion, width shift and count reset follow
 * the retail body. The PS1 matching source remains unchanged.
 */
#include "image_upload_queue.h"

extern u8 screen_w[];
extern u8 image_queue_n;
extern void load_image_rect(s32, s32, s16, s16, s32);

void image_upload_queue_flush(s32 mode)
{
    PcImageUploadQueue *base;
    u32 i;
    u16 x, y, w;

    (void)mode;
    base = (PcImageUploadQueue *)screen_w;
    i = 0;
    if (image_queue_n != 0) {
        do {
            if (base->entries[i].flags == 0) {
                load_image_rect(base->entries[i].x, base->entries[i].y,
                                base->entries[i].w, base->entries[i].h,
                                (s32)base->entries[i].source);
            } else {
                x = base->entries[i].x +
                    ((base->entries[i].flags & 15) << 6);
                y = base->entries[i].y +
                    ((base->entries[i].flags & 16) << 4);
                w = (u32)base->entries[i].w >> base->entries[i].shift;
                load_image_rect(x, y, w, base->entries[i].h,
                                (s32)base->entries[i].source);
            }
            i++;
        } while (i < base->count);
    }
    image_queue_n = 0;
}
