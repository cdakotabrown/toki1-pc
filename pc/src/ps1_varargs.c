/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ps1_varargs.c -- the variadic arguments of the in-image sprintf on the PC
 * build (src/main/matched/sprintf.c, func_800B48D0; the edit is
 * pc/tools/varargs_plan.json).
 *
 * The console's sprintf is K&R: `sprintf(dst, fmt, va_alist) s32 va_alist;
 * ap = &va_alist;` and reads every argument as the next 32-bit word above
 * the third one -- on the console the caller's $a2/$a3 homes and its
 * outgoing stack words are contiguous.  On x86-64 the arguments travel in
 * registers and `&va_alist + 1` is whatever the callee's frame holds there,
 * so a second argument (or a pointer's upper half) was garbage: the port
 * faulted in msg_str_disp's `%F` (sprintf(tmpBuf, fmt, p_name, pp_name),
 * the player's full name, SHOUGATU's first meeting).  The PC build gives
 * the function an ANSI variadic prototype and collects the arguments
 * through stdarg, full width, into an array the matched body walks.
 */
#include <stdint.h>

/* How many argument words FMT reads, by the console sprintf's own parse:
 * flags "-+ #0", a width (digits or '*': one word), a precision ('.' then
 * digits or '*': one word), length letters h/l/L, then one word for
 * d i u o x X p c s n; "%%" reads none, and any other letter ends the
 * format (the console goes to `done`). */
int ps1_va_count(const char *fmt)
{
    int n = 0;
    const char *p;

    for (p = fmt; *p; p++) {
        if (*p != '%')
            continue;
        p++;
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')
            p++;
        if (*p == '*') {
            n++;
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                p++;
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                n++;
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    p++;
            }
        }
        while (*p == 'h' || *p == 'l' || *p == 'L')
            p++;
        switch (*p) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
        case 'p': case 'c': case 's': case 'n':
            n++;
            break;
        case '%':
            break;
        default:
            return n;
        }
    }
    return n;
}

/* A pointer argument (%s, %n): a host pointer passes through; a PS1
 * address that reached the call as a sign-extended 32-bit value (an s32
 * or a K&R int holding 0x80xxxxxx) is the console's 32-bit address,
 * zero-extended (the PS1 RAM is mapped at 0x80000000). */
void *ps1_va_ptr(long long v)
{
    /* long long: a pointer's width on Linux and Windows alike (Windows'
     * long is 32 bits: the word that held a pointer argument was cut) */
    if (((unsigned long long)v >> 32) == 0xFFFFFFFFULL)
        return (void *)(uintptr_t)(uint32_t)v;
    return (void *)(uintptr_t)v;
}
