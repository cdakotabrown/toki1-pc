/* SPDX-License-Identifier: GPL-3.0-or-later */
/* spu_quality.c -- enhanced sound quality: the SPU's interpolation and
 * reverb precision, and the audio device's rate (docs/pc-port/ENHANCED-MODE.md
 * "Sound quality"; PLAYER-GUIDE.md "Sound quality").
 *
 * Settings, never game state.  [audio] interpolation (gaussian | cubic |
 * sinc), reverb (original | hq) and output_rate (44100 | 48000), or
 * TOKI_SPU_INTERP / TOKI_SPU_REVERB / TOKI_AUDIO_RATE, under the SPU_QUALITY
 * switch (enhanced on, parity off).  Under parity, or with every setting at
 * its console value, nothing is called: the core computes exactly what it
 * did before.  The interpolators and the floating-point reverb are the SPU
 * core's patch (pc/3rd/SPU-CORE.md "Local patches"); they change the
 * output only -- voices, envelopes, ENDX, the capture buffers, SPU RAM and
 * the real reverb unit are the same.  The rate is pc/hal/audio_resample.c,
 * read once when the device opens.
 */
#include "types.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "profile.h"
#include "pc_settings.h"
#include "pc_menu.h"
#include "spu_psx.h"

extern const PcSettings *pc_settings_get(void) __attribute__((weak));
extern PcSettings *pc_settings_mut(void) __attribute__((weak));
extern const char *hal_setting_str(const char *name, const char *dflt) __attribute__((weak));
extern void spu_hal_audio_lock(void);
extern void spu_hal_audio_unlock(void);

static const char *const interp_names[] = { "gaussian", "cubic", "sinc", NULL };
static const char *const reverb_names[] = { "original", "hq", NULL };

/* env (TOKI_*) by name or number, else the settings value */
static int pick(const char *env, const char *const *names, int dflt) {
    const char *v = hal_setting_str ? hal_setting_str(env, NULL) : getenv(env);
    int i;
    if (!v || !*v) return dflt;
    for (i = 0; names[i]; i++)
        if (!strcasecmp(v, names[i])) return i;
    return atoi(v);
}

static int on(void) { return hal_model_on("SPU_QUALITY"); }

int spu_quality_interp(void) {
    const PcSettings *s = pc_settings_get ? pc_settings_get() : NULL;
    int v;
    if (!on()) return PC_INTERP_GAUSSIAN;
    v = pick("TOKI_SPU_INTERP", interp_names, s ? s->interpolation : 0);
    return v >= PC_INTERP_GAUSSIAN && v <= PC_INTERP_SINC ? v : PC_INTERP_GAUSSIAN;
}

int spu_quality_reverb(void) {
    const PcSettings *s = pc_settings_get ? pc_settings_get() : NULL;
    if (!on()) return PC_REVERB_ORIGINAL;
    return pick("TOKI_SPU_REVERB", reverb_names, s ? s->reverb_quality : 0) == PC_REVERB_HQ;
}

/* The device's rate: 44100, or 48000 (enhanced, asked for). */
int spu_quality_output_rate(void) {
    const PcSettings *s = pc_settings_get ? pc_settings_get() : NULL;
    const char *v = hal_setting_str ? hal_setting_str("TOKI_AUDIO_RATE", NULL) : getenv("TOKI_AUDIO_RATE");
    if (!on()) return 44100;
    if (v && *v) return atoi(v) == 48000 ? 48000 : 44100;
    return s && s->output_rate == PC_RATE_48000 ? 48000 : 44100;
}

/* Game thread, once per VBlank (pc/hal/spu.c hal_snd_tick). */
void spu_quality_tick(void) {
    static int cur_interp, cur_reverb;
    int interp = spu_quality_interp(), reverb = spu_quality_reverb();
    if (interp == cur_interp && reverb == cur_reverb) return;
    if (interp == PC_INTERP_SINC) spu_psx_prepare_quality(PC_INTERP_SINC);
    spu_hal_audio_lock();
    spu_psx_set_quality(interp, reverb);
    spu_hal_audio_unlock();
    printf("[SPU] sound quality: interpolation %s, reverb %s\n", interp_names[interp], reverb_names[reverb]);
    fflush(stdout);
    cur_interp = interp;
    cur_reverb = reverb;
}

/* ---- the settings menu (a page of its own, from music_pack.c) ---------- */

static const char *const interp_choice[] = { "GAUSSIAN (CONSOLE)", "CUBIC", "SINC", NULL };
static const char *const reverb_choice[] = { "ORIGINAL", "HIGH PRECISION", NULL };
static const char *const rate_choice[] = { "44100 HZ (CONSOLE)", "48000 HZ", NULL };

static int get_field(void *ctx) {
    const PcSettings *s = pc_settings_get ? pc_settings_get() : NULL;
    return s ? *(const int *)((const char *)s + (size_t)ctx) : 0;
}
static void set_field(void *ctx, int v) {
    PcSettings *s = pc_settings_mut ? pc_settings_mut() : NULL;
    if (s) *(int *)((char *)s + (size_t)ctx) = v;
}
static const char *quality_note(void *ctx) {
    (void)ctx;
    return on() ? "" : "ENHANCED ONLY";
}

static void add(int page, const char *label, const char *key, const char *const *choices, size_t field,
                unsigned flags) {
    PcMenuItem it;
    memset(&it, 0, sizeof it);
    it.page = page;
    it.type = PC_MENU_CHOICE;
    it.label = label;
    it.key = key;
    it.choices = choices;
    it.flags = flags;
    it.ctx = (void *)field;
    it.get = get_field;
    it.set = set_field;
    it.note = quality_note;
    if (pc_menu_add(&it) < 0) printf("[SPU] the settings menu is full: no %s entry\n", label);
}

void spu_quality_menu_register(void) {
    static int done;
    PcMenuItem it;
    int page;
    if (done) return;
    done = 1;
    page = pc_menu_page("SOUND QUALITY");
    if (page < 0) return;
    memset(&it, 0, sizeof it);
    it.type = PC_MENU_PAGE;
    it.label = "SOUND QUALITY";
    it.target = page;
    pc_menu_add(&it);
    add(page, "INTERPOLATION", "audio.interpolation", interp_choice, offsetof(PcSettings, interpolation), 0);
    add(page, "REVERB", "audio.reverb", reverb_choice, offsetof(PcSettings, reverb_quality), 0);
    add(page, "OUTPUT RATE", "audio.output_rate", rate_choice, offsetof(PcSettings, output_rate), PC_MENU_RESTART);
}
