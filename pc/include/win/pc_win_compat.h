/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pc_win_compat.h -- force-included into every Windows translation unit
 * (pc/Makefile, PLATFORM=windows) for the POSIX calls MinGW's C runtime
 * has with a different meaning or not at all.
 *
 * rename: msvcrt's rename() fails when the destination exists, so every
 * "write a temp file, rename it over the old one" save in pc/hal (settings,
 * quicksave index, autosave, extras, music dump) would succeed once and then
 * fail forever.  pc_win_rename replaces the destination (MoveFileEx with
 * MOVEFILE_REPLACE_EXISTING), as POSIX rename does.
 * fsync: _commit flushes a descriptor to disk. */
#ifndef PC_WIN_COMPAT_H
#define PC_WIN_COMPAT_H
/* No system headers here: it is force-included ahead of the game's own
 * Psy-Q headers too, which define size_t / wchar_t their own way. */
int pc_win_rename(const char *from, const char *to);
int _commit(int fd);
/* bare arguments: the macro also rewrites stdio.h's own declaration */
#define rename(from, to) pc_win_rename(from, to)
#define fsync(fd) _commit(fd)
#endif
