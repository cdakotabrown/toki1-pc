/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dbg_platform_posix.c -- the debug toolkit's host layer on Linux
 * (pc/hal/dbg_platform.h): the executable's own ELF symbol table, the GNU
 * build-id, /proc/self/maps, mmap'd buffers, the ucontext toolkit stack and
 * BSD sockets.  The Windows counterpart is pc/hal/win/win_dbg_platform.c.
 *
 * Every static here belongs to the process (symbol tables, the toolkit's
 * stack, sockets): pc/hal/dbg_state.c keeps this file out of a state
 * (KEEP_FILES).
 */
#ifndef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "dbg_platform.h"

extern char __data_start[], _end[];

/* ------------------------------------------------------------------ */
/* memory and files                                                    */
/* ------------------------------------------------------------------ */

/* pc/src/main.c maps the MMIO window at 0x1F801000 after the HAL has
 * initialised, and with ASLR the brk heap can already span that address:
 * a malloc'd block (or a stdio buffer) could alias the SPU register mirror.
 * Toolkit buffers therefore come from mmap, and files go through
 * open/read/write, never stdio. */
void *dbg_mem_alloc(unsigned long n) {
    unsigned long *p = mmap(NULL, n + 16, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    p[0] = n + 16;
    return p + 2;
}

void dbg_mem_free(void *q) {
    unsigned long *p = q;
    if (p) munmap(p - 2, p[-2]);
}

void *dbg_mem_realloc(void *q, unsigned long n) {
    unsigned long *p = q, old = p ? p[-2] - 16 : 0;
    void *r = dbg_mem_alloc(n);
    if (r && p) memcpy(r, p, old < n ? old : n);
    dbg_mem_free(q);
    return r;
}

unsigned char *dbg_read_file(const char *path, unsigned long *len) {
    int fd = open(path, O_RDONLY);
    unsigned long cap = 1 << 16, n = 0;
    unsigned char *buf;
    *len = 0;
    if (fd < 0) return NULL;
    buf = dbg_mem_alloc(cap);
    while (buf) {
        ssize_t r;
        if (n + 1 >= cap) { cap *= 2; buf = dbg_mem_realloc(buf, cap); if (!buf) break; }
        r = read(fd, buf + n, cap - n - 1);
        if (r < 0) { dbg_mem_free(buf); buf = NULL; break; }
        if (r == 0) break;
        n += (unsigned long)r;
    }
    close(fd);
    if (buf) buf[n] = 0;
    *len = n;
    return buf;
}

int dbg_write_file(const char *path, const void *data, unsigned long n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const unsigned char *p = data;
    if (fd < 0) return 0;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { close(fd); return 0; }
        p += w;
        n -= (unsigned long)w;
    }
    return close(fd) == 0;
}

int dbg_replace_file(const char *from, const char *to) {
    return rename(from, to);
}

void dbg_plat_setenv(const char *name, const char *value) {
    setenv(name, value, 1);
}

/* ------------------------------------------------------------------ */
/* the executable's own symbol table                                   */
/* ------------------------------------------------------------------ */

static unsigned char *g_elf;
static unsigned long g_elf_size;
static DbgImgSym *g_syms;
static int g_nsyms;
static char g_build_id[80];
static int g_state;             /* 0 not tried, 1 ok, -1 failed */
static char g_err[160];

static int sym_cmp(const void *a, const void *b) {
    const DbgImgSym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static int elf_load(void) {
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *sh;
    int i;
    if (g_state) return g_state > 0;
    g_state = -1;
    g_elf = dbg_read_file("/proc/self/exe", &g_elf_size);
    if (!g_elf || g_elf_size < sizeof(Elf64_Ehdr)) {
        snprintf(g_err, sizeof g_err, "cannot read /proc/self/exe");
        return 0;
    }
    eh = (const Elf64_Ehdr *)g_elf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64) {
        snprintf(g_err, sizeof g_err, "/proc/self/exe is not a 64-bit ELF file");
        return 0;
    }
    sh = (const Elf64_Shdr *)(g_elf + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_NOTE) {
            /* .note.gnu.build-id: namesz descsz type "GNU\0" desc */
            const uint32_t *nt = (const uint32_t *)(g_elf + sh[i].sh_offset);
            if (nt[2] == NT_GNU_BUILD_ID && nt[0] == 4) {
                const unsigned char *d = (const unsigned char *)(nt + 3) + 4;
                uint32_t k;
                for (k = 0; k < nt[1] && k < 31; k++)
                    sprintf(g_build_id + 2 * k, "%02x", d[k]);
            }
        } else if (sh[i].sh_type == SHT_SYMTAB) {
            const Elf64_Sym *st = (const Elf64_Sym *)(g_elf + sh[i].sh_offset);
            const char *str = (const char *)(g_elf + sh[sh[i].sh_link].sh_offset);
            unsigned long cnt = sh[i].sh_size / sizeof *st, k;
            const char *file = NULL;
            g_syms = dbg_mem_alloc(cnt * sizeof *g_syms);
            if (!g_syms) { snprintf(g_err, sizeof g_err, "out of memory"); return 0; }
            for (k = 1; k < cnt; k++) {
                int type = ELF64_ST_TYPE(st[k].st_info);
                int bind = ELF64_ST_BIND(st[k].st_info);
                if (type == STT_FILE) { file = str + st[k].st_name; continue; }
                if (type == STT_FUNC || type == STT_SECTION || !st[k].st_name ||
                    st[k].st_shndx == SHN_UNDEF || !st[k].st_value)
                    continue;
                g_syms[g_nsyms].addr = st[k].st_value;
                g_syms[g_nsyms].size = st[k].st_size;
                g_syms[g_nsyms].name = str + st[k].st_name;
                g_syms[g_nsyms].file = bind == STB_LOCAL ? file : NULL;
                g_syms[g_nsyms].data = st[k].st_value >= (unsigned long)__data_start &&
                                       st[k].st_value < (unsigned long)_end;
                g_nsyms++;
            }
        }
    }
    if (!g_syms) {
        snprintf(g_err, sizeof g_err, "the executable has no symbol table (stripped?)");
        return 0;
    }
    qsort(g_syms, (size_t)g_nsyms, sizeof *g_syms, sym_cmp);
    g_state = 1;
    return 1;
}

