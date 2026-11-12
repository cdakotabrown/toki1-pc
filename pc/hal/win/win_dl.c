/* SPDX-License-Identifier: GPL-3.0-or-later */
/* win_dl.c -- dladdr / dlsym / backtrace for the Windows build, from the
 * executable's own COFF symbol table (pc/include/win/dlfcn.h, execinfo.h).
 *
 * The Linux build links -rdynamic and asks glibc.  What the port relies on:
 *   - pc/hal/cpu_cost.c keys the retail CPU-time table on the name of the
 *     function a __fentry__ call came from, and treats a function dladdr
 *     cannot name (a static one: not in the dynamic symbol table) as having
 *     no retail row.  The timing model -- and so every trace -- depends on
 *     exactly which functions are named.
 *   - the crash reporter and the [FOLD] diagnostics print function+offset.
 *   - trace.c / the debug toolkit look globals up by name (dlsym).
 *
 * Model: every symbol the linker kept in the PE file's COFF table (MinGW ld
 * keeps it unless the image is stripped -- the Makefile never strips).
 *   - code symbols (in an executable section, external or static), sorted:
 *     an address belongs to the last one at or below it, as long as it lies
 *     inside the executable section;  dladdr names it only if that symbol is
 *     EXTERNAL, the glibc-over-dynsym answer for a static function being
 *     "no name".  (ELF sizes are not in COFF; the next symbol bounds each
 *     function, which is the same thing for compiled code.)
 *   - dlsym: any external symbol, in a section or absolute.
 * Test: pc/tools/test_windows_build.py (dladdr of a static vs a global).
 */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <pthread.h>
#include "dlfcn.h"
#include "execinfo.h"

#pragma pack(push, 1)
typedef struct {
    union {
        char short_name[8];
        struct { unsigned int zeros, offset; } l;
    } n;
    unsigned int value;
    short section;
    unsigned short type;
    unsigned char storage_class;
    unsigned char aux;
} CoffSym;
#pragma pack(pop)

#define COFF_CLASS_EXTERNAL 2
#define COFF_CLASS_STATIC   3

typedef struct {
    uintptr_t addr;
    const char *name;
    int global;
} WinSym;

static WinSym *g_code;      /* code symbols, by address */
static int g_ncode;
static WinSym *g_glob;      /* external symbols, by name */
static int g_nglob;
static char *g_strtab;      /* the file's string table (names point into it) */
static char g_exe_path[MAX_PATH * 3];
static uintptr_t g_base, g_text_lo, g_text_hi;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static int by_addr(const void *a, const void *b)
{
    const WinSym *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    /* at one address prefer the global name (an alias of a static), then
     * the name order, so the choice never depends on qsort */
    if (x->global != y->global) return y->global - x->global;
    return strcmp(x->name, y->name);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const WinSym *)a)->name, ((const WinSym *)b)->name);
}

