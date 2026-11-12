/* SPDX-License-Identifier: GPL-3.0-or-later */
/* win_dbg_platform.c -- the debug toolkit's host layer on Windows
 * (pc/hal/dbg_platform.h; the Linux one is pc/hal/dbg_platform_posix.c).
 *
 *   symbols    the exe's own COFF symbol table (the exe is never stripped,
 *              and the link keeps its .file records, pc/Makefile): every
 *              data symbol with its section, the source file of a static
 *              (the .file record before it), and a size -- COFF records
 *              none, so an object runs to the next symbol of its section
 *   build id   the CodeView signature `ld --build-id` writes into the PE
 *              debug directory
 *   maps       VirtualQuery, printed as /proc/self/maps lines
 *   memory     VirtualAlloc'd toolkit buffers; files through pc_fs (UTF-8
 *              paths), replaced with MoveFileEx (pc_win_rename)
 *   stack      the toolkit runs on a fiber; the main-thread stack window a
 *              state carries is committed by a stack probe
 *   console    Winsock (TCP, and AF_UNIX where the host has it), stdin
 *              through a reader thread
 *
 * Every static here belongs to the process: pc/hal/dbg_state.c keeps this
 * file out of a state (KEEP_FILES).
 */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "../dbg_platform.h"
#include "../pc_fs.h"

/* ------------------------------------------------------------------ */
/* memory and files                                                    */
/* ------------------------------------------------------------------ */

/* Top-down: the low 2 GB belong to the game (its heap must stay where an
 * s32 holds an address, pc/hal/win/win_heap.c), and Windows allocates
 * bottom-up -- the rewind buffer's hundreds of MB taken from the bottom
 * pushed a later game malloc above 2 GB (found by test_rewind under Wine).
 * As Linux's mmap'd toolkit buffers, these live high. */
void *dbg_mem_alloc(unsigned long n) {
    unsigned long long *p = VirtualAlloc(NULL, (SIZE_T)n + 16,
                                         MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!p) return NULL;
    p[0] = n;
    return p + 2;
}

void dbg_mem_free(void *q) {
    if (q) VirtualFree((unsigned long long *)q - 2, 0, MEM_RELEASE);
}

void *dbg_mem_realloc(void *q, unsigned long n) {
    unsigned long long old = q ? ((unsigned long long *)q)[-2] : 0;
    void *r = dbg_mem_alloc(n);
    if (r && q) memcpy(r, q, (size_t)(old < n ? old : n));
    dbg_mem_free(q);
    return r;
}

unsigned char *dbg_read_file(const char *path, unsigned long *len) {
    FILE *f = pc_fs_fopen(path, "rb");
    unsigned long cap = 1 << 16, n = 0;
    unsigned char *buf;
    *len = 0;
    if (!f) return NULL;
    buf = dbg_mem_alloc(cap);
    while (buf) {
        size_t r;
        if (n + 1 >= cap) { cap *= 2; buf = dbg_mem_realloc(buf, cap); if (!buf) break; }
        r = fread(buf + n, 1, cap - n - 1, f);
        if (r == 0) {
            if (ferror(f)) { dbg_mem_free(buf); buf = NULL; }
            break;
        }
        n += (unsigned long)r;
    }
    fclose(f);
    if (buf) buf[n] = 0;
    *len = n;
    return buf;
}

int dbg_write_file(const char *path, const void *data, unsigned long n) {
    FILE *f = pc_fs_fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, n, f) == n;
    ok = fflush(f) == 0 && ok;
    return fclose(f) == 0 && ok;
}

int dbg_replace_file(const char *from, const char *to) {
    return rename(from, to);     /* pc_win_rename: MoveFileEx REPLACE_EXISTING */
}

void dbg_plat_setenv(const char *name, const char *value) {
    _putenv_s(name, value);
}

/* ------------------------------------------------------------------ */
/* the executable's own symbol table                                   */
/* ------------------------------------------------------------------ */

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
#define COFF_CLASS_FILE     103

static DbgImgSym *g_syms;
static int g_nsyms;
static int g_state;             /* 0 not tried, 1 ok, -1 failed */
static char g_err[200];
static char g_build_id[80];
static unsigned long g_data[8][2];
static int g_ndata;

