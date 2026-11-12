/* SPDX-License-Identifier: GPL-3.0-or-later */
/* audio_scene.c — Scene-driven BGM routing for the PC port.
 *
 * NOT A RETAIL PATH; OFF BY DEFAULT since 2026-10-30.  Set
 * PC_SCENE_MUSIC=1 to hear it.
 *
 * On PS1, music is requested by the game's own sound driver: the matched
 * SD_* command queue loads a SEQ table and a VAB from the disc through the
 * CD queue, then calls libsnd SsSeqOpen / SsSeqSetVol / SsSeqPlay with the
 * RAM addresses it loaded.  This file instead watched now_step and started
 * a SEQ file chosen by a hand-written table, independent of any request
 * the game made.  Measured against the reference emulator (Interpreter):
 *   - scene 64 (AV parity, navigated at the first title entry): retail
 *     PCM is silent for all 1,200 VBlanks; the table started ROMEO.SEQ on
 *     entry (native RMS ~250, peak 1,143).
 *   - title (step 18, no-input boot): retail's driver calls
 *     SsSeqOpen(0x8002E850, vab 2) and SsSeqPlay(0, 1, 1) at host frame
 *     1528 on the SEQ table read from file 0x25 (to 0x8002E800) with the VAB
 *     body from file 0x50 (0x801B8000); the table started DAYS.SEQ at step
 *     entry, ~950 frames earlier and a different sequence.
 * That is PC-only audio the game never requested (PC-port policy: PS1 is
 * the source of truth), so the default build no longer runs it.  The
 * faithful replacement serves the game's own libsnd requests from the
 * SEQ/VAB bytes it loaded: since 2026-10-30 the matched libsnd plays them
 * (pc/hal/spu.c, docs/pc-port/LIBSND-MUSIC.md).
 *
 * When enabled it runs once per frame from the main loop.  When the scene
 * id changes, it stops the current SEQ, loads the mapped one, and starts it
 * looping.
 */
#include "types.h"
#include "pc_data_root.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From spu.c */
extern void audio_seq_stop(void);   /* pc/hal/spu.c: the PC-only SEQ player */
extern void audio_seq_load_and_play(const char *path, int loop, s16 l_vol, s16 r_vol);
extern int  audio_xa_start(const char *path);
extern void audio_xa_stop(void);
extern void debug_log(const char *fmt, ...);

/* Active girl id — picked by the player at new-game and by the date flow
 * before a confession scene. On PS1 the sound code picks the XA track
 * from this byte. The canonical 0..12 ordering follows the bustup file
 * directory on the disc; entries are the XA filenames present in
 * pc/assets/audio/. Unknown / out-of-range ids fall back to FUJISAKI
 * (the main heroine's theme — safe default). */
extern u8 now_month;

static const char *k_girl_xa[] = {
    "FUJISAKI.XA",  /*  0: Shiori Fujisaki (main heroine) */
    "KAGAMI.XA",    /*  1: Mira Kagami */
    "NIJINO.XA",    /*  2: Saki Nijino */
    "ASAHINA.XA",   /*  3: Yuko Asahina */
    "KATAGIRI.XA",  /*  4: Ayako Katagiri */
    "KIYOKAWA.XA",  /*  5: Hiromi Kiyokawa */
    "MIKIHARA.XA",  /*  6: Megumi Mikihara */
    "KISARAGI.XA",  /*  7: Miharu Kisaragi */
    "YUMI.XA",      /*  8: Yumi Saotome */
    "KOSHIKI.XA",   /*  9: Sayo Koshiki */
    "HIMOO.XA",     /* 10: Yuina Himoo */
    "YOSHIO.XA",    /* 11: Yoshio Saotome (male friend — date arc) */
    "ETC.XA",       /* 12: misc / unused slot */
};
#define GIRL_XA_COUNT (sizeof(k_girl_xa) / sizeof(k_girl_xa[0]))

/* Resolve the XA filename for the current active girl. Used by scenes
 * 32-34 (dates) instead of hardcoding FUJISAKI.XA. */
static const char *girl_xa_filename(void) {
    u8 id = now_month;
    if (id >= GIRL_XA_COUNT) {
        return k_girl_xa[0];
    }
    return k_girl_xa[id];
}

