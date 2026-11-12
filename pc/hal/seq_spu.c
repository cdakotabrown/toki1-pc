/* SPDX-License-Identifier: GPL-3.0-or-later */
/* seq_spu.c -- PS1 SEQ driver for the SPU core.
 *
 * Parses PS1 'pQES' sequence data and drives SPU voice registers
 * directly via spu_psx_write_reg(). Replaces the TinySoundFont path.
 *
 * MIDI event ordering: event byte(s) come FIRST, delta-time SECOND.
 * This is the PS1 SEQ convention (opposite of Standard MIDI files).
 *
 * Internal clock is measured in audio sample-frames (44100 Hz). Each
 * call to seq_spu_advance(n) advances the clock by n samples and pops
 * every event whose scheduled sample-tick has been reached.
 */
#include "seq_spu.h"
#include "spu_psx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SAMPLE_RATE 44100
#define NUM_VOICES  24

/* SPU register offsets within a voice block (voice base = 0x1F801C00 + v*0x10) */
#define SPU_VOICE_BASE   0x1F801C00u
#define V_VOLL           0x00
#define V_VOLR           0x02
#define V_PITCH          0x04
#define V_START_ADDR     0x06
#define V_ADSR1          0x08
#define V_ADSR2          0x0A
#define V_LOOP_ADDR      0x0E

/* Global SPU regs */
#define SPU_KEY_ON_LO    0x1F801D88u
#define SPU_KEY_ON_HI    0x1F801D8Au
#define SPU_KEY_OFF_LO   0x1F801D8Cu
#define SPU_KEY_OFF_HI   0x1F801D8Eu
#define SPU_MAIN_VOL_L   0x1F801D80u
#define SPU_MAIN_VOL_R   0x1F801D82u

/* Optional hook so tests can record SPU register writes without touching
 * the real SPU core. When set, the hook is invoked INSTEAD of
 * spu_psx_write_reg. Default NULL -> direct writes. */
static void (*g_reg_write_hook)(u32 addr, u16 value) = NULL;

/* Public (weakly; only used by test harness) setter. Not declared in
 * the header since it isn't part of the shared API spec. */
void seq_spu__set_reg_hook(void (*hook)(u32 addr, u16 value));
void seq_spu__set_reg_hook(void (*hook)(u32 addr, u16 value)) {
    g_reg_write_hook = hook;
}

static void write_reg(u32 addr, u16 value) {
    if (g_reg_write_hook) g_reg_write_hook(addr, value);
    else spu_psx_write_reg(addr, value);
}

/* -------- Voice allocator -------- */
typedef struct {
    u8  active;
    u8  program;
    u8  channel;
    u8  note;
    u32 alloc_tick;   /* sample-clock at note-on (for LRU voice-steal) */
} VoiceState;

/* -------- Per-channel MIDI state -------- */
typedef struct {
    u8  program;
    u8  volume;     /* CC7, default 100 */
    u8  pan;        /* CC10, default 64 */
    s16 pitch_bend; /* -8192..+8191, 0 = center */
} ChannelState;

struct SeqSpuState {
    const u8 *data;
    u32       size;
    u32       pos;
    u32       loop_start_pos;
    int       loop;
    int       playing;
    u8        running_status;

    u32       ppqn;
    u32       tempo_us_per_qn;
    u32       samples_per_tick;

    /* Timing. Next event is scheduled at next_event_sample; sample_clock
     * advances via seq_spu_advance(). sample_tick_frac carries fractional
     * ticks so we don't lose precision on delta-time conversion. */
    u64       sample_clock;
    u64       next_event_sample;

    u8        master_vol;  /* 0..127 */

    const VabBank *bank;
    VoiceState     voices[NUM_VOICES];
    ChannelState   channels[16];
};

