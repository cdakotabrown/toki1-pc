/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_fs.c -- UTF-8 file-system helpers (pc_fs.h). */
#include "pc_fs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <wchar.h>

#undef rename

/* UTF-8 -> freshly malloc'd UTF-16 (NULL on failure). */
static wchar_t *widen(const char *s) {
    /* UTF-8; a byte string that is not (a CUE written in Shift-JIS on a
     * Japanese system) is read in the ANSI code page instead */
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) {
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, s, -1, NULL, 0);
    }
    if (n <= 0) { errno = EINVAL; return NULL; }
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(cp, flags, s, -1, w, n);
    return w;
}

static int narrow(const wchar_t *w, char *out, size_t n) {
    int got = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    return got > 0 ? 0 : -1;
}

static int set_errno_from_win(void) {
    DWORD e = GetLastError();
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_ACCESS_DENIED: errno = EACCES; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_DIR_NOT_EMPTY: errno = ENOTEMPTY; break;
    default: errno = EIO; break;
    }
    return -1;
}

FILE *pc_fs_fopen(const char *path, const char *mode) {
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp); free(wm);
    return f;
}

FILE *pc_fs_freopen(const char *path, const char *mode, FILE *f) {
    wchar_t *wp = widen(path), *wm = widen(mode);
    FILE *r = (wp && wm) ? _wfreopen(wp, wm, f) : NULL;
    free(wp); free(wm);
    return r;
}

