/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_fs.h -- UTF-8 file-system helpers for the first-run setup
 * (pc/hal/asset_setup.c, pc/hal/setup_screen.c, pc/hal/asset_root.c).
 *
 * Every path is UTF-8.  On Windows each call goes through the wide (UTF-16)
 * API, so a disc image or a user folder whose name is not representable in
 * the ANSI code page still works (SDL hands dropped files over in UTF-8).
 * On POSIX they are the plain calls.
 */
#ifndef PC_FS_H
#define PC_FS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int exists, is_dir, is_reg, is_link;
    int64_t size;
    uint64_t dev, ino;
    int64_t mtime_ns, ctime_ns;
} PcStat;

FILE *pc_fs_fopen(const char *path, const char *mode);
FILE *pc_fs_freopen(const char *path, const char *mode, FILE *f);
/* Create a new file for binary writing; fails (NULL, errno EEXIST) when it
 * already exists (Python's open(path, 'xb')). */
FILE *pc_fs_create_excl(const char *path);
/* 0, or -1 when missing.  A symlink reports its target's type and size
 * with is_link set (a dangling one: is_link only); Windows reports a
 * reparse point's own attributes. */
int pc_fs_stat(const char *path, PcStat *st);
int pc_fs_fstat(FILE *f, PcStat *st);
int pc_fs_mkdir(const char *path);          /* 0, or -1 with errno (EEXIST) */
int pc_fs_mkdirs(const char *path);         /* mkdir -p */
int pc_fs_rmdir(const char *path);
int pc_fs_unlink(const char *path);
int pc_fs_rename(const char *from, const char *to);   /* never replaces */
int pc_fs_link(const char *from, const char *to);     /* hard link */
int pc_fs_copy(const char *from, const char *to);     /* new file, byte copy */
/* Delete a tree this program created (stage folders); symlinks are removed,
 * never followed. */
int pc_fs_rmtree(const char *path);
/* Call fn for each entry (not . / ..); stops early when fn returns non-zero
 * and returns that value.  -1 when the folder cannot be read. */
typedef int (*pc_fs_dir_fn)(const char *name, const PcStat *st, void *ud);
int pc_fs_listdir(const char *path, pc_fs_dir_fn fn, void *ud);
/* Bytes free for this user on the volume holding path, or -1. */
int64_t pc_fs_free_space(const char *path);
/* Folder of the running executable (no trailing separator); 0 on success. */
int pc_fs_exe_dir(char *out, size_t n);
/* Absolute form of path (relative to the working directory); 0 on success. */
int pc_fs_absolute(const char *path, char *out, size_t n);
/* Absolute form with symlinks resolved when the path exists (Python's
 * Path.resolve()); 0 on success. */
int pc_fs_resolve(const char *path, char *out, size_t n);
/* An exclusive lock held through a file at `path` (flock / LockFileEx):
 * the system lets go of it when the holder exits, crashed or not, so a
 * left-behind file never blocks anyone.  pc_fs_unlock removes the file
 * (Windows: deleted when its last handle closes).  Returns 0 when held,
 * 1 when another process holds it (wait = 0 only), -1 on error (errno). */
typedef struct {
    int held;
    int fd;
    void *h;
    char path[1024];
} PcFsLock;
int pc_fs_lock(const char *path, int wait, PcFsLock *l);
void pc_fs_unlock(PcFsLock *l);
/* Environment variable in UTF-8 (Windows: the wide environment); NULL when
 * unset.  The string is valid until the next call. */
const char *pc_fs_getenv(const char *name);

#ifdef __cplusplus
}
#endif
#endif
