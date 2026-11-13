/* state_sign.c -- the signature a save state (.tks) carries, so a load
 * accepts only a state this installation wrote.
 *
 * A state is a machine snapshot: its load writes the native globals
 * (function pointers among them), PS1 RAM and the game stack (return
 * addresses) straight from the file (pc/hal/dbg_state.c).  A state from
 * anyone else is therefore code, not data.  Each install keeps a random
 * 32-byte key in <game data>/.state.key (pc_asset_dir(): <root>/data for a
 * player, beside the disc data and the log, never in saves/, the folder
 * players share; ./pc/assets for harness runs, so they never touch a
 * player's folder; a dot name, which the disc setup counts as its own
 * file, as_owned_entry); dbg_state_write appends
 *     "TKSIGv01" + HMAC-SHA256(key, every byte before the trailer)
 * and dbg_state_load checks it before it parses a byte of the file.  A
 * shared quicksave therefore does not load on another computer; the
 * memory card (saves/card1.mcr), which the game parses as data, is what
 * players share.  PC_LOADSTATE_FORCE=1 (developer runs) skips the check.
 *
 * The key itself is never in a state: dbg_state.c lists this file in
 * KEEP_FILES, and every stack copy is wiped after use.
 *
 * The key is made once, from the system's random source, written to a
 * private temporary file and renamed into place without replacing (two
 * processes starting at once agree on whichever key landed first).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pc_fs.h"
#include "pc_hash.h"
#include "pc_data_root.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
/* RtlGenRandom (advapi32): the CRT's rand_s source */
BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG len);
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#include "state_sign.h"

/* a wipe the compiler cannot drop as a dead store */
static void wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static int random_bytes(uint8_t *out, size_t n) {
#ifdef _WIN32
    return SystemFunction036(out, (ULONG)n) ? 0 : -1;
#else
    size_t got = 0;
#if defined(__linux__)
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        got += (size_t)r;
    }
    if (got == n) return 0;
#endif
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f) return -1;
        got = fread(out, 1, n, f);
        fclose(f);
        return got == n ? 0 : -1;
    }
#endif
}

static void key_path(char *out, size_t n) { snprintf(out, n, "%s/.state.key", pc_asset_dir()); }

static int read_key(const char *path, uint8_t key[STATE_KEY_LEN]) {
    uint8_t extra;
    FILE *f = pc_fs_fopen(path, "rb");
    int ok;
    if (!f) return -1;
    ok = fread(key, 1, STATE_KEY_LEN, f) == STATE_KEY_LEN && fread(&extra, 1, 1, f) == 0;
    fclose(f);
    return ok ? 0 : -1;
}

static int g_have_key = 0;
static uint8_t g_key[STATE_KEY_LEN];

int state_key(uint8_t key[STATE_KEY_LEN]) {
    char path[PC_ROOT_PATH_MAX + 32], tmp[PC_ROOT_PATH_MAX + 64];
    uint8_t fresh[STATE_KEY_LEN];
    FILE *f;
    int ok;
    if (g_have_key) { memcpy(key, g_key, STATE_KEY_LEN); return 0; }
    key_path(path, sizeof path);
    if (read_key(path, g_key) == 0) goto have;
    if (random_bytes(fresh, sizeof fresh)) return -1;
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
#endif
    pc_fs_unlink(tmp);
#ifdef _WIN32
    f = pc_fs_create_excl(tmp);
#else
    {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        f = fd < 0 ? NULL : fdopen(fd, "wb");
        if (!f && fd >= 0) close(fd);
    }
#endif
    if (!f) return -1;
    ok = fwrite(fresh, 1, sizeof fresh, f) == sizeof fresh;
    ok = (fclose(f) == 0) && ok;
    /* never replaces: a key another process put there first wins
     * (MoveFileEx without REPLACE_EXISTING; link(2), atomic, on POSIX) */
#ifdef _WIN32
    if (ok) pc_fs_rename(tmp, path);
#else
    if (ok) pc_fs_link(tmp, path);
#endif
    pc_fs_unlink(tmp);
    wipe(fresh, sizeof fresh);
    if (read_key(path, g_key)) return -1;
have:
    g_have_key = 1;
    memcpy(key, g_key, STATE_KEY_LEN);
    return 0;
}

void state_hmac(const uint8_t key[STATE_KEY_LEN], const void *data, size_t n,
                uint8_t out[STATE_MAC_LEN]) {
    uint8_t pad[64], inner[32];
    PcSha256 c;
    int i;
    memset(pad, 0, sizeof pad);
    memcpy(pad, key, STATE_KEY_LEN);
    for (i = 0; i < 64; i++) pad[i] ^= 0x36;
    pc_sha256_init(&c);
    pc_sha256_update(&c, pad, 64);
    pc_sha256_update(&c, data, n);
    pc_sha256_final(&c, inner);
    for (i = 0; i < 64; i++) pad[i] ^= 0x36 ^ 0x5c;
    pc_sha256_init(&c);
    pc_sha256_update(&c, pad, 64);
    pc_sha256_update(&c, inner, sizeof inner);
    pc_sha256_final(&c, out);
    wipe(pad, sizeof pad);
    wipe(inner, sizeof inner);
    wipe(&c, sizeof c);
}

int state_sign_trailer(const void *data, size_t n, uint8_t trailer[STATE_TRAILER_LEN]) {
    uint8_t key[STATE_KEY_LEN];
    if (state_key(key)) return -1;
    memcpy(trailer, STATE_SIG_MAGIC, 8);
    state_hmac(key, data, n, trailer + 8);
    wipe(key, sizeof key);
    return 0;
}

int state_verify(const unsigned char *file, unsigned long n, unsigned long *body) {
    uint8_t key[STATE_KEY_LEN], mac[STATE_MAC_LEN];
    unsigned diff = 0;
    int i;
    *body = n;
    if (n < STATE_TRAILER_LEN) return -1;
    if (memcmp(file + n - STATE_TRAILER_LEN, STATE_SIG_MAGIC, 8)) return -1;
    if (state_key(key)) return -2;
    state_hmac(key, file, n - STATE_TRAILER_LEN, mac);
    wipe(key, sizeof key);
    for (i = 0; i < STATE_MAC_LEN; i++) diff |= mac[i] ^ file[n - STATE_MAC_LEN + i];
    if (diff) return -1;
    *body = n - STATE_TRAILER_LEN;
    return 0;
}
