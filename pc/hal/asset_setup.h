/* SPDX-License-Identifier: GPL-3.0-or-later */
/* asset_setup.h -- first-run asset preparation inside the executable.
 *
 * A C port of pc/tools/disc_image.py + pc/tools/bootstrap_assets.py: read the
 * player's own raw BIN/CUE of the supported disc, verify it against the
 * pinned profile, extract every ISO9660 file (XA/STR sectors with their
 * subheaders, the external CD-DA extent from Track 2) and write the same
 * tree and the same assets.manifest.json, byte for byte, as the Python tool
 * (equivalence test: pc/tools/test_asset_setup_c.py).
 *
 * No SDL here: the setup screen (pc/hal/setup_screen.c) runs it on a worker
 * thread and reads the progress; the command line (--prepare-assets,
 * --verify-assets) runs it directly.  All paths are UTF-8 (pc/hal/pc_fs.h).
 *
 * Beyond the Python tool, and never changing what it writes for an input
 * the Python tool accepts:
 *   - a single-file BIN/CUE (both tracks in one .bin) is read through its
 *     CUE track offsets;
 *   - without Track 2 the setup can continue (allow_no_audio): Track 2 holds
 *     only the CD-DA extent CDROM/DADIR/TR02.DA, which the game never plays.
 *     The manifest then lists it under
 *     "omitted_files"; the Python tool does not accept such a tree;
 *   - each failure carries a code the setup screen turns into advice
 *     (wrong revision, cooked ISO, CHD/ECM/PBP, missing Track 2, space...).
 */
#ifndef ASSET_SETUP_H
#define ASSET_SETUP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AS_SECTOR 2352
#define AS_MANIFEST "assets.manifest.json"
#define AS_RELEASE_NAME "Tokimeki Memorial: Forever with You (Japan) (Rev 1)"
#define AS_SERIAL "SLPS-00065"
#define AS_CDDA_NAME "CDROM/DADIR/TR02.DA"

typedef struct {
    int64_t track_size;
    char track_sha1[41];
    int64_t exe_lba;
    int64_t exe_size;
    char exe_sha1[41];
    int64_t file_count;
    int64_t audio_size;
    char audio_sha1[41];
    int64_t audio_skip;
    int64_t cdda_lba;
    int64_t cdda_size;
    int64_t preserved_subheader_lba;
    /* SHA-256 (hex) of the managed-file list an extraction of this disc
     * writes into its manifest: every row but the optional Track 2 file
     * (TR02.DA, raw/ and its alias), sorted by path, one
     * "path\tsize\tsha256\n" line each.  The disc and the extraction are
     * fixed, so the list is too: a manifest whose rows do not hash to this
     * was not written by the setup (as_verify).  "" skips the check (the
     * test fixtures' profiles). */
    char files_digest[65];
} AsProfile;

extern const AsProfile as_supported_profile;

enum {
    AS_OK = 0,
    AS_E_NOT_FOUND,         /* the image (or a file the CUE names) is missing */
    AS_E_UNREADABLE,        /* permission / I/O error reading the image */
    AS_E_CUE,               /* the CUE sheet is not one we can use */
    AS_E_COOKED_ISO,        /* 2048-byte sectors: XA subheaders are gone */
    AS_E_FORMAT,            /* CHD / ECM / PBP / an archive: not supported */
    AS_E_AUDIO_TRACK_GIVEN, /* the image given is Track 2, not Track 1 */
    AS_E_WRONG_SIZE,        /* Track 1 has another size: another release */
    AS_E_WRONG_SHA1,        /* Track 1 SHA1 differs: other revision/damaged */
    AS_E_NO_TRACK2,         /* Track 2 is missing (may continue without) */
    AS_E_TRACK2,            /* Track 2 exists but differs */
    AS_E_LAYOUT,            /* the data track's contents fail a check */
    AS_E_DISK_SPACE,        /* not enough free space for the extraction */
    AS_E_WRITE,             /* cannot create/write the destination */
    AS_E_DEST_INVALID,      /* the destination holds unmanaged/damaged files */
    AS_E_LOCKED,            /* another setup holds the lock */
    AS_E_CHANGED,           /* the image changed while being read */
    AS_E_CANCELLED,
    AS_E_MANIFEST,          /* verify: manifest missing / bad */
    AS_E_CORRUPT,           /* verify: a file is missing / has the wrong size or hash */
};