typedef struct { unsigned long lo, hi; int data; } Sec;

static int ul_cmp(const void *a, const void *b) {
    unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;
    return x < y ? -1 : x > y;
}

static int sym_cmp(const void *a, const void *b) {
    const DbgImgSym *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* Names live in two blocks made once: a copy of the string table (long
 * names point into it) and an arena for the short names and the .file
 * names.  (One VirtualAlloc per name reserved 64 KB of address space each:
 * ~100k names took the low 2 GB the game heap needs, and the boot movie's
 * malloc landed above 2 GB -- found by test_rewind under Wine.) */
static char *g_arena;
static unsigned long g_arena_used, g_arena_cap;

static char *dup_n(const char *s, size_t n) {
    char *d;
    if (!g_arena || g_arena_used + n + 1 > g_arena_cap) return NULL;
    d = g_arena + g_arena_used;
    memcpy(d, s, n);
    d[n] = 0;
    g_arena_used += (unsigned long)n + 1;
    return d;
}

static void load_build_id(void) {
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    IMAGE_DEBUG_DIRECTORY *d = (IMAGE_DEBUG_DIRECTORY *)(base + dd.VirtualAddress);
    unsigned i, k;
    if (!dd.VirtualAddress) return;
    for (i = 0; i < dd.Size / sizeof *d; i++) {
        const unsigned char *cv;
        if (d[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !d[i].AddressOfRawData) continue;
        cv = base + d[i].AddressOfRawData;
        if (memcmp(cv, "RSDS", 4)) continue;
        for (k = 0; k < 16; k++) sprintf(g_build_id + 2 * k, "%02x", cv[4 + k]);
        return;
    }
}

static int load(void) {
    unsigned char *file = NULL, *base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sh;
    Sec sec[64];
    CoffSym *syms;
    unsigned nsec, nsym, strsize, i;
    const char *strtab, *cur_file = NULL;
    wchar_t wpath[MAX_PATH];
    char path[MAX_PATH * 3];
    unsigned long flen = 0, *bounds;
    int n, k, cap, nb = 0;

    if (g_state) return g_state > 0;
    g_state = -1;
    load_build_id();
    base = (unsigned char *)GetModuleHandleW(NULL);
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    nsec = nt->FileHeader.NumberOfSections;
    if (nsec > 64) nsec = 64;
    sh = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nsec; i++) {
        char name[9];
        DWORD ch = sh[i].Characteristics;
        memcpy(name, sh[i].Name, 8);
        name[8] = 0;
        sec[i].lo = (unsigned long)(uintptr_t)(base + sh[i].VirtualAddress);
        sec[i].hi = sec[i].lo + sh[i].Misc.VirtualSize;
        /* the image's own variables: .data and .bss (not .rdata, nor the
         * import table, TLS or CRT tables, which are writable too) */
        sec[i].data = (ch & IMAGE_SCN_MEM_WRITE) && !(ch & IMAGE_SCN_CNT_CODE) &&
                      (!strncmp(name, ".data", 5) || !strncmp(name, ".bss", 4));
        if (sec[i].data && g_ndata < 8) {
            g_data[g_ndata][0] = sec[i].lo;
            g_data[g_ndata][1] = sec[i].hi;
            g_ndata++;
        }
    }

    /* the symbol table is in the file, not the mapped image */
    n = (int)GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (n <= 0 || n >= MAX_PATH) { snprintf(g_err, sizeof g_err, "no module path"); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof path, NULL, NULL);
    file = dbg_read_file(path, &flen);
    if (!file) { snprintf(g_err, sizeof g_err, "cannot read %s", path); return 0; }
    {
        IMAGE_NT_HEADERS64 *fnt = (IMAGE_NT_HEADERS64 *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
        unsigned long ptr = fnt->FileHeader.PointerToSymbolTable;
        nsym = fnt->FileHeader.NumberOfSymbols;
        if (!ptr || !nsym || ptr + (unsigned long)nsym * sizeof(CoffSym) + 4 > flen) {
            snprintf(g_err, sizeof g_err, "the executable has no symbol table (stripped?)");
            dbg_mem_free(file);
            return 0;
        }
        syms = (CoffSym *)(file + ptr);
        memcpy(&strsize, (unsigned char *)(syms + nsym), 4);
        if ((unsigned long)((unsigned char *)(syms + nsym) - file) + strsize > flen) {
            snprintf(g_err, sizeof g_err, "the symbol table is truncated");
            dbg_mem_free(file);
            return 0;
        }
        {
            char *copy = dbg_mem_alloc(strsize + 1);
            if (copy) {
                memcpy(copy, (const char *)(syms + nsym), strsize);
                copy[strsize] = 0;
            }
            strtab = copy;
        }
        g_arena_cap = (unsigned long)nsym * 20 + 64;   /* short names <= 9, file names <= 19 per record */
        g_arena = dbg_mem_alloc(g_arena_cap);
        if (!strtab || !g_arena) {
            dbg_mem_free(file);
            snprintf(g_err, sizeof g_err, "out of memory");
            return 0;
        }
    }
    cap = (int)nsym;
    g_syms = dbg_mem_alloc((unsigned long)cap * sizeof *g_syms);
    bounds = dbg_mem_alloc((unsigned long)cap * sizeof *bounds);
    if (!g_syms || !bounds) { dbg_mem_free(file); snprintf(g_err, sizeof g_err, "out of memory"); return 0; }
    for (i = 0; i < nsym; i += 1u + syms[i].aux) {
        const CoffSym *s = &syms[i];
        const char *name;
        size_t nl;
        int si;
        if (s->storage_class == COFF_CLASS_FILE) {
            /* the file name is in the aux records (18 bytes each) */
            const char *fn = (const char *)(s + 1);
            size_t max = (size_t)s->aux * sizeof(CoffSym), fl = 0;
            while (fl < max && fn[fl]) fl++;
            cur_file = dup_n(fn, fl);
            continue;
        }
        if (s->storage_class != COFF_CLASS_EXTERNAL && s->storage_class != COFF_CLASS_STATIC)
            continue;
        if (s->n.l.zeros == 0) {
            if (s->n.l.offset >= strsize) continue;
            name = strtab + s->n.l.offset;   /* NUL-terminated in the copy */
            nl = strlen(name);
        } else {
            name = s->n.short_name;
            for (nl = 0; nl < 8 && name[nl]; nl++) {}
        }
        /* every symbol in a section bounds the object before it (an input
         * section's own .data$x / .bss symbol included) */
        if (s->section > 0 && (unsigned)s->section <= nsec)
            bounds[nb++] = sec[s->section - 1].lo + s->value;
        if (!nl || name[0] == '.') continue;         /* section definitions, .refptr, .weak */
        if (s->section == IMAGE_SYM_ABSOLUTE) {
            /* the PS1 data symbols (--just-symbols): lookups only */
            if (s->storage_class != COFF_CLASS_EXTERNAL || !s->value) continue;
            g_syms[g_nsyms].addr = s->value;
            g_syms[g_nsyms].size = 0;
            g_syms[g_nsyms].name = s->n.l.zeros == 0 ? name : dup_n(name, nl);
            g_syms[g_nsyms].file = NULL;
            g_syms[g_nsyms].data = 0;
            g_nsyms++;
            continue;
        }
        if (s->section <= 0 || (unsigned)s->section > nsec) continue;
        si = s->section - 1;
        if (sh[si].Characteristics & IMAGE_SCN_CNT_CODE) continue;
        if (s->type == 0x20) continue;                   /* a function */
        g_syms[g_nsyms].addr = sec[si].lo + s->value;
        g_syms[g_nsyms].size = 0;                        /* sized below */
        g_syms[g_nsyms].name = s->n.l.zeros == 0 ? name : dup_n(name, nl);
        g_syms[g_nsyms].file = s->storage_class == COFF_CLASS_STATIC ? cur_file : NULL;
        g_syms[g_nsyms].data = sec[si].data;
        g_nsyms++;
    }
    dbg_mem_free(file);
    qsort(g_syms, (size_t)g_nsyms, sizeof *g_syms, sym_cmp);
    qsort(bounds, (size_t)nb, sizeof *bounds, ul_cmp);
    /* sizes: up to the next symbol at a higher address in the same section */
    for (k = 0; k < g_nsyms; k++) {
        int si = -1, j;
        unsigned long end;
        for (j = 0; j < (int)nsec; j++)
            if (g_syms[k].addr >= sec[j].lo && g_syms[k].addr < sec[j].hi) { si = j; break; }
        if (si < 0) {
            /* the PS1 data symbols (--just-symbols, absolute): to the next
             * one in PS1 RAM, as ELF's sizes give the toolkit's peeks and
             * `when NAME=VALUE` conditions their width (now_step: 1) */
            g_syms[k].size = 0;
            if (g_syms[k].addr >= 0x80000000ul && g_syms[k].addr < 0x80200000ul) {
                for (j = k + 1; j < g_nsyms && g_syms[j].addr == g_syms[k].addr; j++) {}
                g_syms[k].size = (j < g_nsyms && g_syms[j].addr < 0x80200000ul
                                  ? g_syms[j].addr : 0x80200000ul) - g_syms[k].addr;
            }
            continue;
        }
        end = sec[si].hi;
        {
            /* the first bound above the symbol */
            int lo = 0, hi = nb - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (bounds[mid] <= g_syms[k].addr) lo = mid + 1; else hi = mid - 1;
            }
            if (lo < nb && bounds[lo] < end) end = bounds[lo];
        }
        g_syms[k].size = end - g_syms[k].addr;
    }
    dbg_mem_free(bounds);
    g_state = 1;
    return 1;
}

int dbg_plat_image(const DbgImgSym **syms, int *n) {
    if (!load()) { *syms = NULL; *n = 0; return 0; }
    *syms = g_syms;
    *n = g_nsyms;
    return 1;
}

const char *dbg_plat_image_error(void) { return g_err; }

int dbg_plat_data_ranges(unsigned long (*out)[2], int cap) {
    int i;
    load();
    for (i = 0; i < g_ndata && i < cap; i++) {
        out[i][0] = g_data[i][0];
        out[i][1] = g_data[i][1];
    }
    return i;
}

const char *dbg_plat_build_id(void) {
    load();
    return g_build_id;
}

const char *dbg_plat_name(void) { return "windows-x86_64"; }

/* VirtualQuery over the user address space, as /proc/self/maps lines:
 * "lo-hi perms 00000000 00:00 0 path", path = the module of an image
 * region, [heap] for the process heaps' regions, empty otherwise */
char *dbg_plat_maps(unsigned long *len) {
    unsigned long cap = 1 << 16, n = 0;
    char *out = dbg_mem_alloc(cap);
    unsigned char *a = (unsigned char *)0x10000;
    HANDLE heaps[64];
    DWORD nheaps = GetProcessHeaps(64, heaps), h;
    MEMORY_BASIC_INFORMATION m;
    *len = 0;
    if (!out) return NULL;
    while (VirtualQuery(a, &m, sizeof m) == sizeof m) {
        unsigned char *next = (unsigned char *)m.BaseAddress + m.RegionSize;
        if (m.State == MEM_COMMIT) {
            char path[MAX_PATH * 3] = "", line[MAX_PATH * 3 + 128];
            DWORD p = m.Protect & 0xff;
            const char *perm = p == PAGE_READONLY ? "r--p" : p == PAGE_READWRITE ? "rw-p" :
                               p == PAGE_WRITECOPY ? "rw-p" : p == PAGE_EXECUTE ? "--xp" :
                               p == PAGE_EXECUTE_READ ? "r-xp" : p == PAGE_EXECUTE_READWRITE ? "rwxp" :
                               p == PAGE_EXECUTE_WRITECOPY ? "rwxp" : "---p";
            int ln;
            if (m.Type == MEM_IMAGE) {
                wchar_t w[MAX_PATH];
                if (GetModuleFileNameW((HMODULE)m.AllocationBase, w, MAX_PATH))
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, path, sizeof path, NULL, NULL);
            } else {
                for (h = 0; h < nheaps; h++)
                    if (m.AllocationBase == (void *)heaps[h]) { strcpy(path, "[heap]"); break; }
            }
            ln = snprintf(line, sizeof line, "%llx-%llx %s 00000000 00:00 0 %s\n",
                          (unsigned long long)(uintptr_t)m.BaseAddress,
                          (unsigned long long)(uintptr_t)next, perm, path);
            if (n + (unsigned long)ln + 1 > cap) {
                cap *= 2;
                out = dbg_mem_realloc(out, cap);
                if (!out) return NULL;
            }
            memcpy(out + n, line, (size_t)ln);
            n += (unsigned long)ln;
        }
        if (next <= a) break;
        a = next;
    }
    out[n] = 0;
    *len = n;
    return out;
}