static void load(void)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    unsigned char *file = NULL;
    long size;
    FILE *f;
    unsigned i, nsec, nsym;
    CoffSym *syms;
    unsigned strsize;
    wchar_t wpath[MAX_PATH];
    char *names;
    int n;

    g_base = (uintptr_t)GetModuleHandleW(NULL);
    n = (int)GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (n <= 0 || n >= MAX_PATH) return;
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, g_exe_path, sizeof g_exe_path, NULL, NULL);
    f = _wfopen(wpath, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) file = malloc((size_t)size);
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(file);
        return;
    }
    fclose(f);

    dos = (IMAGE_DOS_HEADER *)file;
    if (size < (long)sizeof *dos || dos->e_magic != IMAGE_DOS_SIGNATURE) goto out;
    nt = (IMAGE_NT_HEADERS64 *)(file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) goto out;
    nsec = nt->FileHeader.NumberOfSections;
    sec = (IMAGE_SECTION_HEADER *)((unsigned char *)&nt->OptionalHeader +
                                   nt->FileHeader.SizeOfOptionalHeader);
    nsym = nt->FileHeader.NumberOfSymbols;
    if (!nt->FileHeader.PointerToSymbolTable || !nsym) goto out;   /* stripped */
    syms = (CoffSym *)(file + nt->FileHeader.PointerToSymbolTable);
    memcpy(&strsize, (unsigned char *)(syms + nsym), 4);
    g_strtab = malloc(strsize + 1);
    memcpy(g_strtab, (unsigned char *)(syms + nsym), strsize);
    g_strtab[strsize] = 0;
    /* short names (<= 8 chars, not NUL-terminated) get their own copies */
    names = malloc((size_t)nsym * 9);

    g_code = malloc(sizeof *g_code * nsym);
    g_glob = malloc(sizeof *g_glob * nsym);
    for (i = 0; i < nsec; i++) {
        if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE) {
            uintptr_t lo = g_base + sec[i].VirtualAddress;
            uintptr_t hi = lo + sec[i].Misc.VirtualSize;
            if (!g_text_lo || lo < g_text_lo) g_text_lo = lo;
            if (hi > g_text_hi) g_text_hi = hi;
        }
    }
    for (i = 0; i < nsym; i += 1 + syms[i].aux) {
        const CoffSym *s = &syms[i];
        const char *name;
        uintptr_t addr;
        int code;
        if (s->storage_class != COFF_CLASS_EXTERNAL && s->storage_class != COFF_CLASS_STATIC)
            continue;
        if (s->n.l.zeros == 0) {
            if (s->n.l.offset >= strsize) continue;
            name = g_strtab + s->n.l.offset;
        } else {
            char *d = names + (size_t)i * 9;
            memcpy(d, s->n.short_name, 8);
            d[8] = 0;
            name = d;
        }
        if (s->section == IMAGE_SYM_ABSOLUTE) {
            if (s->storage_class == COFF_CLASS_EXTERNAL) {
                g_glob[g_nglob].addr = s->value;
                g_glob[g_nglob].name = name;
                g_glob[g_nglob++].global = 1;
            }
            continue;
        }
        if (s->section <= 0 || (unsigned)s->section > nsec) continue;   /* undefined / debug */
        /* a section-definition symbol (".text", ".data$x"): not a name */
        if (s->storage_class == COFF_CLASS_STATIC && s->aux && name[0] == '.') continue;
        addr = g_base + sec[s->section - 1].VirtualAddress + s->value;
        code = (sec[s->section - 1].Characteristics & IMAGE_SCN_CNT_CODE) != 0;
        if (code) {
            g_code[g_ncode].addr = addr;
            g_code[g_ncode].name = name;
            g_code[g_ncode++].global = s->storage_class == COFF_CLASS_EXTERNAL;
        }
        if (s->storage_class == COFF_CLASS_EXTERNAL) {
            g_glob[g_nglob].addr = addr;
            g_glob[g_nglob].name = name;
            g_glob[g_nglob++].global = 1;
        }
    }
    qsort(g_code, (size_t)g_ncode, sizeof *g_code, by_addr);
    qsort(g_glob, (size_t)g_nglob, sizeof *g_glob, by_name);
out:
    free(file);
}

static void ensure(void) { pthread_once(&g_once, load); }

int dladdr(const void *addr, Dl_info *info)
{
    uintptr_t a = (uintptr_t)addr;
    int lo = 0, hi, best = -1;
    ensure();
    memset(info, 0, sizeof *info);
    if (!g_ncode || a < g_text_lo || a >= g_text_hi) return 0;
    hi = g_ncode - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_code[mid].addr <= a) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    info->dli_fname = g_exe_path;
    info->dli_fbase = (void *)g_base;
    if (best < 0) return 1;
    /* the first entry at that address (by_addr put a global one first) */
    while (best > 0 && g_code[best - 1].addr == g_code[best].addr) best--;
    if (g_code[best].global) {
        info->dli_sname = g_code[best].name;
        info->dli_saddr = (void *)g_code[best].addr;
    }
    return 1;
}

void *dlsym(void *handle, const char *name)
{
    WinSym key, *hit;
    (void)handle;
    ensure();
    if (!g_nglob || !name) return NULL;
    key.name = name;
    hit = bsearch(&key, g_glob, (size_t)g_nglob, sizeof *g_glob, by_name);
    return hit ? (void *)hit->addr : NULL;
}

int backtrace(void **buffer, int size)
{
    if (size <= 0) return 0;
    if (size > 62) size = 62;           /* RtlCaptureStackBackTrace's limit */
    return (int)RtlCaptureStackBackTrace(1, (DWORD)size, buffer, NULL);
}

void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    int i;
    for (i = 0; i < size; i++) {
        Dl_info di;
        char line[512];
        int n;
        if (dladdr(buffer[i], &di) && di.dli_sname)
            n = snprintf(line, sizeof line, "%s(%s+0x%llx) [%p]\n", di.dli_fname, di.dli_sname,
                         (unsigned long long)((char *)buffer[i] - (char *)di.dli_saddr), buffer[i]);
        else
            n = snprintf(line, sizeof line, "%s [%p]\n", g_exe_path, buffer[i]);
        if (n > 0) _write(fd, line, (unsigned)(n < (int)sizeof line ? n : (int)sizeof line - 1));
    }
}
#endif