int dbg_plat_image(const DbgImgSym **syms, int *n) {
    if (!elf_load()) { *syms = NULL; *n = 0; return 0; }
    *syms = g_syms;
    *n = g_nsyms;
    return 1;
}

const char *dbg_plat_image_error(void) { return g_err; }

int dbg_plat_data_ranges(unsigned long (*out)[2], int cap) {
    if (cap < 1) return 0;
    out[0][0] = (unsigned long)__data_start;
    out[0][1] = (unsigned long)_end;
    return 1;
}

const char *dbg_plat_build_id(void) {
    elf_load();
    return g_build_id;
}

const char *dbg_plat_name(void) { return "linux-x86_64"; }

char *dbg_plat_maps(unsigned long *len) {
    return (char *)dbg_read_file("/proc/self/maps", len);
}

/* ------------------------------------------------------------------ */
/* the toolkit's own stack                                             */
/* ------------------------------------------------------------------ */

#define DBG_STACK_SIZE (4u << 20)
static unsigned char g_dbg_stack[DBG_STACK_SIZE] __attribute__((aligned(16)));
static ucontext_t g_dbg_ctx, g_game_ctx;
static void (*g_fn)(void);

static void tramp(void) { g_fn(); }

void dbg_plat_run_on_toolkit_stack(void (*fn)(void)) {
    g_fn = fn;
    getcontext(&g_dbg_ctx);
    g_dbg_ctx.uc_stack.ss_sp = g_dbg_stack;
    g_dbg_ctx.uc_stack.ss_size = sizeof g_dbg_stack;
    g_dbg_ctx.uc_link = &g_game_ctx;
    makecontext(&g_dbg_ctx, tramp, 0);
    swapcontext(&g_game_ctx, &g_dbg_ctx);
}

/* the game stack is one mmap'd region (pc/src/main.c): nothing to do */
void dbg_plat_stack_prepare(unsigned long size) { (void)size; }

/* ------------------------------------------------------------------ */
/* console transport                                                   */
/* ------------------------------------------------------------------ */

static char g_sock_err[160];

const char *dbg_sock_error(void) { return g_sock_err; }

static int nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

int dbg_sock_listen_tcp(int port) {
    struct sockaddr_in sa;
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* local only */
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 1) || !nonblock(fd)) {
        snprintf(g_sock_err, sizeof g_sock_err, "%s", strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

int dbg_sock_listen_unix(const char *path) {
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    unlink(sa.sun_path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 1) || !nonblock(fd)) {
        snprintf(g_sock_err, sizeof g_sock_err, "%s", strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

int dbg_sock_accept(int listener) {
    int c = accept(listener, NULL, NULL);
    if (c < 0) return -1;
    nonblock(c);
    return c;
}

long dbg_sock_recv(int h, char *buf, unsigned long cap) {
    struct pollfd pf;
    ssize_t r;
    pf.fd = h;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, 0) <= 0 || !(pf.revents & (POLLIN | POLLHUP))) return -1;
    r = read(h, buf, cap);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
    if (r < 0) return -2;
    return (long)r;
}

void dbg_sock_send(int h, const char *buf, unsigned long n) {
    /* a vanished client must not SIGPIPE the game */
    ssize_t w = send(h, buf, n, MSG_NOSIGNAL);
    (void)w;
}

void dbg_sock_close(int h) {
    if (h > 0) close(h);
}

int dbg_stdin_open(void) {
    if (!nonblock(0)) return -1;
    return 0;
}
#endif /* !_WIN32 */