FILE *pc_fs_create_excl(const char *path) {
    wchar_t *wp = widen(path);
    int fd;
    FILE *f;
    if (!wp) return NULL;
    fd = _wopen(wp, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    free(wp);
    if (fd < 0) return NULL;
    f = _fdopen(fd, "wb");
    if (!f) _close(fd);
    return f;
}

static int64_t ft_ns(FILETIME ft) {
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(v * 100u);
}

int pc_fs_stat(const char *path, PcStat *st) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t *wp = widen(path);
    BOOL ok;
    memset(st, 0, sizeof *st);
    if (!wp) return -1;
    ok = GetFileAttributesExW(wp, GetFileExInfoStandard, &a);
    free(wp);
    if (!ok) { set_errno_from_win(); return -1; }
    st->exists = 1;
    st->is_link = (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    st->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->is_reg = !st->is_dir && !st->is_link;
    st->size = (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
    st->mtime_ns = ft_ns(a.ftLastWriteTime);
    st->ctime_ns = ft_ns(a.ftCreationTime);
    return 0;
}

int pc_fs_fstat(FILE *f, PcStat *st) {
    BY_HANDLE_FILE_INFORMATION i;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    memset(st, 0, sizeof *st);
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &i)) return -1;
    st->exists = 1;
    st->is_dir = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->is_reg = !st->is_dir;
    st->size = (int64_t)(((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow);
    st->dev = i.dwVolumeSerialNumber;
    st->ino = ((uint64_t)i.nFileIndexHigh << 32) | i.nFileIndexLow;
    st->mtime_ns = ft_ns(i.ftLastWriteTime);
    st->ctime_ns = ft_ns(i.ftCreationTime);
    return 0;
}

int pc_fs_mkdir(const char *path) {
    wchar_t *wp = widen(path);
    BOOL ok;
    if (!wp) return -1;
    ok = CreateDirectoryW(wp, NULL);
    free(wp);
    return ok ? 0 : set_errno_from_win();
}

int pc_fs_rmdir(const char *path) {
    wchar_t *wp = widen(path);
    BOOL ok;
    if (!wp) return -1;
    ok = RemoveDirectoryW(wp);
    free(wp);
    return ok ? 0 : set_errno_from_win();
}

int pc_fs_unlink(const char *path) {
    wchar_t *wp = widen(path);
    BOOL ok;
    if (!wp) return -1;
    ok = DeleteFileW(wp);
    free(wp);
    return ok ? 0 : set_errno_from_win();
}

int pc_fs_rename(const char *from, const char *to) {
    wchar_t *a = widen(from), *b = widen(to);
    BOOL ok = a && b && MoveFileExW(a, b, 0);
    free(a); free(b);
    return ok ? 0 : set_errno_from_win();
}

int pc_fs_link(const char *from, const char *to) {
    wchar_t *a = widen(from), *b = widen(to);
    BOOL ok = a && b && CreateHardLinkW(b, a, NULL);
    free(a); free(b);
    return ok ? 0 : set_errno_from_win();
}

int pc_fs_listdir(const char *path, pc_fs_dir_fn fn, void *ud) {
    char pat[4096], name[1024], full[4096];
    wchar_t *wp;
    WIN32_FIND_DATAW d;
    HANDLE h;
    int rc = 0;
    snprintf(pat, sizeof pat, "%s\\*", path);
    wp = widen(pat);
    if (!wp) return -1;
    h = FindFirstFileW(wp, &d);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        PcStat st;
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
        if (narrow(d.cFileName, name, sizeof name)) continue;
        memset(&st, 0, sizeof st);
        st.exists = 1;
        st.is_link = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        st.is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        st.is_reg = !st.is_dir && !st.is_link;
        st.size = (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
        (void)full;
        rc = fn(name, &st, ud);
    } while (!rc && FindNextFileW(h, &d));
    FindClose(h);
    return rc;
}

int64_t pc_fs_free_space(const char *path) {
    ULARGE_INTEGER avail;
    wchar_t *wp = widen(path);
    BOOL ok;
    if (!wp) return -1;
    ok = GetDiskFreeSpaceExW(wp, &avail, NULL, NULL);
    free(wp);
    return ok ? (int64_t)avail.QuadPart : -1;
}

int pc_fs_exe_dir(char *out, size_t n) {
    wchar_t w[4096];
    DWORD got = GetModuleFileNameW(NULL, w, 4096);
    wchar_t *slash;
    if (!got || got >= 4096) return -1;
    slash = wcsrchr(w, L'\\');
    if (!slash) slash = wcsrchr(w, L'/');
    if (slash) *slash = 0;
    return narrow(w, out, n);
}

int pc_fs_absolute(const char *path, char *out, size_t n) {
    wchar_t *wp = widen(path), full[4096];
    DWORD got;
    if (!wp) return -1;
    got = GetFullPathNameW(wp, 4096, full, NULL);
    free(wp);
    if (!got || got >= 4096) return -1;
    return narrow(full, out, n);
}

int pc_fs_resolve(const char *path, char *out, size_t n) { return pc_fs_absolute(path, out, n); }

const char *pc_fs_getenv(const char *name) {
    static char buf[4096];
    wchar_t *wn = widen(name);
    const wchar_t *v = wn ? _wgetenv(wn) : NULL;
    free(wn);
    if (!v || narrow(v, buf, sizeof buf)) return NULL;
    return buf;
}

int pc_fs_lock(const char *path, int wait, PcFsLock *l) {
    wchar_t *wp = widen(path);
    HANDLE h;
    OVERLAPPED ov;
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    memset(l, 0, sizeof *l);
    if (!wp) { errno = EINVAL; return -1; }
    /* DELETE_ON_CLOSE: the file goes away when its last handle closes,
     * which the system does for a program that crashed, too */
    h = CreateFileW(wp, GENERIC_READ | GENERIC_WRITE | DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return set_errno_from_win();
    memset(&ov, 0, sizeof ov);
    if (!LockFileEx(h, flags, 0, 1, 0, &ov)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) { errno = EBUSY; return 1; }
        SetLastError(e);
        return set_errno_from_win();
    }
    l->h = h;
    l->held = 1;
    return 0;
}

void pc_fs_unlock(PcFsLock *l) {
    OVERLAPPED ov;
    if (!l->held) return;
    memset(&ov, 0, sizeof ov);
    UnlockFileEx((HANDLE)l->h, 0, 1, 0, &ov);
    CloseHandle((HANDLE)l->h);
    l->held = 0;
}

#else /* POSIX */

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

FILE *pc_fs_fopen(const char *path, const char *mode) { return fopen(path, mode); }
FILE *pc_fs_freopen(const char *path, const char *mode, FILE *f) { return freopen(path, mode, f); }

FILE *pc_fs_create_excl(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    FILE *f;
    if (fd < 0) return NULL;
    f = fdopen(fd, "wb");
    if (!f) close(fd);
    return f;
}

static void fill(PcStat *st, const struct stat *s) {
    st->exists = 1;
    st->is_link = S_ISLNK(s->st_mode);
    st->is_dir = S_ISDIR(s->st_mode);
    st->is_reg = S_ISREG(s->st_mode);
    st->size = (int64_t)s->st_size;
    st->dev = (uint64_t)s->st_dev;
    st->ino = (uint64_t)s->st_ino;
    st->mtime_ns = (int64_t)s->st_mtim.tv_sec * 1000000000 + s->st_mtim.tv_nsec;
    st->ctime_ns = (int64_t)s->st_ctim.tv_sec * 1000000000 + s->st_ctim.tv_nsec;
}

int pc_fs_stat(const char *path, PcStat *st) {
    struct stat s, t;
    memset(st, 0, sizeof *st);
    if (lstat(path, &s)) return -1;
    if (S_ISLNK(s.st_mode) && stat(path, &t) == 0) {
        /* a link: the target's type and size, with is_link kept so the
         * callers that refuse links (verify_cache's checked_path) still can */
        fill(st, &t);
        st->is_link = 1;
        return 0;
    }
    fill(st, &s);
    return 0;
}

int pc_fs_fstat(FILE *f, PcStat *st) {
    struct stat s;
    memset(st, 0, sizeof *st);
    if (fstat(fileno(f), &s)) return -1;
    fill(st, &s);
    return 0;
}

int pc_fs_mkdir(const char *path) { return mkdir(path, 0755); }
int pc_fs_rmdir(const char *path) { return rmdir(path); }
int pc_fs_unlink(const char *path) { return unlink(path); }

int pc_fs_rename(const char *from, const char *to) {
    struct stat s;
    if (!lstat(to, &s)) { errno = EEXIST; return -1; }
    return rename(from, to);
}

int pc_fs_link(const char *from, const char *to) { return link(from, to); }

int pc_fs_listdir(const char *path, pc_fs_dir_fn fn, void *ud) {
    DIR *d = opendir(path);
    struct dirent *e;
    int rc = 0;
    char full[PATH_MAX + 256];
    if (!d) return -1;
    while (!rc && (e = readdir(d))) {
        PcStat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (pc_fs_stat(full, &st)) continue;
        rc = fn(e->d_name, &st, ud);
    }
    closedir(d);
    return rc;
}

int64_t pc_fs_free_space(const char *path) {
    struct statvfs v;
    if (statvfs(path, &v)) return -1;
    return (int64_t)v.f_bavail * (int64_t)v.f_frsize;
}

int pc_fs_exe_dir(char *out, size_t n) {
    char buf[PATH_MAX];
    ssize_t got = readlink("/proc/self/exe", buf, sizeof buf - 1);
    char *slash;
    if (got <= 0) return -1;
    buf[got] = 0;
    slash = strrchr(buf, '/');
    if (slash) *slash = 0;
    if (strlen(buf) + 1 > n) return -1;
    memcpy(out, buf, strlen(buf) + 1);
    return 0;
}

int pc_fs_resolve(const char *path, char *out, size_t n) {
    char real[PATH_MAX];
    /* Path.resolve(): an existing path has its symlinks resolved, so a CUE
     * reached through a link names its BINs relative to the real folder */
    if (realpath(path, real)) {
        if (strlen(real) + 1 > n) return -1;
        memcpy(out, real, strlen(real) + 1);
        return 0;
    }
    return pc_fs_absolute(path, out, n);
}

int pc_fs_absolute(const char *path, char *out, size_t n) {
    char cwd[PATH_MAX];
    if (path[0] == '/') {
        if (strlen(path) + 1 > n) return -1;
        memcpy(out, path, strlen(path) + 1);
        return 0;
    }
    if (!getcwd(cwd, sizeof cwd)) return -1;
    if ((size_t)snprintf(out, n, "%s/%s", cwd, path) >= n) return -1;
    return 0;
}

const char *pc_fs_getenv(const char *name) { return getenv(name); }


int pc_fs_lock(const char *path, int wait, PcFsLock *l) {
    int tries;
    memset(l, 0, sizeof *l);
    snprintf(l->path, sizeof l->path, "%s", path);
    for (tries = 0; tries < 50; tries++) {
        struct stat a, b;
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return -1;
        while (flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
            int e = errno;
            if (e == EINTR) continue;
            close(fd);
            if (e == EWOULDBLOCK) { errno = EBUSY; return 1; }
            errno = e;
            return -1;
        }
        /* the holder before us removes the file as it lets go: a lock on
         * that removed file guards nothing, so take the one now there */
        if (fstat(fd, &a) == 0 && stat(path, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
            l->fd = fd;
            l->held = 1;
            return 0;
        }
        close(fd);
    }
    errno = EAGAIN;
    return -1;
}

void pc_fs_unlock(PcFsLock *l) {
    if (!l->held) return;
    unlink(l->path);            /* while still held: nobody locks the old file */
    flock(l->fd, LOCK_UN);
    close(l->fd);
    l->held = 0;
}

#endif

int pc_fs_mkdirs(const char *path) {
    char tmp[4096];
    char *p;
    PcStat st;
    if (strlen(path) >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
#ifdef _WIN32
            if (p[-1] == ':') continue;    /* "C:\" */
#endif
            *p = 0;
            if (pc_fs_stat(tmp, &st) || !st.is_dir) {
                if (pc_fs_mkdir(tmp) && errno != EEXIST) return -1;
            }
            *p = c;
        }
    }
    if (pc_fs_stat(tmp, &st) == 0 && st.is_dir) return 0;
    if (pc_fs_mkdir(tmp) && errno != EEXIST) return -1;
    return 0;
}

int pc_fs_copy(const char *from, const char *to) {
    FILE *in = pc_fs_fopen(from, "rb"), *out;
    char buf[65536];
    size_t n;
    int rc = 0;
    if (!in) return -1;
    out = pc_fs_create_excl(to);
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out)) rc = -1;
    return rc;
}

typedef struct { const char *base; int rc; } RmCtx;

static int rm_entry(const char *name, const PcStat *st, void *ud) {
    RmCtx *c = (RmCtx *)ud;
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", c->base, name);
    if (st->is_dir && !st->is_link) {
        if (pc_fs_rmtree(full)) c->rc = -1;
    } else if (pc_fs_unlink(full)) {
#ifdef _WIN32
        if (!(st->is_dir && pc_fs_rmdir(full) == 0))
#endif
            c->rc = -1;
    }
    return 0;
}

int pc_fs_rmtree(const char *path) {
    RmCtx c;
    PcStat st;
    if (pc_fs_stat(path, &st)) return 0;
    if (!st.is_dir || st.is_link) return pc_fs_unlink(path);
    c.base = path;
    c.rc = 0;
    pc_fs_listdir(path, rm_entry, &c);
    if (pc_fs_rmdir(path)) c.rc = -1;
    return c.rc;
}