/* ---------- VLQ reader ---------- */
static u32 read_vlq(SeqSpuState *s) {
    u32 value = 0;
    u8 byte;
    do {
        if (s->pos >= s->size) return 0;
        byte = s->data[s->pos++];
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    return value;
}

/* ---------- Header parsing ---------- */
/* Finds 'pQES' magic and seeds ppqn/tempo/pos. Returns 1 on success. */
static int parse_header(SeqSpuState *s) {
    u32 i, hdr;

    if (s->size < 16) return 0;

    for (i = 0; i + 4 < s->size; i++) {
        if (s->data[i] == 'p' && s->data[i+1] == 'Q' &&
            s->data[i+2] == 'E' && s->data[i+3] == 'S') {
            break;
        }
    }
    if (i + 4 >= s->size) {
        /* No pQES found; fall back to legacy @ format if possible */
        if (s->data[0] == 0x40) {
            s->ppqn = ((u32)s->data[0] << 8) | s->data[1];
            s->tempo_us_per_qn = 500000;
            s->pos = 2;
        } else {
            return 0;
        }
    } else {
        /* pQES + 4-byte version + 2-byte ppqn (BE) + 3-byte tempo (BE)
         * + 2-byte rhythm. Data starts at magic + 4 + 4 + 2 + 3 + 2 = +15. */
        hdr = i + 4;
        if (hdr + 11 > s->size) return 0;
        /* version is 4 bytes */
        s->ppqn = ((u32)s->data[hdr+4] << 8) | s->data[hdr+5];
        s->tempo_us_per_qn = ((u32)s->data[hdr+6] << 16) |
                             ((u32)s->data[hdr+7] << 8)  |
                                   s->data[hdr+8];
        /* Data starts at hdr+12 (seq_start+16). The layout past the
         * 'pQES' magic is: version(4) + ppqn(2 BE) + tempo(3 BE) +
         * rhythm(2) + padding(1) = 12 bytes. Verified against
         * VISUAL.SEQ where the first event 0xC0 appears at seq_start+16. */
        s->pos = hdr + 12;
    }

    if (s->ppqn == 0) s->ppqn = 480;
    if (s->tempo_us_per_qn == 0) s->tempo_us_per_qn = 500000;

    s->samples_per_tick = (u32)(((u64)SAMPLE_RATE * (u64)s->tempo_us_per_qn)
                                / ((u64)s->ppqn * 1000000ULL));
    if (s->samples_per_tick == 0) s->samples_per_tick = 1;

    s->loop_start_pos = s->pos;
    return 1;
}

/* ---------- Pitch calculation ----------
 *
 * SPU pitch register: 0x1000 = "play sample at its native recorded rate".
 * For a tone with center_note C and pitch_fine F (signed, 1 unit = 1/128
 * of a semitone in the Sony convention), playing MIDI note N, pitch-bend
 * B in semitones:
 *   ratio = 2 ^ ((N - C + F/128 + B) / 12)
 *   pitch_reg = round(ratio * 0x1000), clamped to [0, 0x3FFF]
 */
static u16 compute_pitch(const VabTone *tone, u8 note, float bend_semitones) {
    float semitones = (float)note - (float)tone->center_note
                    + (float)tone->pitch_fine / 128.0f
                    + bend_semitones;
    float ratio = powf(2.0f, semitones / 12.0f);
    s32 pitch = (s32)(ratio * 4096.0f + 0.5f);
    if (pitch < 1) pitch = 1;
    if (pitch > 0x3FFF) pitch = 0x3FFF;
    return (u16)pitch;
}

/* ---------- Voice allocation ----------
 * Round-robin with LRU fallback (steal oldest active voice if all busy). */
static int alloc_voice(SeqSpuState *s) {
    int i, oldest = 0;
    u32 oldest_tick = 0xFFFFFFFFu;
    for (i = 0; i < NUM_VOICES; i++) {
        if (!s->voices[i].active) return i;
    }
    /* All busy — steal oldest */
    for (i = 0; i < NUM_VOICES; i++) {
        if (s->voices[i].alloc_tick < oldest_tick) {
            oldest_tick = s->voices[i].alloc_tick;
            oldest = i;
        }
    }
    /* Force-off the stolen voice */
    {
        u32 mask = 1u << oldest;
        write_reg(SPU_KEY_OFF_LO, (u16)(mask & 0xFFFF));
        write_reg(SPU_KEY_OFF_HI, (u16)((mask >> 16) & 0xFFFF));
    }
    s->voices[oldest].active = 0;
    return oldest;
}

static int find_voice_for_note(SeqSpuState *s, u8 channel, u8 note) {
    int i;
    for (i = 0; i < NUM_VOICES; i++) {
        if (s->voices[i].active &&
            s->voices[i].channel == channel &&
            s->voices[i].note == note) {
            return i;
        }
    }
    return -1;
}

/* ---------- Note on / off ---------- */
static void handle_note_on(SeqSpuState *s, u8 channel, u8 note, u8 velocity) {
    VabTone tone;
    int v;
    u32 vol_num;
    u16 voll, volr;
    float bend;
    u16 pitch;
    u32 key_mask;

    if (!s->bank) return;
    if (!vab_find_tone(s->bank, s->channels[channel].program, note, &tone)) {
        return;  /* no tone covers this note */
    }

    v = alloc_voice(s);
    s->voices[v].active = 1;
    s->voices[v].program = s->channels[channel].program;
    s->voices[v].channel = channel;
    s->voices[v].note = note;
    s->voices[v].alloc_tick = (u32)s->sample_clock;

    /* Volume: velocity * tone.vol * chan.vol * seq.vol / (127^3) * 0x3FFF
     * Then split L/R by pan. */
    vol_num = (u32)velocity * (u32)tone.vol
            * (u32)s->channels[channel].volume
            * (u32)s->master_vol;
    {
        u64 scaled = (u64)vol_num * 0x3FFFu;
        u64 denom  = 127ULL * 127ULL * 127ULL * 127ULL;
        u32 v_full = (u32)(scaled / denom);
        u8  pan    = s->channels[channel].pan;
        if (pan > 127) pan = 127;
        voll = (u16)(v_full * (127u - pan) / 127u);
        volr = (u16)(v_full * pan / 127u);
    }

    /* Pitch (pitch-bend interpreted as +/- 2 semitones, MIDI default) */
    bend = (float)s->channels[channel].pitch_bend * (2.0f / 8192.0f);
    pitch = compute_pitch(&tone, note, bend);

    write_reg(SPU_VOICE_BASE + v*0x10 + V_VOLL, voll);
    write_reg(SPU_VOICE_BASE + v*0x10 + V_VOLR, volr);
    write_reg(SPU_VOICE_BASE + v*0x10 + V_PITCH, pitch);
    write_reg(SPU_VOICE_BASE + v*0x10 + V_START_ADDR,
              (u16)((tone.spu_addr >> 3) & 0xFFFF));
    write_reg(SPU_VOICE_BASE + v*0x10 + V_ADSR1, tone.adsr1);
    write_reg(SPU_VOICE_BASE + v*0x10 + V_ADSR2, tone.adsr2);
    write_reg(SPU_VOICE_BASE + v*0x10 + V_LOOP_ADDR,
              (u16)((tone.spu_loop_addr >> 3) & 0xFFFF));

    /* Key on */
    key_mask = 1u << v;
    write_reg(SPU_KEY_ON_LO, (u16)(key_mask & 0xFFFF));
    write_reg(SPU_KEY_ON_HI, (u16)((key_mask >> 16) & 0xFFFF));
}

static void handle_note_off(SeqSpuState *s, u8 channel, u8 note) {
    int v = find_voice_for_note(s, channel, note);
    u32 mask;
    if (v < 0) return;
    mask = 1u << v;
    write_reg(SPU_KEY_OFF_LO, (u16)(mask & 0xFFFF));
    write_reg(SPU_KEY_OFF_HI, (u16)((mask >> 16) & 0xFFFF));
    /* Free immediately — ADSR release still fades the voice in the SPU. */
    s->voices[v].active = 0;
}

/* ---------- Event processing ---------- */
static void process_events(SeqSpuState *s) {
    while (s->playing && s->pos < s->size &&
           s->sample_clock >= s->next_event_sample) {

        u8 status = s->data[s->pos];
        u8 channel, cmd;

        /* Running status */
        if (status < 0x80) {
            status = s->running_status;
        } else {
            s->pos++;
            if (status < 0xF0) s->running_status = status;
        }

        channel = status & 0x0F;
        cmd     = status & 0xF0;

        switch (cmd) {
        case 0x90: { /* Note on (velocity 0 => note off) */
            u8 note, vel;
            if (s->pos + 1 >= s->size) { s->playing = 0; return; }
            note = s->data[s->pos++];
            vel  = s->data[s->pos++];
            if (vel > 0) handle_note_on(s, channel, note, vel);
            else         handle_note_off(s, channel, note);
            break;
        }
        case 0x80: { /* Note off */
            u8 note;
            if (s->pos + 1 >= s->size) { s->playing = 0; return; }
            note = s->data[s->pos++];
            s->pos++; /* vel ignored */
            handle_note_off(s, channel, note);
            break;
        }
        case 0xB0: { /* CC */
            u8 ctrl, val;
            if (s->pos + 1 >= s->size) { s->playing = 0; return; }
            ctrl = s->data[s->pos++];
            val  = s->data[s->pos++];
            if (ctrl == 7)       s->channels[channel].volume = val;
            else if (ctrl == 10) s->channels[channel].pan    = val;
            else if (ctrl == 123 || ctrl == 120) {
                /* All-notes-off / all-sound-off */
                int i;
                for (i = 0; i < NUM_VOICES; i++) {
                    if (s->voices[i].active && s->voices[i].channel == channel)
                        handle_note_off(s, channel, s->voices[i].note);
                }
            }
            break;
        }
        case 0xC0: { /* Program change */
            u8 prog;
            if (s->pos >= s->size) { s->playing = 0; return; }
            prog = s->data[s->pos++];
            s->channels[channel].program = prog;
            break;
        }
        case 0xE0: { /* Pitch bend, 2 bytes LSB/MSB */
            u8 lsb, msb;
            int v14;
            if (s->pos + 1 >= s->size) { s->playing = 0; return; }
            lsb = s->data[s->pos++];
            msb = s->data[s->pos++];
            v14 = ((int)msb << 7) | (int)lsb;
            s->channels[channel].pitch_bend = (s16)(v14 - 0x2000);
            /* Re-apply pitch to any voices still active on this channel */
            {
                int i;
                float bend = (float)s->channels[channel].pitch_bend * (2.0f/8192.0f);
                for (i = 0; i < NUM_VOICES; i++) {
                    if (!s->voices[i].active || s->voices[i].channel != channel)
                        continue;
                    /* Need tone to recompute pitch; re-lookup. */
                    {
                        VabTone t;
                        if (vab_find_tone(s->bank,
                                          s->voices[i].program,
                                          s->voices[i].note, &t)) {
                            u16 p = compute_pitch(&t, s->voices[i].note, bend);
                            write_reg(SPU_VOICE_BASE + i*0x10 + V_PITCH, p);
                        }
                    }
                }
            }
            break;
        }
        case 0xD0: /* channel pressure - 1 byte */
            if (s->pos >= s->size) { s->playing = 0; return; }
            s->pos++;
            break;
        case 0xA0: /* poly aftertouch - 2 bytes */
            if (s->pos + 1 >= s->size) { s->playing = 0; return; }
            s->pos += 2;
            break;
        case 0xF0: {
            if (status == 0xFF) { /* meta */
                u8 type;
                u32 len;
                if (s->pos >= s->size) { s->playing = 0; return; }
                type = s->data[s->pos++];
                len  = read_vlq(s);
                if (type == 0x2F) { /* end of track */
                    if (s->loop) {
                        s->pos = s->loop_start_pos;
                        s->running_status = 0;
                        s->next_event_sample = s->sample_clock;
                        /* All notes off on loop */
                        seq_spu_all_off(s);
                        return;
                    }
                    s->playing = 0;
                    return;
                } else if (type == 0x51 && len == 3 &&
                           s->pos + 2 < s->size) {
                    u32 tempo = ((u32)s->data[s->pos] << 16) |
                                ((u32)s->data[s->pos+1] << 8) |
                                       s->data[s->pos+2];
                    s->tempo_us_per_qn = tempo;
                    s->samples_per_tick =
                        (u32)(((u64)SAMPLE_RATE * (u64)tempo)
                              / ((u64)s->ppqn * 1000000ULL));
                    if (s->samples_per_tick == 0) s->samples_per_tick = 1;
                }
                if (s->pos + len <= s->size) s->pos += len;
                else { s->playing = 0; return; }
            } else {
                /* SysEx: skip until 0xF7 */
                while (s->pos < s->size && s->data[s->pos] != 0xF7) s->pos++;
                if (s->pos < s->size) s->pos++;
            }
            break;
        }
        default:
            /* Unknown — abort to avoid desync */
            s->playing = 0;
            return;
        }

        /* Delta-time FOLLOWS the event (PS1 convention) */
        if (s->pos < s->size) {
            u32 delta = read_vlq(s);
            s->next_event_sample += (u64)delta * (u64)s->samples_per_tick;
        } else {
            s->playing = 0;
        }
    }
}

/* ============================================================
 *                    Public API
 * ============================================================ */

SeqSpuState *seq_spu_start(const u8 *seq_data, u32 seq_size,
                           const VabBank *bank, int loop) {
    SeqSpuState *s;
    int i;

    if (!seq_data || seq_size < 16) return NULL;

    s = (SeqSpuState *)calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->data = seq_data;
    s->size = seq_size;
    s->bank = bank;
    s->loop = loop ? 1 : 0;
    s->master_vol = 127;

    for (i = 0; i < 16; i++) {
        s->channels[i].program = 0;
        s->channels[i].volume  = 100;  /* GM default */
        s->channels[i].pan     = 64;   /* centre */
        s->channels[i].pitch_bend = 0;
    }
    for (i = 0; i < NUM_VOICES; i++) s->voices[i].active = 0;

    if (!parse_header(s)) {
        free(s);
        return NULL;
    }

    s->sample_clock = 0;
    s->next_event_sample = 0;
    s->running_status = 0;
    s->playing = 1;

    /* Unmute main SPU volume so writes are audible. */
    write_reg(SPU_MAIN_VOL_L, 0x3FFF);
    write_reg(SPU_MAIN_VOL_R, 0x3FFF);

    return s;
}

void seq_spu_stop(SeqSpuState *s) {
    if (!s) return;
    seq_spu_all_off(s);
    free(s);
}

void seq_spu_advance(SeqSpuState *s, u32 sample_frames) {
    if (!s || !s->playing) return;
    s->sample_clock += sample_frames;
    process_events(s);
}

u32 seq_spu_frames_to_next_event(const SeqSpuState *s) {
    if (!s || !s->playing) return 0xFFFFFFFFu;
    if (s->next_event_sample <= s->sample_clock) return 0;
    {
        u64 delta = s->next_event_sample - s->sample_clock;
        if (delta > 0xFFFFFFFFu) return 0xFFFFFFFFu;
        return (u32)delta;
    }
}

void seq_spu_set_volume(SeqSpuState *s, u8 vol) {
    if (!s) return;
    if (vol > 127) vol = 127;
    s->master_vol = vol;
}

void seq_spu_all_off(SeqSpuState *s) {
    int i;
    u32 mask = 0;
    if (!s) return;
    for (i = 0; i < NUM_VOICES; i++) {
        if (s->voices[i].active) {
            mask |= (1u << i);
            s->voices[i].active = 0;
        }
    }
    if (mask) {
        write_reg(SPU_KEY_OFF_LO, (u16)(mask & 0xFFFF));
        write_reg(SPU_KEY_OFF_HI, (u16)((mask >> 16) & 0xFFFF));
    }
}
