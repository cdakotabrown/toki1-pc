/* SPDX-License-Identifier: GPL-3.0-or-later */
/* BIOS printf subset: literal bytes and d/u/x/X/c/s/%%, with numeric
 * zero padding or ordinary right-aligned widths. Return the number of
 * formatted bytes, excluding literals, as the reference BIOS does.
 *
 * CPU costs are a measured approximation of SCPH-5500's CD-booted path,
 * derived from authored diagnostic inputs rather than a game boot target.
 * Interrupt occupancy, cache variation and byte-level TTY interleaving are
 * not modeled. Unsupported formats/ranges fail without partial output.
 */
#include "bios_printf.h"
#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRINT_LIMIT 32768u
#define PRINT_ENTRY_TICKS 2183u
#define LITERAL_TICKS 1294u
#define NEWLINE_TICKS 491u
#define CONVERSION_TICKS 3852u
#define NUMBER_DIGIT_TICKS 1467u
#define WIDTH_DIGIT_TICKS 1042u
#define PADDING_TICKS 1100u

extern void hal_cpu_spend(u32 ticks);

typedef struct {
    char *bytes;
    u32 length;
    u32 ticks;
    s32 formatted;
} PrintRequest;

static int get_byte(const char *source, u32 offset, u8 *value)
{
    if (offset >= PRINT_LIMIT || !pc_ptr_is_mapped(source + offset, 1))
        return 0;
    *value = (u8)source[offset];
    return 1;
}

static int append_byte(PrintRequest *request, u8 value)
{
    if (request->length >= PRINT_LIMIT) return 0;
    request->bytes[request->length++] = (char)value;
    return 1;
}

static s32 reject(PrintRequest *request, const char *reason)
{
    free(request->bytes);
    fprintf(stderr, "[BIOS printf] rejected: %s\n", reason);
    return -1;
}

s32 hal_bios_vprintf(const char *format, va_list args)
{
    PrintRequest request;
    const char *text;
    const char *alphabet;
    char number[33];
    u32 cursor, width, width_digits, digits, raw, magnitude, base;
    u32 count, padding, i, zero, negative;
    u8 code, byte;
    int numeric, written;

    request.bytes = (char *)malloc(PRINT_LIMIT);
    request.length = 0;
    request.ticks = PRINT_ENTRY_TICKS;
    request.formatted = 0;
    if (!request.bytes) return -1;
    format = (const char *)hal_ps1_ptr(format);
    if (!format) return reject(&request, "null format");
    cursor = 0;
    for (;;) {
        if (!get_byte(format, cursor++, &code))
            return reject(&request, "unmapped or unterminated format");
        if (!code) break;
        if (code != '%') {
            if (!append_byte(&request, code))
                return reject(&request, "output limit");
            request.ticks += LITERAL_TICKS;
            if (code == '\n') request.ticks += NEWLINE_TICKS;
            continue;
        }
        width = width_digits = zero = 0;
        if (!get_byte(format, cursor++, &code))
            return reject(&request, "unterminated conversion");
        if (code == '0') zero = 1;
        while (code >= '0' && code <= '9') {
            width = width * 10u + code - '0';
            width_digits++;
            if (width > PRINT_LIMIT)
                return reject(&request, "field width limit");
            if (!get_byte(format, cursor++, &code))
                return reject(&request, "unterminated width");
        }
        numeric = code == 'd' || code == 'u' || code == 'x' || code == 'X';
        if (zero && !numeric)
            return reject(&request, "zero padding of nonnumeric value");
        negative = 0;
        if (numeric) {
            raw = code == 'd' ? (u32)va_arg(args, int) :
                               va_arg(args, unsigned int);
            negative = code == 'd' && (raw & 0x80000000u);
            magnitude = negative ? 0u - raw : raw;
            base = code == 'x' || code == 'X' ? 16u : 10u;
            alphabet = code == 'X' ? "0123456789ABCDEF" :
                                      "0123456789abcdef";
            digits = 0;
            do {
                number[digits++] = alphabet[magnitude % base];
                magnitude /= base;
            } while (magnitude);
            count = digits + negative;
            request.ticks += (base == 16 ? CONVERSION_TICKS :
                              (code == 'd' ? 3621u : 3654u));
            request.ticks += digits * NUMBER_DIGIT_TICKS;
            if (!raw) request.ticks += 66u;
            if (negative) request.ticks += 1272u;
        } else if (code == 's') {
            text = (const char *)hal_ps1_ptr(va_arg(args, const char *));
            if (!text) return reject(&request, "null string");
            count = 0;
            for (;;) {
                if (!get_byte(text, count, &byte))
                    return reject(&request, "unmapped or unterminated string");
                if (!byte) break;
                count++;
            }
            request.ticks += CONVERSION_TICKS + count * 1234u;
            if (count) request.ticks += 33u;
        } else if (code == 'c' || code == '%') {
            byte = code == 'c' ? (u8)va_arg(args, int) : '%';
            count = 1;
            request.ticks += code == 'c' ? 4282u : 2465u;
        } else {
            return reject(&request, "unsupported conversion");
        }
        padding = width > count ? width - count : 0;
        request.ticks += width_digits * WIDTH_DIGIT_TICKS;
        request.ticks += padding * PADDING_TICKS;
        if (negative && zero && !append_byte(&request, '-'))
            return reject(&request, "output limit");
        for (i = 0; i < padding; i++)
            if (!append_byte(&request, zero ? '0' : ' '))
                return reject(&request, "output limit");
        if (numeric) {
            if (negative && !zero && !append_byte(&request, '-'))
                return reject(&request, "output limit");
            while (digits)
                if (!append_byte(&request, (u8)number[--digits]))
                    return reject(&request, "output limit");
        } else if (code == 's') {
            for (i = 0; i < count; i++) {
                if (!append_byte(&request, (u8)text[i]))
                    return reject(&request, "output limit");
                if (text[i] == '\n') request.ticks += NEWLINE_TICKS;
            }
        } else {
            if (!append_byte(&request, byte))
                return reject(&request, "output limit");
            if (byte == '\n') request.ticks += NEWLINE_TICKS;
        }
        request.formatted += (s32)(count + padding);
    }
    written = fwrite(request.bytes, 1, request.length, stdout) ==
              request.length;
    free(request.bytes);
    /* No output or allocation lock spans callbacks on the shared clock. */
    hal_cpu_spend(request.ticks);
    return written ? request.formatted : -1;
}