/* ------------------------------------------------------------------ */
/* the toolkit's own stack                                             */
/* ------------------------------------------------------------------ */

#define DBG_STACK_SIZE (4u << 20)
static LPVOID g_game_fiber, g_tk_fiber;
static void (*g_fn)(void);

static VOID CALLBACK tk_main(LPVOID arg) {
    (void)arg;
    for (;;) {
        g_fn();
        SwitchToFiber(g_game_fiber);
    }
}

void dbg_plat_run_on_toolkit_stack(void (*fn)(void)) {
    g_fn = fn;
    if (!g_game_fiber) {
        g_game_fiber = ConvertThreadToFiber(NULL);
        if (!g_game_fiber) g_game_fiber = GetCurrentFiber();   /* already one */
    }
    if (!g_tk_fiber) g_tk_fiber = CreateFiber(DBG_STACK_SIZE, tk_main, NULL);
    if (!g_game_fiber || !g_tk_fiber) {
        static int warned;
        if (!warned++) fprintf(stderr, "[DBG] no toolkit fiber (%lu): running on the game stack\n",
                               (unsigned long)GetLastError());
        fn();
        return;
    }
    SwitchToFiber(g_tk_fiber);
}

/* Commit SIZE bytes of the stack below the caller: an alloca of that size
 * runs ___chkstk_ms, which touches each page from the top down (the guard
 * page mechanism commits them) without changing a byte.  The fresh pages
 * read as zero, as an untouched Linux stack does. */