typedef struct {
    int code;
    char message[768];      /* technical detail (the Python tool's wording) */
    char found[72];         /* AS_E_WRONG_SHA1 / AS_E_TRACK2 / AS_E_WRONG_SIZE */
    char expected[72];
    char path[1024];        /* the file concerned, when there is one */
} AsError;

enum { AS_PH_IDLE, AS_PH_HASH, AS_PH_HASH_AUDIO, AS_PH_CHECK, AS_PH_EXTRACT, AS_PH_VERIFY, AS_PH_DONE };

/* Progress and control, shared with a UI thread.  `done`/`total` are bytes
 * over the whole run; plain ints written by the worker, read by the UI. */
typedef struct {
    volatile int cancel;            /* set by the UI: stop, clean up */
    volatile int phase;             /* AS_PH_* */
    volatile int64_t done, total;
    int allow_no_audio;             /* 1: continue when Track 2 is missing; 2: also
                                       when it is present but differs */
    int fast_verify;                /* as_verify: sizes and structure, and the contents of
                                     * every file but the streams (as_stream_file) */
    /* results */
    int files;                      /* managed files in the manifest */
    int anomalies;                  /* preserved source anomalies */
    int audio_omitted;              /* Track 2 was missing and skipped */
    char track_sha1[41], audio_sha1[41], exe_sha1[41];   /* as found ("" = not read) */
    char track1_path[1024], audio_path[1024];            /* the files actually read */
} AsCtl;

/* Python's prepare_assets(image, destination, profile, audio_track):
 * image may be NULL when the destination already holds a valid cache.
 * audio_track (a Track 2 BIN for a BIN image) may be NULL. */
int as_prepare(const char *image, const char *audio_track, const char *dest,
               const AsProfile *profile, AsCtl *ctl, AsError *err);

/* Python's verify_cache: the full check hashes every managed file;
 * ctl->fast_verify checks the manifest (its rows against the profile's
 * files_digest), the inventory, every file's presence and size, the
 * contents of every file but the audio/video streams (~120 MB, about a
 * third of a second), and the executable's SHA1 (fast enough for every
 * launch).  ctl may be NULL (full). */
int as_verify(const char *dest, const AsProfile *profile, AsCtl *ctl, AsError *err);
/* 1 for the audio/video streams (.XA, .STR, .DA): read only by the HAL's
 * bounded stream decoders, never as data the game code walks; the fast
 * check leaves their contents to the full one. */
int as_stream_file(const char *path);

/* Only open the image and run the identity gate (sizes, SHA1s, filesystem
 * inventory, executable): what the setup does before extracting. */
int as_check_image(const char *image, const char *audio_track, const AsProfile *profile,
                   AsCtl *ctl, AsError *err);

/* Cheap, no hashing: does this file look like the supported disc?  A .cue
 * whose Track 1 file has the profile's size, or a .bin of that size.
 * Returns 1 for a candidate. */
int as_quick_candidate(const char *path, const AsProfile *profile);

/* The FILE names a CUE sheet lists, as written (up to max); -1 when the
 * CUE cannot be read.  For "the .cue expects these files" messages. */
int as_cue_files(const char *cue, char names[][256], int max);

/* safe_relative(): 0 when the ISO path is acceptable. */
int as_safe_relative(const char *name);

/* The command-line front end (tokimeki-pc --prepare-assets / --verify-assets,
 * and the standalone test build).  Returns the process exit status. */
int as_cli(int argc, char **argv);

/* The destination of as_prepare: AS_DEST_EMPTY (missing, or holding only
 * the program's own files: as_owned_entry), AS_DEST_DATA (has the
 * manifest), AS_DEST_OTHER (anything else; `other` names one entry). */
enum { AS_DEST_EMPTY, AS_DEST_DATA, AS_DEST_OTHER };
int as_dest_status(const char *dest, char *other, size_t on);
/* A name the program keeps in the game-data folder besides the extracted
 * disc: the log (tokimeki-pc.log, .log.old), the English files (en/) and
 * hidden work files (a leading '.'). */
int as_owned_entry(const char *name);

#ifdef __cplusplus
}
#endif
#endif
