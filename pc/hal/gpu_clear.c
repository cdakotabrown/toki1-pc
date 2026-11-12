/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Preserve ClearImage's checkRECT -> enqueue -> _clr -> _cwc order.
 * The unchanged SDK builder writes the original 48-byte packet storage.
 * Only its two known node layouts are accepted; no raw 24-bit link is
 * followed as a native pointer. Pixels are eager; decoded command work has
 * a separate busy deadline. Raw DMA/MMIO and custom raw PS1 GPU_printf
 * addresses remain separate work.
 */
#include "hal.h"
#include "gpu_clear.h"
#include "gpu_queue.h"
#include "bios_printf.h"
#include <stdio.h>
#include <string.h>

extern u32 libgpu_clr_packet_buf[12], libgpu_clr_restore_packet[];
extern u8 libgpu_clearimage_name_str;
extern u8 graph_env[128];
extern void hal_cpu_spend(u32 ticks);
extern void gpu_sdk_check_rect(s32 label, void *rect);
extern s32 gpu_sdk_clear_image(void *rect, u32 color);
extern const char libgpu_badrect_fmt[], libgpu_badrect_rect_fmt[], libgpu_rectdump_name_fmt[], libgpu_rectdump_rect_fmt[];

/* checkRECT passes a label as an s32 PS1 word, not a native vararg
 * pointer. Decode its four known call shapes before invoking BIOS printf. */
static s32 clear_diagnostic(const char *format, ...)
{
    va_list args;
    s32 a, b, c, d;
    s32 result;
    va_start(args, format);
    if (format == libgpu_badrect_fmt || format == libgpu_rectdump_name_fmt) {
        a = va_arg(args, int);
        result = func_800B4850(format, (const char *)(unsigned long)(u32)a);
    } else if (format == libgpu_badrect_rect_fmt || format == libgpu_rectdump_rect_fmt) {
        a = va_arg(args, int); b = va_arg(args, int);
        c = va_arg(args, int); d = va_arg(args, int);
        result = func_800B4850(format, a, b, c, d);
    } else {
        fprintf(stderr, "[GPU] unsupported clear diagnostic format\n");
        result = -1;
    }
    va_end(args);
    return result;
}
s32 (*gpu_sdk_gpu_printf)(const char *, ...) = clear_diagnostic;

void gpu_clear_submit(u32 *packet)
{
    u32 count;
    if (packet != libgpu_clr_packet_buf) goto invalid;
    count = packet[0] >> 24;
    if (count == 4) {
        if ((packet[0] & 0xffffffu) != 0xffffffu) goto invalid;
    } else if (count == 7) {
        if ((packet[0] & 0xffffffu) !=
            ((u32)(unsigned long)libgpu_clr_restore_packet & 0xffffffu) ||
            packet[8] != 0x03ffffffu) goto invalid;
    } else goto invalid;
    if (!gpu_execute_clear_words(packet + 1, count)) goto invalid;
    if (count == 7 && !gpu_execute_clear_words(packet + 9, 3))
        goto invalid;
    return;
invalid:
    fprintf(stderr, "[GPU] rejected malformed SDK clear packet\n");
}

void gpu_clear_image(void *rect, u32 color)
{
    u32 ticket, activity;
    gpu_sdk_check_rect((s32)(unsigned long)&libgpu_clearimage_name_str, rect);
    /* Warm ClearImage/_addque2 entry reaches the alarm read near tick
     * 205 and SetIntrMask(0) near 245. Only this unmasked CPU prefix is
     * modeled here. Packet-building, DMA and masked IRQ work stay separate.
     * Enter the submission after these interruptible stages: a reset or
     * nested SDK call may have changed the current generation meanwhile. */
    hal_cpu_spend(205u);
    gpu_queue_alarm_set();
    hal_cpu_spend(40u);
    /* _addque2's queue-disabled path fences before calling the worker. */
    if (!graph_env[1]) {
        activity = 1;
        memcpy(graph_env + 8, &activity, 4);
        (void)gpu_queue_sync(0);
    }
    ticket = gpu_queue_begin();
    gpu_sdk_clear_image(rect, color);
    gpu_queue_end(ticket);
}