__attribute__((noinline)) void dbg_plat_stack_prepare(unsigned long size) {
    volatile char *p = alloca(size + 8192);
    (void)p[0];
}

/* ------------------------------------------------------------------ */
/* console transport                                                   */
/* ------------------------------------------------------------------ */

#define MAX_HANDLES 8
#define STDIN_HANDLE 100
static SOCKET g_sock[MAX_HANDLES];
static int g_wsa;
static char g_sock_err[160];

/* stdin: a reader thread fills a buffer the console drains */
static CRITICAL_SECTION g_in_mu;
static char g_in_buf[8192];
static unsigned long g_in_len;
static int g_in_eof;

const char *dbg_sock_error(void) { return g_sock_err; }

static int wsa(void) {
    WSADATA d;
    if (!g_wsa) {
        int i;
        for (i = 0; i < MAX_HANDLES; i++) g_sock[i] = INVALID_SOCKET;
        g_wsa = WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 1 : -1;
        if (g_wsa < 0) snprintf(g_sock_err, sizeof g_sock_err, "WSAStartup failed");
    }
    return g_wsa > 0;
}

static int slot(SOCKET s) {
    int i;
    for (i = 1; i < MAX_HANDLES; i++)
        if (g_sock[i] == INVALID_SOCKET) { g_sock[i] = s; return i; }
    closesocket(s);
    snprintf(g_sock_err, sizeof g_sock_err, "too many sockets");
    return -1;
}

