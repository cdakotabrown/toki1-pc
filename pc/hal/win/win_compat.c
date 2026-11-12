/* SPDX-License-Identifier: GPL-3.0-or-later */
/* win_compat.c -- the functions behind pc/include/win/pc_win_compat.h. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#undef rename
int pc_win_rename(const char *from, const char *to)
{
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

/* The process's DPI awareness and the window's DPI, for the log
 * (pc/hal/system.c after the window opens).
 * The manifest asks for per-monitor v2, so the
 * window stays sharp at 125-200 % scaling and when it moves to a monitor
 * of another scale; the presenter sizes everything in pixels.  The calls
 * are looked up at run time (Windows 10 1607+), so older systems log
 * "unknown" instead of failing to start. */
typedef void *(WINAPI *GetCtxFn)(void);
typedef int (WINAPI *AwareFn)(void *);
typedef BOOL (WINAPI *AreEqualFn)(void *, void *);
typedef UINT (WINAPI *DpiFn)(HWND);
void pc_win_dpi_report(void *hwnd)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    GetCtxFn get = u ? (GetCtxFn)(void *)GetProcAddress(u, "GetThreadDpiAwarenessContext") : NULL;
    AreEqualFn eq = u ? (AreEqualFn)(void *)GetProcAddress(u, "AreDpiAwarenessContextsEqual") : NULL;
    AwareFn aw = u ? (AwareFn)(void *)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext") : NULL;
    DpiFn dpi = u ? (DpiFn)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
    const char *name = "unknown";
    if (get && eq && aw) {
        void *ctx = get();
        if (eq(ctx, (void *)(intptr_t)-4))     /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
            name = "per-monitor v2";
        else switch (aw(ctx)) {
            case 0: name = "unaware (scaled by Windows: blurry)"; break;
            case 1: name = "system"; break;
            case 2: name = "per-monitor"; break;
        }
    }
    printf("[HAL] DPI awareness: %s; window DPI %u\n", name,
           dpi && hwnd ? (unsigned)dpi((HWND)hwnd) : 0u);
}