/* Track type: SEQ (VAB/TSF synth) or XA (streamed ADPCM). */
#define TRACK_SEQ 0
#define TRACK_XA  1

/* Scene → track mapping. A NULL entry means "no music — keep the last
 * track playing" (useful for short transition scenes where cutting would
 * be jarring). An empty-string entry means "stop music". */
typedef struct {
    u8          scene;
    const char *seq;       /* filename, NULL=keep, ""=stop */
    s16         l_vol;
    s16         r_vol;
    int         loop;
    int         type;      /* TRACK_SEQ or TRACK_XA */
} SceneMusic;

static const SceneMusic k_scene_music[] = {
    /* Title screen family — Tokimeki Memorial main theme.
     * Was VISUAL.SEQ which played a horn fanfare (wrong). The PSX
     * title BGM is the main "love-theme" melody. ROMEO.SEQ matches
     * the canonical title music style. */
    { 64, "ROMEO.SEQ",    96,  96, 1, TRACK_SEQ },
    { 65, "ROMEO.SEQ",    96,  96, 1, TRACK_SEQ },
    { 66, "ROMEO.SEQ",    96,  96, 1, TRACK_SEQ },
    /* Brief intermediate scenes — keep whatever was last playing so
     * title ↔ continue transitions don't thrash the music loader. */
    {  0, NULL,            0,   0, 0, TRACK_SEQ },
    { 16, NULL,            0,   0, 0, TRACK_SEQ },
    { 49, NULL,            0,   0, 0, TRACK_SEQ },
    { 50, NULL,            0,   0, 0, TRACK_SEQ },
    /* Scene 48: "STARTING NEW GAME" splash → name entry */
    { 48, NULL,            0,   0, 0, TRACK_SEQ },
    /* Scene 1: name entry overlay */
    {  1, "VISUAL.SEQ",   80,  80, 1, TRACK_SEQ },
    /* Scene 17-21: weekly schedule / daily life (GEKO).
     * ASA.SEQ / DAYS.SEQ are short SEQ tracks, not streamed BGM. */
    { 17, "ASA.SEQ",     112, 112, 1, TRACK_SEQ },
    { 18, "DAYS.SEQ",    112, 112, 1, TRACK_SEQ },
    { 19, "DAYS.SEQ",    112, 112, 1, TRACK_SEQ },
    { 20, "DAYS.SEQ",    112, 112, 1, TRACK_SEQ },
    { 21, "DAYS.SEQ",    112, 112, 1, TRACK_SEQ },
    /* Date scenes — XA-streamed character themes. Filename is resolved
     * at tick time from now_month via girl_xa_filename(); the string
     * here is only a fallback if the lookup path is skipped. */
    { 32, NULL /*girl-routed*/, 127, 127, 1, TRACK_XA },
    { 33, NULL /*girl-routed*/, 127, 127, 1, TRACK_XA },
    { 34, NULL /*girl-routed*/, 127, 127, 1, TRACK_XA },
    /* Special events */
    { 35, "SHOGATU.SEQ", 112, 112, 1, TRACK_SEQ },  /* New Year */
    { 36, "NIJI.SEQ",    112, 112, 1, TRACK_SEQ },  /* Valentine-ish fallback */
    { 67, "DAYS.SEQ",    112, 112, 1, TRACK_SEQ },  /* Continue flow */
    /* Endings */
    {144, "LASTDATE.SEQ",127, 127, 0, TRACK_SEQ },
    {145, "LASTDATE.SEQ",127, 127, 0, TRACK_SEQ },
    {255, "__END__",      0,   0, 0, TRACK_SEQ }, /* sentinel */
};

static const char *asset_dir_prefix(void) {
    /* Single source of truth for where SEQs live. Match spu.c default. */
    static char p[1100];
    return pc_asset_path(p, sizeof p, "raw/CDROM/VAB_SEQ/");
}

static const char *xa_dir_prefix(void) {
    /* Single source of truth for where XA streams live. */
    static char p[1100];
    return pc_asset_path(p, sizeof p, "audio/");
}

static const SceneMusic *lookup_scene_music(u8 scene) {
    const SceneMusic *m;
    for (m = k_scene_music; m->scene != 255; m++) {
        if (m->scene == scene) return m;
    }
    return NULL;
}