static void sock_fail(SOCKET s) {
    snprintf(g_sock_err, sizeof g_sock_err, "Winsock error %d", WSAGetLastError());
    if (s != INVALID_SOCKET) closesocket(s);
}

int dbg_sock_listen_tcp(int port) {
    struct sockaddr_in sa;
    u_long nb = 1;
    BOOL excl = TRUE;
    SOCKET s;
    if (!wsa()) return -1;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* local only */
    if (s != INVALID_SOCKET)
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&sa, sizeof sa) || listen(s, 1) ||
        ioctlsocket(s, FIONBIO, &nb)) {
        sock_fail(s);
        return -1;
    }
    return slot(s);
}

/* AF_UNIX (Windows 10 1803+, <afunix.h>): the address layout, without the
 * header (not every MinGW ships it) */
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
typedef struct { ADDRESS_FAMILY sun_family; char sun_path[108]; } WinSockaddrUn;

int dbg_sock_listen_unix(const char *path) {
    WinSockaddrUn sa;
    u_long nb = 1;
    SOCKET s;
    if (!wsa()) return -1;
    DeleteFileA(path);
    WSASetLastError(0);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        /* no AF_UNIX on this host (Windows before 10 1803; Wine 10) */
        snprintf(g_sock_err, sizeof g_sock_err, "this system has no unix sockets "
                 "(Winsock error %d); use PC_DBG_CONSOLE=PORT", WSAGetLastError());
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) || listen(s, 1) || ioctlsocket(s, FIONBIO, &nb)) {
        sock_fail(s);
        return -1;
    }
    return slot(s);
}

int dbg_sock_accept(int listener) {
    SOCKET c;
    u_long nb = 1;
    if (listener <= 0 || listener >= MAX_HANDLES || g_sock[listener] == INVALID_SOCKET) return -1;
    c = accept(g_sock[listener], NULL, NULL);
    if (c == INVALID_SOCKET) return -1;
    ioctlsocket(c, FIONBIO, &nb);
    return slot(c);
}

long dbg_sock_recv(int h, char *buf, unsigned long cap) {
    int r;
    if (h == STDIN_HANDLE) {
        unsigned long n;
        EnterCriticalSection(&g_in_mu);
        n = g_in_len < cap ? g_in_len : cap;
        if (n) {
            memcpy(buf, g_in_buf, n);
            memmove(g_in_buf, g_in_buf + n, g_in_len - n);
            g_in_len -= n;
        }
        r = n ? (int)n : g_in_eof ? 0 : -1;
        LeaveCriticalSection(&g_in_mu);
        return r;
    }
    if (h <= 0 || h >= MAX_HANDLES || g_sock[h] == INVALID_SOCKET) return -2;
    r = recv(g_sock[h], buf, (int)(cap > 0x7fffffff ? 0x7fffffff : cap), 0);
    if (r == SOCKET_ERROR) return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
    return r;
}

void dbg_sock_send(int h, const char *buf, unsigned long n) {
    if (h <= 0 || h >= MAX_HANDLES || g_sock[h] == INVALID_SOCKET) return;
    while (n) {
        int w = send(g_sock[h], buf, (int)n, 0);
        if (w == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(1); continue; }
            return;
        }
        buf += w;
        n -= (unsigned long)w;
    }
}

void dbg_sock_close(int h) {
    if (h <= 0 || h >= MAX_HANDLES || g_sock[h] == INVALID_SOCKET) return;
    closesocket(g_sock[h]);
    g_sock[h] = INVALID_SOCKET;
}

static DWORD WINAPI stdin_reader(LPVOID arg) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char chunk[1024];
    (void)arg;
    for (;;) {
        DWORD got = 0;
        int done = !ReadFile(in, chunk, sizeof chunk, &got, NULL) || got == 0;
        EnterCriticalSection(&g_in_mu);
        if (done) g_in_eof = 1;
        else {
            /* wait for room: the console drains it at every boundary */
            while (g_in_len + got > sizeof g_in_buf) {
                LeaveCriticalSection(&g_in_mu);
                Sleep(5);
                EnterCriticalSection(&g_in_mu);
            }
            memcpy(g_in_buf + g_in_len, chunk, got);
            g_in_len += got;
        }
        LeaveCriticalSection(&g_in_mu);
        if (done) return 0;
    }
}

int dbg_stdin_open(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), t;
    if (!in || in == INVALID_HANDLE_VALUE) {
        snprintf(g_sock_err, sizeof g_sock_err, "no standard input");
        return -1;
    }
    InitializeCriticalSection(&g_in_mu);
    t = CreateThread(NULL, 0, stdin_reader, NULL, 0, NULL);
    if (!t) {
        snprintf(g_sock_err, sizeof g_sock_err, "cannot start the stdin reader");
        return -1;
    }
    CloseHandle(t);
    return STDIN_HANDLE;
}
#endif /* _WIN32 */