/* Called once per frame from main.c. Detects scene transitions and
 * drives the SEQ loader. */
/* PC_SCENE_MUSIC=1 opts in to the non-retail table (see the header). */
static int scene_music_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PC_SCENE_MUSIC");
        enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return enabled;
}

void audio_scene_tick(u8 current_scene) {
    static int initialized = 0;
    static u8  last_scene = 255;
    static char last_path[256] = {0};
    const SceneMusic *m;
    char path[256];

    if (!scene_music_enabled()) return;
    if (!initialized) {
        initialized = 1;
        last_scene = current_scene;
        /* Fall through to route the initial scene */
    } else if (current_scene == last_scene) {
        return;
    }
    last_scene = current_scene;

    m = lookup_scene_music(current_scene);
    if (!m) {
        debug_log("audio_scene: scene %d has no mapping, leaving music",
                  current_scene);
        return;
    }

    if (m->seq == NULL) {
        /* NULL has two meanings here, distinguished by track type:
         *   TRACK_SEQ + NULL → "keep last track" (transition scenes)
         *   TRACK_XA  + NULL → "girl-routed" (pick XA by now_month) */
        if (m->type != TRACK_XA) {
            return;
        }
        /* Fall through with a resolved XA filename below. */
    }

    if (m->seq && m->seq[0] == '\0') {
        /* Explicit stop */
        audio_seq_stop();
        audio_xa_stop();
        last_path[0] = '\0';
        debug_log("audio_scene: scene %d → stop", current_scene);
        return;
    }

    {
        const char *fname = m->seq;
        if (!fname && m->type == TRACK_XA) {
            fname = girl_xa_filename();
        }
        snprintf(path, sizeof(path), "%s%s",
                 m->type == TRACK_XA ? xa_dir_prefix() : asset_dir_prefix(),
                 fname);
    }
    if (strcmp(path, last_path) == 0) {
        return;  /* already playing the right track */
    }
    strncpy(last_path, path, sizeof(last_path) - 1);

    debug_log("audio_scene: scene %d → %s (%s, vol=%d/%d, loop=%d)",
              current_scene, path,
              m->type == TRACK_XA ? "XA" : "SEQ",
              m->l_vol, m->r_vol, m->loop);

    if (m->type == TRACK_XA) {
        /* Switch from SEQ path to XA stream: stop any running SEQ first
         * so we aren't mixing a leftover loop over the character theme. */
        audio_seq_stop();
        audio_xa_start(path);
    } else {
        /* Switch from XA back to SEQ: stop the stream so the two paths
         * don't overlap in the mix callback. */
        audio_xa_stop();
        audio_seq_load_and_play(path, m->loop, m->l_vol, m->r_vol);
    }
}

/* SE (sound effect) dispatch — called from input hooks. Maps a logical
 * SE id to an on-disk SEQ file from the assets. For now these are fired
 * as one-shot SEQs on slot 1; the TSF renderer handles mixing. Only
 * the menu-click effect is wired for now; more can be added without
 * touching callers. */
static const char *k_se_seq[] = {
    [0] = NULL,            /* SE_NONE */
    [1] = "HIMO.SEQ",      /* SE_MENU_CLICK (placeholder: short tone) */
    [2] = "KATA.SEQ",      /* SE_CONFIRM   */
    [3] = "KISA.SEQ",      /* SE_CANCEL    */
};
#define SE_COUNT (sizeof(k_se_seq) / sizeof(k_se_seq[0]))

void audio_scene_se(int se_id) {
    const char *name;
    char path[256];
    if (se_id <= 0 || (unsigned)se_id >= SE_COUNT) return;
    name = k_se_seq[se_id];
    if (!name) return;
    snprintf(path, sizeof(path), "%s%s", asset_dir_prefix(), name);
    /* SE is a one-shot: re-use SEQ slot 0 if it's idle, otherwise skip.
     * We deliberately don't preempt running music — SFX on PS1 is
     * mixed on separate voices. A full mix would need a second SEQ
     * slot in spu.c; noted for a follow-up. */
    (void)path;
    debug_log("audio_scene: SE %d (%s) — deferred (no SE slot yet)",
              se_id, name);
}
