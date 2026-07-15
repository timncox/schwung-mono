#include "mono_core.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MONO_WAVES 32
#define MONO_WAVE_LEN 512
#define MONO_DELAY_SECONDS 2

enum {
    TRIG_NOTE = 1,
    TRIG_AMP = 2,
    TRIG_FILTER = 4,
    TRIG_LFO = 8
};

typedef enum {
    ENV_OFF = 0,
    ENV_ATTACK,
    ENV_HOLD,
    ENV_DECAY,
    ENV_RELEASE
} env_stage_t;

typedef struct {
    float value;
    float release_step;
    int hold_left;
    env_stage_t stage;
} mono_env_t;

typedef struct {
    float low;
    float band;
} mono_svf_t;

typedef struct {
    float phase;
    float held;
    uint32_t rng;
    int stopped;
} mono_lfo_t;

typedef struct {
    int8_t note;
    uint8_t velocity;
    uint8_t gate;
    uint8_t trig_mask;
    uint64_t lock_mask;
    uint8_t lock_values[MONO_PARAMS];
} mono_step_t;

typedef struct {
    mono_machine_t machine;
    uint8_t base[MONO_PARAMS];
    uint8_t effective[MONO_PARAMS];
    float smoothed[MONO_PARAMS];
    mono_step_t steps[MONO_STEPS];

    int note;
    int last_note;
    int velocity;
    int gate_left;
    float freq;
    float target_freq;
    float phase[10];
    float mod_phase[4];
    float fm_feedback;
    float fm_feedback2;
    uint32_t noise;

    mono_env_t amp;
    mono_env_t filter_env;
    mono_lfo_t lfo[3];
    /* LFO-to-LFO modulation is fed into the next sample. This makes cross- and
     * self-modulation deterministic without recursively evaluating an LFO. */
    float lfo_param_mod[3 * MONO_PAGE_PARAMS];
    mono_svf_t hp[2];
    mono_svf_t lp[2];
    mono_svf_t eq;

    int srr_left;
    float srr_hold;
    float *delay;
    int delay_pos;
    float delay_lp[2];
    float delay_hp[2];
    float delay_hp_in[2];
} mono_track_t;

struct mono {
    const host_api_v1_t *host;
    int sample_rate;
    int track_count;
    int selected_track;
    int selected_page;
    int step_page;
    int pattern_len;
    int transport;
    int record_locks;
    int seq_step;
    int tick_in_step;
    int external_clock;
    int external_clock_age;
    double internal_frames;
    float bpm_override;
    float master;
    float smooth_coeff;
    uint32_t revision;
    uint32_t note_events;
    uint32_t render_blocks;
    uint32_t nonzero_blocks;
    int render_peak;
    int lifetime_peak;
    uint32_t nonfinite_samples;
    float wavetable[MONO_WAVES][MONO_WAVE_LEN];
    mono_track_t track[MONO_MAX_TRACKS];
};

static float fclamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int iclamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void changed(mono_t *m) {
    if (m) ++m->revision;
}

static int is_lfo_destination_param(int pid) {
    return pid >= 32 && pid < MONO_PRIMARY_PARAMS && ((pid - 32) % 8) == 0;
}

static int is_smoothable_param(const mono_track_t *t, int pid) {
    /* Keep enumerated choices stepped so the glide never passes through an
     * unintended waveform/routing mode. Everything else gets a short zipper-
     * noise-free glide, including ordinary synth controls. */
    if (pid < 8) {
        if (t->machine == MONO_SID_6581 && (pid == 3 || pid == 4)) return 0;
        if (t->machine == MONO_DIGIPRO_WAVE && (pid == 0 || pid == 4)) return 0;
        if (t->machine == MONO_FM_STATIC && (pid == 0 || pid == 4)) return 0;
        return 1;
    }
    if (pid < 32) return 1;
    if (pid >= 32 && pid < MONO_PRIMARY_PARAMS) {
        int offset = (pid - 32) % MONO_PAGE_PARAMS;
        return offset == 4 || offset == 6 || offset == 7;
    }
    if (pid >= MONO_ALT_BASE && pid < MONO_PARAMS) {
        int offset = pid - MONO_ALT_BASE;
        if (t->machine == MONO_SWAVE_PULSE && offset == 3) return 0;
        if (t->machine == MONO_DIGIPRO_WAVE && (offset == 0 || offset == 3)) return 0;
        if (t->machine == MONO_FM_STATIC && offset == 2) return 0;
        return 1;
    }
    return 0;
}

static void sync_smoothed(mono_track_t *t) {
    for (int pid = 0; pid < MONO_PARAMS; ++pid)
        t->smoothed[pid] = (float)t->effective[pid];
}

static void reset_effective_to_base(mono_t *m) {
    for (int track = 0; track < m->track_count; ++track)
        memcpy(m->track[track].effective, m->track[track].base, MONO_PARAMS);
}

static void smooth_param_value(const mono_t *m, mono_track_t *t,
                               uint8_t *rendered, int pid) {
    float target = (float)t->effective[pid];
    t->smoothed[pid] += (target - t->smoothed[pid]) * m->smooth_coeff;
    if (fabsf(target - t->smoothed[pid]) < 0.001f) t->smoothed[pid] = target;
    rendered[pid] = (uint8_t)iclamp((int)lrintf(t->smoothed[pid]), 0, 127);
}

static int lfo_destination_index(int value) {
    return iclamp(value, 0, MONO_LFO_DESTINATIONS - 1);
}

static int lfo_destination_param(int destination) {
    return destination >= 2 ? destination - 2 : -1;
}

static int migrate_legacy_lfo_destination(int value) {
    /* v2/v3 stored the original seven destinations as 0,16,...,96. */
    static const uint8_t migrated[7] = {
        0,                  /* Off */
        1,                  /* Pitch */
        2 + 16,             /* Filter Base */
        2 + 17,             /* Filter Width */
        2 + 13,             /* Volume */
        2 + 14,             /* Pan */
        2 + 28              /* Delay Time */
    };
    return migrated[iclamp(value / 16, 0, 6)];
}

static int appendf(char *buf, int buf_len, int *used, const char *fmt, ...) {
    if (*used < 0 || *used >= buf_len) return 0;
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(buf + *used, (size_t)(buf_len - *used), fmt, ap);
    va_end(ap);
    if (wrote < 0 || wrote >= buf_len - *used) return 0;
    *used += wrote;
    return 1;
}

static float midi_freq(float note) {
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}

static float pnorm(int p) {
    return (float)iclamp(p, 0, 127) / 127.0f;
}

static float time_from_param(int p, float max_seconds) {
    if (p <= 0) return 0.0f;
    return 0.002f * powf(max_seconds / 0.002f, pnorm(p));
}

static float bpm_now(const mono_t *m) {
    if (m->bpm_override >= 30.0f) return m->bpm_override;
    if (m->host && m->host->get_bpm) {
        float bpm = m->host->get_bpm();
        if (bpm >= 30.0f && bpm <= 400.0f) return bpm;
    }
    return 120.0f;
}

static uint32_t xrnd(uint32_t *state) {
    uint32_t x = *state ? *state : 0x6d2b79f5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float phase_step(float *phase, float hz, int sample_rate) {
    float old = *phase;
    *phase += hz / (float)sample_rate;
    *phase -= floorf(*phase);
    return *phase < old ? 1.0f : 0.0f;
}

static float saw(float p) { return 2.0f * p - 1.0f; }

static float tri(float p) {
    return 1.0f - 4.0f * fabsf(p - 0.5f);
}

static float pulse(float p, float width) {
    return p < width ? 1.0f : -1.0f;
}

static void generate_wavetables(mono_t *m) {
    for (int w = 0; w < MONO_WAVES; ++w) {
        float peak = 0.0f;
        for (int i = 0; i < MONO_WAVE_LEN; ++i) {
            float ph = (float)i / MONO_WAVE_LEN;
            float v = 0.0f;
            int harmonics = 2 + (w % 14);
            for (int h = 1; h <= harmonics; ++h) {
                float amp;
                switch (w / 8) {
                case 0: amp = 1.0f / h; break;
                case 1: amp = (h & 1) ? 1.0f / h : 0.0f; break;
                case 2: amp = (h & 1) ? 1.0f / (h * h) : 0.0f; break;
                default: amp = sinf((float)(w + 1) * h * 0.731f) / h; break;
                }
                v += amp * sinf(2.0f * (float)M_PI * h * ph + w * 0.173f);
            }
            m->wavetable[w][i] = v;
            if (fabsf(v) > peak) peak = fabsf(v);
        }
        if (peak < 1e-6f) peak = 1.0f;
        for (int i = 0; i < MONO_WAVE_LEN; ++i) {
            float v = m->wavetable[w][i] / peak;
            m->wavetable[w][i] = roundf(v * 2047.0f) / 2047.0f;
        }
    }
}

static void clear_step(mono_step_t *s) {
    memset(s, 0, sizeof(*s));
    s->note = -1;
    s->velocity = 100;
    s->gate = 100;
}

static void common_defaults(mono_track_t *t) {
    static const uint8_t amp[8] = { 0, 96, 48, 28, 12, 110, 64, 0 };
    static const uint8_t flt[8] = { 0, 127, 0, 0, 0, 48, 64, 32 };
    static const uint8_t fx[8]  = { 64, 64, 127, 0, 48, 32, 0, 127 };
    static const uint8_t lfo[8] = { 0, 0, 0, 32, 32, 0, 0, 0 };
    memcpy(t->base + 8, amp, 8);
    memcpy(t->base + 16, flt, 8);
    memcpy(t->base + 24, fx, 8);
    for (int p = 4; p < 7; ++p) memcpy(t->base + p * 8, lfo, 8);
}

static void machine_defaults(mono_track_t *t, mono_machine_t machine) {
    static const uint8_t defs[MONO_MACHINE_COUNT][8] = {
        { 76, 18, 32, 0, 0, 0, 0, 64 },  /* SWAVE SAW */
        { 76, 18, 32, 64, 0, 1, 0, 64 }, /* SWAVE PULSE */
        { 20, 35, 55, 72, 64, 50, 48, 64 },
        { 64, 0, 1, 30, 0, 0, 64, 64 },
        { 0, 0, 0, 1, 0, 64, 0, 64 },
        { 34, 64, 10, 72, 46, 42, 72, 64 }
    };
    static const uint8_t alt_defs[MONO_MACHINE_COUNT][MONO_ALT_PARAMS] = {
        { 0, 21, 64, 0, 0, 0, 0, 0 },       /* BRITE STACK SUBPW SUBOCT */
        { 64, 21, 0, 0, 0, 0, 0, 0 },       /* ASYM STACK SUB SYNC */
        { 127, 127, 127, 127, 0, 0, 0, 0 }, /* per-voice levels */
        { 0, 0, 0, 0, 0, 0, 0, 0 },         /* RING SUB MODMIX CHAOS */
        { 0, 0, 64, 64, 0, 0, 0, 0 },       /* WAVE2 BLEND DETUNE OCTAVE */
        { 64, 0, 34, 0, 0, 0, 0, 0 }        /* OP2FIN OP2FB OP3RAT OP3LVL */
    };
    t->machine = machine;
    memcpy(t->base, defs[machine], 8);
    memcpy(t->base + MONO_ALT_BASE, alt_defs[machine], MONO_ALT_PARAMS);
    memcpy(t->effective, t->base, MONO_PARAMS);
    sync_smoothed(t);
}

static void init_track(mono_track_t *t, int index, int delay_frames) {
    memset(t, 0, sizeof(*t));
    t->machine = MONO_SWAVE_SAW;
    t->note = -1;
    t->last_note = 48 + index * 5;
    t->noise = 0x1234567u ^ (uint32_t)(index * 0x9e3779b9u);
    for (int i = 0; i < MONO_STEPS; ++i) clear_step(&t->steps[i]);
    common_defaults(t);
    machine_defaults(t, MONO_SWAVE_SAW);
    memcpy(t->effective, t->base, MONO_PARAMS);
    for (int i = 0; i < 3; ++i) t->lfo[i].rng = t->noise + (uint32_t)i * 7919u;
    t->delay = calloc((size_t)delay_frames * 2, sizeof(float));
}

mono_t *mono_create(const host_api_v1_t *host, int track_count) {
    mono_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->host = host;
    m->sample_rate = host && host->sample_rate > 0 ? host->sample_rate : MOVE_SAMPLE_RATE;
    m->track_count = iclamp(track_count, 1, MONO_MAX_TRACKS);
    m->pattern_len = 16;
    m->seq_step = -1;
    m->master = m->track_count > 1 ? 0.34f : 0.7f;
    m->smooth_coeff = 1.0f - expf(-1.0f / (0.030f * m->sample_rate));
    generate_wavetables(m);
    int delay_frames = m->sample_rate * MONO_DELAY_SECONDS;
    for (int i = 0; i < m->track_count; ++i) {
        init_track(&m->track[i], i, delay_frames);
        if (!m->track[i].delay) {
            mono_destroy(m);
            return NULL;
        }
    }
    return m;
}

void mono_destroy(mono_t *m) {
    if (!m) return;
    for (int i = 0; i < MONO_MAX_TRACKS; ++i) free(m->track[i].delay);
    free(m);
}

static void env_trigger(mono_env_t *e, int attack_param, int sample_rate) {
    e->hold_left = 0;
    e->release_step = 0.0f;
    if (attack_param <= 0) {
        e->value = 1.0f;
        e->stage = ENV_HOLD;
    } else {
        e->stage = ENV_ATTACK;
        if (e->value < 0.0f || e->value > 1.0f) e->value = 0.0f;
    }
    (void)sample_rate;
}

static void env_release(mono_env_t *e, int release_param, int sample_rate) {
    if (e->stage == ENV_OFF) return;
    float sec = time_from_param(release_param, 8.0f);
    if (sec <= 0.0f) {
        e->value = 0.0f;
        e->stage = ENV_OFF;
    } else {
        e->release_step = e->value / (sec * sample_rate);
        e->stage = ENV_RELEASE;
    }
}

static float amp_env_tick(mono_env_t *e, const uint8_t *p, int sample_rate) {
    switch (e->stage) {
    case ENV_ATTACK: {
        float sec = time_from_param(p[0], 4.0f);
        e->value += sec <= 0.0f ? 1.0f : 1.0f / (sec * sample_rate);
        if (e->value >= 1.0f) {
            e->value = 1.0f;
            e->hold_left = (int)(time_from_param(p[1], 4.0f) * sample_rate);
            e->stage = ENV_HOLD;
        }
        break;
    }
    case ENV_HOLD:
        if (e->hold_left <= 0)
            e->hold_left = (int)(time_from_param(p[1], 4.0f) * sample_rate);
        if (--e->hold_left <= 0) e->stage = ENV_DECAY;
        break;
    case ENV_DECAY: {
        float sec = time_from_param(p[2], 12.0f);
        e->value -= sec <= 0.0f ? 1.0f : 1.0f / (sec * sample_rate);
        if (e->value <= 0.0f) { e->value = 0.0f; e->stage = ENV_OFF; }
        break;
    }
    case ENV_RELEASE:
        e->value -= e->release_step;
        if (e->value <= 0.0f) { e->value = 0.0f; e->stage = ENV_OFF; }
        break;
    default: break;
    }
    return e->value;
}

static float filter_env_tick(mono_env_t *e, const uint8_t *p, int sample_rate) {
    if (e->stage == ENV_ATTACK) {
        float sec = time_from_param(p[4], 4.0f);
        e->value += sec <= 0.0f ? 1.0f : 1.0f / (sec * sample_rate);
        if (e->value >= 1.0f) { e->value = 1.0f; e->stage = ENV_DECAY; }
    } else if (e->stage == ENV_DECAY || e->stage == ENV_HOLD) {
        float sec = time_from_param(p[5], 8.0f);
        e->value -= sec <= 0.0f ? 1.0f : 1.0f / (sec * sample_rate);
        if (e->value <= 0.0f) { e->value = 0.0f; e->stage = ENV_OFF; }
    }
    return e->value;
}

static void lfo_trigger(mono_lfo_t *l, const uint8_t *p) {
    int mode = (p[1] * 5) / 128;
    if (mode != 0) {
        l->phase = pnorm(p[7]);
        l->stopped = 0;
        if (mode == 2) l->held = 0.0f;
    }
}

static float lfo_tick(mono_lfo_t *l, const uint8_t *p, float bpm, int sample_rate) {
    int wave = (p[2] * 5) / 128;
    int mode = (p[1] * 5) / 128;
    static const float mults[8] = { 0.125f, 0.25f, 0.5f, 1, 2, 4, 8, 16 };
    int mi = (p[3] * 8) / 128;
    float hz = (bpm / 60.0f) * mults[mi] * (0.125f + 3.875f * pnorm(p[4]));
    float old = l->phase;
    if (!l->stopped) {
        l->phase += hz / sample_rate;
        if (l->phase >= 1.0f) l->phase -= 1.0f;
    }
    if (l->phase < old && wave == 4)
        l->held = ((xrnd(&l->rng) >> 8) / 8388607.5f) - 1.0f;
    float v;
    switch (wave) {
    case 1: v = saw(l->phase); break;
    case 2: v = tri(l->phase); break;
    case 3: v = l->phase < 0.5f ? 1.0f : -1.0f; break;
    case 4: v = l->held; break;
    default: v = sinf(2.0f * (float)M_PI * l->phase); break;
    }
    if (mode == 2) {
        if (old == pnorm(p[7])) l->held = v;
        v = l->held;
    } else if (mode == 3 && l->phase < old) {
        l->stopped = 1;
    } else if (mode == 4 && old < 0.5f && l->phase >= 0.5f) {
        l->stopped = 1;
    }
    int interlace = p[5];
    if (interlace && ((int)(l->phase * (2 + interlace / 8)) & 1)) v = 0.0f;
    return v * pnorm(p[6]);
}

static void track_pitch(mono_track_t *t, int note, int velocity) {
    t->note = note;
    t->last_note = note;
    t->velocity = iclamp(velocity, 1, 127);
    t->target_freq = midi_freq(note);
    if (t->freq <= 0.0f || t->effective[15] == 0) t->freq = t->target_freq;
}

static void track_trigger(mono_t *m, mono_track_t *t, int note,
                          int velocity, int trig_mask, int gate) {
    if ((trig_mask & TRIG_NOTE) && note >= 0) track_pitch(t, note, velocity);
    if (trig_mask & TRIG_AMP) env_trigger(&t->amp, t->effective[8], m->sample_rate);
    if (trig_mask & TRIG_FILTER) {
        t->filter_env.value = 0.0f;
        t->filter_env.stage = ENV_ATTACK;
    }
    if (trig_mask & TRIG_LFO)
        for (int i = 0; i < 3; ++i) lfo_trigger(&t->lfo[i], t->effective + (4 + i) * 8);
    if (gate > 0) {
        float frames_per_step = m->sample_rate * 60.0f / (bpm_now(m) * 4.0f);
        t->gate_left = (int)(frames_per_step * fclamp(gate / 127.0f, 0.03f, 1.0f));
    } else {
        /* A played note belongs to the performer until note-off. Sequencer
         * trigs pass their positive gate value and retain step-length gates. */
        t->gate_left = 0;
    }
}

void mono_note_on(mono_t *m, int track, int note, int velocity) {
    if (!m || track < 0 || track >= m->track_count) return;
    mono_track_t *t = &m->track[track];
    /* While clocked, keep the current automation targets under incoming Move
     * notes. Stopped performance notes still start from the base patch. */
    if (!m->transport) memcpy(t->effective, t->base, MONO_PARAMS);
    track_trigger(m, t, iclamp(note, 0, 127), velocity, 15, 0);
    ++m->note_events;
    changed(m);
}

void mono_note_off(mono_t *m, int track, int note) {
    if (!m || track < 0 || track >= m->track_count) return;
    mono_track_t *t = &m->track[track];
    if (note < 0 || t->note == note) {
        env_release(&t->amp, t->effective[11], m->sample_rate);
        t->gate_left = 0;
        changed(m);
    }
}

static float osc_swavesaw(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    const uint8_t *a = t->effective + MONO_ALT_BASE;
    float stack = fclamp(1.0f + ((int)a[1] - 21) * (2.0f / 106.0f), 0.6f, 3.0f);
    float width = 0.009f * pnorm(p[1]) * stack;
    float ext = width * 2.0f;
    float v = saw(t->phase[0]);
    phase_step(&t->phase[0], freq, m->sample_rate);
    v += sinf(4.0f * (float)M_PI * t->phase[0]) * pnorm(a[0]) * 0.45f;
    float ul = pnorm(p[0]);
    float ux = ul * pnorm(p[2]);
    const float ratios[4] = { 1.0f - width, 1.0f + width, 1.0f - ext, 1.0f + ext };
    for (int i = 0; i < 4; ++i) {
        phase_step(&t->phase[1 + i], freq * ratios[i], m->sample_rate);
        v += saw(t->phase[1 + i]) * (i < 2 ? ul : ux);
    }
    phase_step(&t->phase[5], freq * 0.5f, m->sample_rate);
    phase_step(&t->phase[6], freq * 0.5f, m->sample_rate);
    phase_step(&t->phase[7], freq * 0.25f, m->sample_rate);
    float sub_pw = fclamp(0.5f + ((int)a[2] - 64) * (0.9f / 127.0f), 0.05f, 0.95f);
    v += pulse(t->phase[5], sub_pw) * pnorm(p[3]);
    v += sinf(2.0f * (float)M_PI * t->phase[6]) * pnorm(p[4]);
    v += sinf(2.0f * (float)M_PI * t->phase[7]) * pnorm(p[5]);
    phase_step(&t->phase[8], freq * 0.125f, m->sample_rate);
    float sub_oct = pnorm(a[3]);
    v += sinf(2.0f * (float)M_PI * t->phase[8]) * sub_oct;
    return v / (1.0f + 2.0f * ul + 2.0f * ux + pnorm(p[3]) +
                pnorm(p[4]) + pnorm(p[5]) + sub_oct);
}

static float osc_swavepulse(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    const uint8_t *a = t->effective + MONO_ALT_BASE;
    float stack = fclamp(1.0f + ((int)a[1] - 21) * (2.0f / 106.0f), 0.6f, 3.0f);
    float det = 0.009f * pnorm(p[1]) * stack;
    float pw = fclamp(0.05f + 0.9f * pnorm(p[3]), 0.03f, 0.97f);
    float sweep = (pnorm(p[4]) - 0.5f) * 0.35f * sinf(2.0f * (float)M_PI * t->mod_phase[0]);
    phase_step(&t->mod_phase[0], 0.08f + 7.0f * pnorm(p[5]), m->sample_rate);
    pw = fclamp(pw + sweep, 0.03f, 0.97f);
    float v = 0.0f;
    float ratios[5] = { 1, 1-det, 1+det, 1-2*det, 1+2*det };
    float levels[5] = { 1, pnorm(p[0]), pnorm(p[0]), pnorm(p[2]), pnorm(p[2]) };
    float sum = 0.0f;
    if (a[3] && phase_step(&t->mod_phase[2], freq * (1.0f + 7.0f * pnorm(a[3])),
                            m->sample_rate))
        for (int i = 0; i < 5; ++i) t->phase[i] = 0.0f;
    float asym = ((int)a[0] - 64) / 64.0f * 0.25f;
    for (int i = 0; i < 5; ++i) {
        phase_step(&t->phase[i], freq * ratios[i], m->sample_rate);
        float voice_pw = fclamp(pw + asym * (i * 0.25f), 0.03f, 0.97f);
        v += pulse(t->phase[i], voice_pw) * levels[i];
        sum += levels[i];
    }
    v += saw(t->phase[0]) * asym * sum * 0.35f;
    float sub = pnorm(a[2]);
    phase_step(&t->phase[5], freq * 0.5f, m->sample_rate);
    v += pulse(t->phase[5], pw) * sub;
    sum += sub;
    return v / fmaxf(sum, 1.0f);
}

static float chord_offset(int p) {
    if (p < 4) return 0.0f;
    return roundf((p - 4) * 24.0f / 123.0f);
}

static float osc_ensemble(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    float shape = pnorm(p[4]);
    float pw = fclamp(pnorm(p[5]), 0.05f, 0.95f);
    float chorus = 0.004f * pnorm(p[7]);
    float v = 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        float semi = i == 0 ? 0.0f : chord_offset(p[i - 1]);
        float r = powf(2.0f, semi / 12.0f) * (1.0f + chorus * (i - 1.5f));
        phase_step(&t->phase[i], freq * r, m->sample_rate);
        float a = saw(t->phase[i]);
        float b = pulse(t->phase[i], pw);
        float level = pnorm(t->effective[MONO_ALT_BASE + i]);
        v += (a + (b - a) * shape) * level;
        sum += level;
    }
    return v / fmaxf(sum, 1.0f);
}

static float osc_sid(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    const uint8_t *a = t->effective + MONO_ALT_BASE;
    float mod_ratio = 0.125f * powf(2.0f, pnorm(p[6]) * 6.0f);
    int mod_wrap = (int)phase_step(&t->mod_phase[0], freq * mod_ratio, m->sample_rate);
    int mode = (p[4] * 4) / 128;
    if ((mode == 2 || mode == 3) && mod_wrap) t->phase[0] = 0.0f;
    int wrap = (int)phase_step(&t->phase[0], freq, m->sample_rate);
    if (wrap) xrnd(&t->noise);
    float pw = fclamp(0.02f + 0.96f * pnorm(p[0]) +
                      (pnorm(p[1]) - 0.5f) * sinf(2.0f * (float)M_PI * t->mod_phase[1]),
                      0.02f, 0.98f);
    phase_step(&t->mod_phase[1], 0.15f + 5.0f * pnorm(p[1]), m->sample_rate);
    int wave = (p[3] * 5) / 128;
    float v;
    switch (wave) {
    case 0: v = tri(t->phase[0]); break;
    case 1: v = saw(t->phase[0]); break;
    case 2: v = pulse(t->phase[0], pw); break;
    case 3: v = 0.5f * (saw(t->phase[0]) + pulse(t->phase[0], pw)); break;
    default: v = ((int32_t)t->noise) / 2147483648.0f; break;
    }
    float mod = sinf(2.0f * (float)M_PI * t->mod_phase[0]);
    if (mode == 1 || mode == 3) v *= t->mod_phase[0] < 0.5f ? -1.0f : 1.0f;
    float ring = pnorm(a[0]);
    v += (v * mod - v) * ring;
    float sub = pnorm(a[1]);
    phase_step(&t->phase[1], freq * 0.5f, m->sample_rate);
    v = (v + pulse(t->phase[1], 0.5f) * sub) / (1.0f + sub);
    v += mod * pnorm(a[2]) * 0.5f;
    if (a[3]) {
        float chaos = ((int32_t)xrnd(&t->noise)) / 2147483648.0f;
        float mix = pnorm(a[3]) * 0.45f;
        v += (chaos - v) * mix;
    }
    return roundf(v * 2047.0f) / 2047.0f;
}

static float table_read(const float *tab, float phase) {
    float x = phase * MONO_WAVE_LEN;
    int i = (int)x & (MONO_WAVE_LEN - 1);
    int j = (i + 1) & (MONO_WAVE_LEN - 1);
    float f = x - floorf(x);
    return tab[i] + (tab[j] - tab[i]) * f;
}

static float osc_digipro(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    int wave = (p[0] * MONO_WAVES) / 128;
    int next = (wave + 1) % MONO_WAVES;
    float morph = pnorm(p[1]);
    phase_step(&t->mod_phase[0], 0.02f + 18.0f * pnorm(p[2]), m->sample_rate);
    morph = fclamp(morph + sinf(2.0f * (float)M_PI * t->mod_phase[0]) * pnorm(p[2]) * 0.5f,
                   0.0f, 1.0f);
    float sync_hz = freq * (0.25f + 7.75f * pnorm(p[5]));
    int sw = (int)phase_step(&t->mod_phase[1], sync_hz, m->sample_rate);
    if (p[4] > 42 && sw) t->phase[0] = 0.0f;
    phase_step(&t->phase[0], freq, m->sample_rate);
    float a = table_read(m->wavetable[wave], t->phase[0]);
    float b = table_read(m->wavetable[next], t->phase[0]);
    float primary = a + (b - a) * morph;
    int wave2 = (t->effective[MONO_ALT_BASE] * MONO_WAVES) / 128;
    float detune = powf(2.0f, ((int)t->effective[MONO_ALT_BASE + 2] - 64) /
                                      (64.0f * 12.0f));
    int octave = (int)lrintf(((int)t->effective[MONO_ALT_BASE + 3] - 64) / 32.0f);
    phase_step(&t->phase[1], freq * detune * powf(2.0f, (float)octave), m->sample_rate);
    float secondary = table_read(m->wavetable[wave2], t->phase[1]);
    float blend = pnorm(t->effective[MONO_ALT_BASE + 1]);
    return primary + (secondary - primary) * blend;
}

static float osc_fm(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    const uint8_t *a = t->effective + MONO_ALT_BASE;
    static const float ratios[16] = {
        0.25f, 0.3333f, 0.5f, 0.6667f, 0.75f, 1, 1.25f, 1.5f,
        2, 2.5f, 3, 4, 5, 6, 8, 12
    };
    int r1 = (p[0] * 16) / 128;
    int r2 = (p[4] * 16) / 128;
    float fine = powf(2.0f, ((int)p[1] - 64) / (64.0f * 12.0f));
    phase_step(&t->mod_phase[0], freq * ratios[r1] * fine, m->sample_rate);
    float fine2 = powf(2.0f, ((int)a[0] - 64) / (64.0f * 12.0f));
    phase_step(&t->mod_phase[1], freq * ratios[r2] * fine2, m->sample_rate);
    float fb = pnorm(p[2]) * 3.5f;
    float m1 = sinf(2.0f * (float)M_PI * t->mod_phase[0] + t->fm_feedback * fb);
    float m2 = sinf(2.0f * (float)M_PI * t->mod_phase[1] +
                     t->fm_feedback2 * pnorm(a[1]) * 3.5f);
    int r3 = (a[2] * 16) / 128;
    phase_step(&t->mod_phase[2], freq * ratios[r3], m->sample_rate);
    float m3 = sinf(2.0f * (float)M_PI * t->mod_phase[2]);
    float index = pnorm(p[3]) * 8.0f;
    float index2 = pnorm(p[5]) * 8.0f;
    phase_step(&t->phase[0], freq, m->sample_rate);
    float v = sinf(2.0f * (float)M_PI * t->phase[0] + m1 * index +
                   m2 * index2 + m3 * pnorm(a[3]) * 8.0f);
    t->fm_feedback = m1;
    t->fm_feedback2 = m2;
    float tone = 0.2f + 0.8f * pnorm(p[6]);
    return tanhf(v / tone) * tone;
}

static float secondary_finish(mono_track_t *t, float v) {
    const uint8_t *a = t->effective + MONO_ALT_BASE;
    if (a[5]) {
        float drive = 1.0f + pnorm(a[5]) * 12.0f;
        v = tanhf(v * drive) / tanhf(drive);
    }
    if (a[6]) {
        int bits = 16 - (a[6] * 12) / 127;
        float levels = (float)((1u << bits) - 1u);
        v = roundf(v * levels) / levels;
    }
    if (a[7]) {
        float noise = ((int32_t)xrnd(&t->noise)) / 2147483648.0f;
        float mix = pnorm(a[7]) * 0.35f;
        v += (noise - v) * mix;
    }
    return v;
}

static float oscillator(mono_t *m, mono_track_t *t, float freq) {
    float v;
    switch (t->machine) {
    case MONO_SWAVE_PULSE: v = osc_swavepulse(m, t, freq); break;
    case MONO_SWAVE_ENSEMBLE: v = osc_ensemble(m, t, freq); break;
    case MONO_SID_6581: v = osc_sid(m, t, freq); break;
    case MONO_DIGIPRO_WAVE: v = osc_digipro(m, t, freq); break;
    case MONO_FM_STATIC: v = osc_fm(m, t, freq); break;
    default: v = osc_swavesaw(m, t, freq); break;
    }
    return secondary_finish(t, v);
}

static float cutoff_from_param(float p) {
    return 18.0f * powf(1000.0f, fclamp(p, 0.0f, 127.0f) / 127.0f);
}

static float svf(mono_svf_t *s, float in, float hz, float resonance,
                 int sample_rate, int highpass) {
    /* Topology-preserving state-variable filter. The former Chamberlin
     * integrator became unstable at the default wide-open cutoff, poisoning
     * its state with NaNs after a few blocks and silencing every later note. */
    hz = fclamp(hz, 15.0f, sample_rate * 0.45f);
    float g = tanf((float)M_PI * hz / sample_rate);
    float k = 2.0f - 1.95f * fclamp(resonance, 0.0f, 1.0f);
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    float v3 = in - s->low;
    float band = a1 * s->band + a2 * v3;
    float low = s->low + a2 * s->band + a3 * v3;
    s->band = 2.0f * band - s->band;
    s->low = 2.0f * low - s->low;
    float high = in - k * band - low;
    if (!isfinite(high) || !isfinite(low) || !isfinite(s->band) ||
        !isfinite(s->low)) {
        s->band = 0.0f;
        s->low = 0.0f;
        return 0.0f;
    }
    return highpass ? high : low;
}

static float render_track(mono_t *m, mono_track_t *t, float *right) {
    float bpm = bpm_now(m);
    uint8_t targets[MONO_PARAMS];
    uint8_t unmodulated[MONO_PARAMS];
    uint8_t modulated[MONO_PARAMS];
    float next_lfo_mod[3 * MONO_PAGE_PARAMS] = {0};
    int target_pid[3] = {-1, -1, -1};
    float target_delta[3] = {0};
    memcpy(targets, t->effective, sizeof(targets));
    memcpy(unmodulated, targets, sizeof(unmodulated));
    for (int pid = 0; pid < MONO_PARAMS; ++pid)
        if (is_smoothable_param(t, pid))
            smooth_param_value(m, t, unmodulated, pid);
    memcpy(modulated, unmodulated, sizeof(modulated));

    /* LFO controls receive the previous sample's modulation while the current
     * outputs are evaluated. All other destinations are applied immediately. */
    for (int pid = 32; pid < MONO_PRIMARY_PARAMS; ++pid) {
        int value = (int)lrintf(unmodulated[pid] + t->lfo_param_mod[pid - 32]);
        int max = is_lfo_destination_param(pid) ? MONO_LFO_DESTINATIONS - 1 : 127;
        modulated[pid] = (uint8_t)iclamp(value, 0, max);
    }

    float pitch_mod = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const uint8_t *lp = modulated + (4 + i) * 8;
        int dest = lfo_destination_index(lp[0]);
        float v = lfo_tick(&t->lfo[i], lp, bpm, m->sample_rate);
        if (dest == 1) {
            pitch_mod += v * 24.0f;
        } else {
            int pid = lfo_destination_param(dest);
            if (pid >= 0 && pid < MONO_PARAMS) {
                target_pid[i] = pid;
                target_delta[i] = v * 64.0f;
            }
        }
    }

    /* At most three params are targeted per sample. Accumulate duplicate
     * routes without scanning all 64 params in the realtime path. */
    for (int i = 0; i < 3; ++i) {
        int pid = target_pid[i];
        if (pid < 0) continue;
        int already_applied = 0;
        float delta = 0.0f;
        for (int j = 0; j < 3; ++j) {
            if (target_pid[j] == pid) {
                if (j < i) already_applied = 1;
                delta += target_delta[j];
            }
        }
        if (already_applied) continue;
        int value = (int)lrintf(unmodulated[pid] + delta);
        int max = is_lfo_destination_param(pid) ? MONO_LFO_DESTINATIONS - 1 : 127;
        modulated[pid] = (uint8_t)iclamp(value, 0, max);
        if (pid >= 32 && pid < MONO_PRIMARY_PARAMS)
            next_lfo_mod[pid - 32] = delta;
    }
    memcpy(t->lfo_param_mod, next_lfo_mod, sizeof(next_lfo_mod));
    memcpy(t->effective, modulated, sizeof(modulated));

    if (t->gate_left > 0 && --t->gate_left == 0)
        env_release(&t->amp, t->effective[11], m->sample_rate);

    float port_sec = time_from_param(t->effective[15], 3.0f);
    if (port_sec > 0.0f)
        t->freq += (t->target_freq - t->freq) / fmaxf(1.0f, port_sec * m->sample_rate);
    else
        t->freq = t->target_freq;
    float tune = ((int)t->effective[7] - 64) / 64.0f;
    float freq = t->freq * powf(2.0f, (pitch_mod + tune) / 12.0f);
    const uint8_t *alt = t->effective + MONO_ALT_BASE;
    if (alt[4]) {
        phase_step(&t->mod_phase[3], 0.05f + 0.45f * pnorm(alt[4]), m->sample_rate);
        float drift = sinf(2.0f * (float)M_PI * t->mod_phase[3]) *
                      pnorm(alt[4]) * 0.125f;
        freq *= powf(2.0f, drift / 12.0f);
    }
    float x = oscillator(m, t, fclamp(freq, 1.0f, m->sample_rate * 0.45f));

    const uint8_t *ap = t->effective + 8;
    const uint8_t *fp = t->effective + 16;
    const uint8_t *ep = t->effective + 24;
    float aenv = amp_env_tick(&t->amp, ap, m->sample_rate);
    float fenv = filter_env_tick(&t->filter_env, fp, m->sample_rate);
    float vel = t->velocity / 127.0f;
    float drive = 1.0f + pnorm(ap[4]) * 15.0f;
    x = tanhf(x * drive) / tanhf(drive);
    x *= aenv * vel;

    float base = fp[0] + (((int)fp[6] - 64) * fenv);
    float width = fp[1] + (((int)fp[7] - 64) * fenv);
    base = fclamp(base, 0.0f, 127.0f);
    width = fclamp(width, 0.0f, 127.0f);
    float hp_hz = cutoff_from_param(base);
    float lp_param = base + (127.0f - base) * (width / 127.0f);
    float lp_hz = cutoff_from_param(lp_param);
    x = svf(&t->hp[0], x, hp_hz, pnorm(fp[2]), m->sample_rate, 1);
    x = svf(&t->hp[1], x, hp_hz, pnorm(fp[2]), m->sample_rate, 1);
    x = svf(&t->lp[0], x, lp_hz, pnorm(fp[3]), m->sample_rate, 0);
    x = svf(&t->lp[1], x, lp_hz, pnorm(fp[3]), m->sample_rate, 0);

    float eq_hz = cutoff_from_param(ep[0]);
    float band = svf(&t->eq, x, eq_hz, 0.4f, m->sample_rate, 0);
    x += band * (((int)ep[1] - 64) / 64.0f);

    int hold = 1 + (127 - ep[2]) / 4;
    if (t->srr_left-- <= 0) {
        t->srr_hold = roundf(x * 2047.0f) / 2047.0f;
        t->srr_left = hold;
    }
    x = t->srr_hold;

    float volume = pnorm(ap[5]);
    float pan = fclamp(((int)ap[6] - 64) / 64.0f, -1.0f, 1.0f);
    float lg = sqrtf(0.5f * (1.0f - pan));
    float rg = sqrtf(0.5f * (1.0f + pan));
    float left = x * volume * lg;
    *right = x * volume * rg;

    int delay_frames = m->sample_rate * MONO_DELAY_SECONDS;
    float delay_p = ep[4];
    int offset = (int)((0.015f + 1.82f * delay_p / 127.0f) * m->sample_rate);
    offset = iclamp(offset, 1, delay_frames - 1);
    int rp = t->delay_pos - offset;
    if (rp < 0) rp += delay_frames;
    float dl = t->delay[rp * 2];
    float dr = t->delay[rp * 2 + 1];
    float hp_a = pnorm(ep[6]) * 0.995f;
    float lp_a = 0.02f + pnorm(ep[7]) * 0.975f;
    float d[2] = { dl, dr };
    for (int c = 0; c < 2; ++c) {
        float h = hp_a * (t->delay_hp[c] + d[c] - t->delay_hp_in[c]);
        t->delay_hp_in[c] = d[c];
        t->delay_hp[c] = h;
        t->delay_lp[c] += lp_a * (h - t->delay_lp[c]);
        d[c] = t->delay_lp[c];
    }
    float send = pnorm(ep[3]);
    float fb = fclamp(pnorm(ep[5]) * 1.08f, 0.0f, 1.08f);
    t->delay[t->delay_pos * 2] = left * send + d[0] * fb;
    t->delay[t->delay_pos * 2 + 1] = *right * send + d[1] * fb;
    if (++t->delay_pos >= delay_frames) t->delay_pos = 0;
    left += dl * send;
    *right += dr * send;
    memcpy(t->effective, targets, sizeof(targets));
    return left;
}

void mono_advance_step(mono_t *m) {
    if (!m) return;
    m->seq_step = (m->seq_step + 1) % m->pattern_len;
    for (int ti = 0; ti < m->track_count; ++ti) {
        mono_track_t *t = &m->track[ti];
        mono_step_t *s = &t->steps[m->seq_step];
        if (s->note < 0 && s->trig_mask == 0 && s->lock_mask == 0) continue;
        memcpy(t->effective, t->base, MONO_PARAMS);
        for (int p = 0; p < MONO_PARAMS; ++p)
            if (s->lock_mask & (UINT64_C(1) << p)) t->effective[p] = s->lock_values[p];
        track_trigger(m, t, s->note, s->velocity,
                      s->trig_mask ? s->trig_mask : (s->note >= 0 ? 15 : 0), s->gate);
    }
}

static void internal_clock_tick(mono_t *m) {
    if (!m->transport) return;
    if (m->external_clock) {
        if (++m->external_clock_age < m->sample_rate) return;
        /* Clock cables/settings can change during a performance. After one
         * silent second, resume from project tempo instead of freezing. */
        m->external_clock = 0;
        m->internal_frames = 0;
    }
    double frames_per_step = m->sample_rate * 60.0 / (bpm_now(m) * 4.0);
    m->internal_frames += 1.0;
    if (m->internal_frames >= frames_per_step) {
        m->internal_frames -= frames_per_step;
        mono_advance_step(m);
    }
}

void mono_render(mono_t *m, int16_t *out_lr, int frames) {
    if (!m || !out_lr || frames <= 0) return;
    int peak = 0;
    for (int i = 0; i < frames; ++i) {
        internal_clock_tick(m);
        float l = 0.0f, r = 0.0f;
        for (int t = 0; t < m->track_count; ++t) {
            float tr = 0.0f;
            l += render_track(m, &m->track[t], &tr);
            r += tr;
        }
        l = tanhf(l * m->master);
        r = tanhf(r * m->master);
        if (!isfinite(l)) {
            l = 0.0f;
            ++m->nonfinite_samples;
        }
        if (!isfinite(r)) {
            r = 0.0f;
            ++m->nonfinite_samples;
        }
        out_lr[i * 2] = (int16_t)lrintf(fclamp(l, -1.0f, 1.0f) * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)lrintf(fclamp(r, -1.0f, 1.0f) * 32767.0f);
        int al = out_lr[i * 2] < 0 ? -out_lr[i * 2] : out_lr[i * 2];
        int ar = out_lr[i * 2 + 1] < 0 ? -out_lr[i * 2 + 1] : out_lr[i * 2 + 1];
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
    }
    m->render_peak = peak;
    ++m->render_blocks;
    if (peak > 0) ++m->nonzero_blocks;
    if (peak > m->lifetime_peak) m->lifetime_peak = peak;
}

void mono_on_midi(mono_t *m, const uint8_t *msg, int len, int source) {
    if (!m || !msg || len < 1) return;
    uint8_t st = msg[0];
    if (st == 0xF8) {
        m->external_clock = 1;
        m->external_clock_age = 0;
        if (m->transport && ++m->tick_in_step >= 6) {
            m->tick_in_step = 0;
            mono_advance_step(m);
        }
        return;
    }
    if (st == 0xFA) {
        m->transport = 1;
        m->seq_step = -1;
        m->tick_in_step = 0;
        m->internal_frames = 0;
        mono_advance_step(m);
        return;
    }
    if (st == 0xFB) { m->transport = 1; return; }
    if (st == 0xFC) {
        m->transport = 0;
        reset_effective_to_base(m);
        return;
    }
    if (len < 3) return;
    int kind = st & 0xF0;
    int channel = st & 0x0F;
    int track = m->track_count == 1 ? 0 : channel % m->track_count;
    int note = msg[1];
    if (m->track_count > 1 && source == MOVE_MIDI_SOURCE_INTERNAL) {
        /* Overtake receives raw Move pad note IDs. Let the DSP own the lower
         * three rows directly so sound never depends on an asynchronous JS
         * parameter round-trip; the top row remains reserved for controls. */
        if (note < 68 || note >= 92) return;
        track = m->selected_track;
        note = 48 + note - 68;
    }
    if (kind == 0x90 && msg[2] > 0) mono_note_on(m, track, note, msg[2]);
    else if (kind == 0x80 || (kind == 0x90 && msg[2] == 0)) mono_note_off(m, track, note);
}

static int param_id(const mono_t *m, const char *key) {
    int a, b;
    if (sscanf(key, "syn%d", &a) == 1 && a >= 1 && a <= 8) return a - 1;
    if (sscanf(key, "syn%d", &a) == 1 && a >= 9 && a <= 16)
        return MONO_ALT_BASE + a - 9;
    if (sscanf(key, "alt%d", &a) == 1 && a >= 1 && a <= MONO_ALT_PARAMS)
        return MONO_ALT_BASE + a - 1;
    if (sscanf(key, "amp%d", &a) == 1 && a >= 1 && a <= 8) return 8 + a - 1;
    if (sscanf(key, "flt%d", &a) == 1 && a >= 1 && a <= 8) return 16 + a - 1;
    if (sscanf(key, "fx%d", &a) == 1 && a >= 1 && a <= 8) return 24 + a - 1;
    if (sscanf(key, "lfo%d_%d", &a, &b) == 2 && a >= 1 && a <= 3 && b >= 1 && b <= 8)
        return (3 + a) * 8 + b - 1;
    if (sscanf(key, "p%d", &a) == 1 && a >= 1 && a <= 8)
        return m->selected_page * 8 + a - 1;
    return -1;
}

static void select_machine(mono_track_t *t, int machine) {
    machine = iclamp(machine, 0, MONO_MACHINE_COUNT - 1);
    machine_defaults(t, (mono_machine_t)machine);
}

static void reset_track_runtime(mono_t *m, mono_track_t *t, int index) {
    t->note = -1;
    t->last_note = 48 + index * 5;
    t->velocity = 0;
    t->gate_left = 0;
    t->freq = 0.0f;
    t->target_freq = 0.0f;
    memset(t->phase, 0, sizeof(t->phase));
    memset(t->mod_phase, 0, sizeof(t->mod_phase));
    t->fm_feedback = 0.0f;
    t->fm_feedback2 = 0.0f;
    memset(&t->amp, 0, sizeof(t->amp));
    memset(&t->filter_env, 0, sizeof(t->filter_env));
    for (int i = 0; i < 3; ++i) {
        t->lfo[i].phase = 0.0f;
        t->lfo[i].held = 0.0f;
        t->lfo[i].stopped = 0;
    }
    memset(t->lfo_param_mod, 0, sizeof(t->lfo_param_mod));
    memset(t->hp, 0, sizeof(t->hp));
    memset(t->lp, 0, sizeof(t->lp));
    memset(&t->eq, 0, sizeof(t->eq));
    t->srr_left = 0;
    t->srr_hold = 0.0f;
    t->delay_pos = 0;
    memset(t->delay_lp, 0, sizeof(t->delay_lp));
    memset(t->delay_hp, 0, sizeof(t->delay_hp));
    memset(t->delay_hp_in, 0, sizeof(t->delay_hp_in));
    if (t->delay)
        memset(t->delay, 0,
               (size_t)m->sample_rate * MONO_DELAY_SECONDS * 2 * sizeof(float));
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int read_hex(const char **cursor, const char *end, int digits,
                    uint64_t *value) {
    if (digits <= 0 || end - *cursor < digits) return 0;
    uint64_t result = 0;
    for (int i = 0; i < digits; ++i) {
        int digit = hex_digit((*cursor)[i]);
        if (digit < 0) return 0;
        result = (result << 4) | (uint64_t)digit;
    }
    *cursor += digits;
    *value = result;
    return 1;
}

/* Validate first, then call again with apply=1. The compact payload keeps a
 * fully populated six-track, 64-step pattern below the chain host's 16 KiB
 * state ceiling in the normal (sparse-lock) case. */
static int compact_state_pass(mono_t *m, const char *data, const char *end,
                              int saved_params, int mask_digits,
                              int legacy_destinations, int apply) {
    const char *p = data;
    unsigned seen_tracks = 0;
    while (p < end) {
        char record = *p++;
        uint64_t tr64;
        if (!read_hex(&p, end, 1, &tr64) || tr64 >= MONO_MAX_TRACKS) return 0;
        int tr = (int)tr64;
        if (record == 'T') {
            uint64_t machine64;
            uint8_t params[MONO_PARAMS];
            if (!read_hex(&p, end, 2, &machine64) ||
                machine64 >= MONO_MACHINE_COUNT) return 0;
            for (int i = 0; i < saved_params; ++i) {
                uint64_t param;
                if (!read_hex(&p, end, 2, &param) || param > 127) return 0;
                if (legacy_destinations && is_lfo_destination_param(i))
                    param = (uint64_t)migrate_legacy_lfo_destination((int)param);
                params[i] = (uint8_t)param;
            }
            if (tr < m->track_count) {
                seen_tracks |= 1u << tr;
                if (apply) {
                    mono_track_t *t = &m->track[tr];
                    machine_defaults(t, (mono_machine_t)machine64);
                    memcpy(t->base, params, (size_t)saved_params);
                    memcpy(t->effective, t->base, MONO_PARAMS);
                    sync_smoothed(t);
                }
            }
            continue;
        }
        if (record == 'S') {
            uint64_t step64, note64, velocity64, gate64, trig64, mask;
            if (!read_hex(&p, end, 2, &step64) || step64 >= MONO_STEPS ||
                !read_hex(&p, end, 2, &note64) ||
                (note64 != 255 && note64 > 127) ||
                !read_hex(&p, end, 2, &velocity64) || velocity64 > 127 ||
                !read_hex(&p, end, 2, &gate64) || gate64 > 127 ||
                !read_hex(&p, end, 2, &trig64) || trig64 > 15 ||
                !read_hex(&p, end, mask_digits, &mask)) return 0;
            mono_step_t *step = tr < m->track_count
                ? &m->track[tr].steps[(int)step64] : NULL;
            if (apply && step) {
                step->note = note64 == 255 ? -1 : (int8_t)note64;
                step->velocity = (uint8_t)velocity64;
                step->gate = (uint8_t)gate64;
                step->trig_mask = (uint8_t)trig64;
                step->lock_mask = mask;
            }
            for (int i = 0; i < saved_params; ++i) {
                if (!(mask & (UINT64_C(1) << i))) continue;
                uint64_t lock_value;
                if (!read_hex(&p, end, 2, &lock_value) || lock_value > 127) return 0;
                if (legacy_destinations && is_lfo_destination_param(i))
                    lock_value = (uint64_t)migrate_legacy_lfo_destination((int)lock_value);
                if (apply && step) step->lock_values[i] = (uint8_t)lock_value;
            }
            continue;
        }
        return 0;
    }
    unsigned wanted = (1u << m->track_count) - 1u;
    return p == end && (seen_tracks & wanted) == wanted;
}

static int json_int(const char *json, const char *key, int fallback) {
    const char *p = strstr(json, key);
    return p ? atoi(p + strlen(key)) : fallback;
}

static float json_float(const char *json, const char *key, float fallback) {
    const char *p = strstr(json, key);
    return p ? strtof(p + strlen(key), NULL) : fallback;
}

static void restore_state(mono_t *m, const char *json) {
    const char *tag = strstr(json, "\"data\":\"");
    if (!tag) return; /* Ignore the old display-only v1 state safely. */
    int version = json_int(json, "\"v\":", 0);
    if (version != 2 && version != 3 && version != 4) return;
    int saved_params = version >= 3 ? MONO_PARAMS : MONO_PRIMARY_PARAMS;
    int mask_digits = version >= 3 ? 16 : 14;
    int legacy_destinations = version < 4;
    const char *data = tag + strlen("\"data\":\"");
    const char *end = strchr(data, '"');
    if (!end || !compact_state_pass(m, data, end, saved_params, mask_digits,
                                    legacy_destinations, 0)) return;

    for (int tr = 0; tr < m->track_count; ++tr) {
        for (int step = 0; step < MONO_STEPS; ++step)
            clear_step(&m->track[tr].steps[step]);
        reset_track_runtime(m, &m->track[tr], tr);
    }
    if (!compact_state_pass(m, data, end, saved_params, mask_digits,
                            legacy_destinations, 1)) return;

    m->selected_track = iclamp(json_int(json, "\"track\":", 0),
                               0, m->track_count - 1);
    m->selected_page = iclamp(json_int(json, "\"page\":", 0),
                              0, MONO_PAGES - 1);
    m->step_page = iclamp(json_int(json, "\"step_page\":", 0), 0, 3);
    m->pattern_len = iclamp(json_int(json, "\"pattern_len\":", 16),
                            1, MONO_STEPS);
    m->master = fclamp(json_float(json, "\"master\":", 100.0f) / 100.0f,
                       0.0f, 2.0f);
    m->bpm_override = fclamp(json_float(json, "\"bpm_override\":", 0.0f),
                             0.0f, 400.0f);
    m->transport = 0;
    m->record_locks = 0;
    m->seq_step = -1;
    m->tick_in_step = 0;
    m->external_clock = 0;
    m->external_clock_age = 0;
    m->internal_frames = 0.0;
    changed(m);
}

static void set_lock(mono_t *m, const char *val, int clear) {
    int tr = m->selected_track, step = -1, pid = -1, value = 0;
    int n = sscanf(val, "%d:%d:%d:%d", &tr, &step, &pid, &value);
    if (n < 4) {
        tr = m->selected_track;
        n = sscanf(val, "%d:%d:%d", &step, &pid, &value);
    }
    if ((clear && n < 2) || (!clear && n < 3)) return;
    if (tr < 0 || tr >= m->track_count || step < 0 || step >= MONO_STEPS ||
        pid < 0 || pid >= MONO_PARAMS) return;
    mono_step_t *s = &m->track[tr].steps[step];
    if (clear) s->lock_mask &= ~(UINT64_C(1) << pid);
    else {
        s->lock_mask |= UINT64_C(1) << pid;
        int max = is_lfo_destination_param(pid) ? MONO_LFO_DESTINATIONS - 1 : 127;
        s->lock_values[pid] = (uint8_t)iclamp(value, 0, max);
    }
}

void mono_set_param(mono_t *m, const char *key, const char *val) {
    if (!m || !key || !val) return;
    if (!strcmp(key, "rui_set")) {
        const char *separator = strchr(val, ':');
        size_t key_len = separator ? (size_t)(separator - val) : 0;
        char routed_key[32];
        if (!separator || key_len == 0 || key_len >= sizeof(routed_key)) return;
        memcpy(routed_key, val, key_len);
        routed_key[key_len] = '\0';
        if (!strcmp(routed_key, "rui_set")) return;
        mono_set_param(m, routed_key, separator + 1);
        return;
    }
    if (!strcmp(key, "state")) { restore_state(m, val); return; }
    int v = atoi(val);
    if (!strcmp(key, "track")) { m->selected_track = iclamp(v, 0, m->track_count - 1); changed(m); return; }
    if (!strcmp(key, "page")) { m->selected_page = iclamp(v, 0, MONO_PAGES - 1); changed(m); return; }
    if (!strcmp(key, "step_page")) { m->step_page = iclamp(v, 0, 3); changed(m); return; }
    if (!strcmp(key, "pattern_len")) { m->pattern_len = iclamp(v, 1, MONO_STEPS); changed(m); return; }
    if (!strcmp(key, "bpm_override")) { m->bpm_override = fclamp(strtof(val, NULL), 0, 400); changed(m); return; }
    if (!strcmp(key, "master")) { m->master = fclamp(strtof(val, NULL) / 100.0f, 0, 2); changed(m); return; }
    if (!strcmp(key, "record")) { m->record_locks = v != 0; changed(m); return; }
    if (!strcmp(key, "transport")) {
        if (v && !m->transport) {
            m->seq_step = -1;
            m->internal_frames = 0;
            mono_advance_step(m);
        }
        m->transport = v != 0;
        if (!m->transport) {
            m->external_clock = 0;
            m->external_clock_age = 0;
            reset_effective_to_base(m);
        }
        changed(m);
        return;
    }
    mono_track_t *t = &m->track[m->selected_track];
    if (!strcmp(key, "machine")) { select_machine(t, v); changed(m); return; }
    int pid = param_id(m, key);
    if (pid >= 0) {
        int max = is_lfo_destination_param(pid) ? MONO_LFO_DESTINATIONS - 1 : 127;
        t->base[pid] = (uint8_t)iclamp(v, 0, max);
        t->effective[pid] = t->base[pid];
        if (m->record_locks && m->transport && m->seq_step >= 0) {
            mono_step_t *step = &t->steps[m->seq_step];
            step->lock_mask |= UINT64_C(1) << pid;
            step->lock_values[pid] = t->base[pid];
        }
        changed(m);
        return;
    }
    if (!strcmp(key, "toggle_step")) {
        int step = iclamp(v, 0, MONO_STEPS - 1);
        mono_step_t *s = &t->steps[step];
        if (s->note >= 0 || s->trig_mask) clear_step(s);
        else { s->note = (int8_t)t->last_note; s->velocity = 100; s->gate = 100; s->trig_mask = 15; }
        changed(m);
        return;
    }
    if (!strcmp(key, "set_step")) {
        int step, note, vel, gate, mask;
        if (sscanf(val, "%d:%d:%d:%d:%d", &step, &note, &vel, &gate, &mask) == 5 &&
            step >= 0 && step < MONO_STEPS) {
            mono_step_t *s = &t->steps[step];
            s->note = (int8_t)iclamp(note, -1, 127);
            s->velocity = (uint8_t)iclamp(vel, 1, 127);
            s->gate = (uint8_t)iclamp(gate, 1, 127);
            s->trig_mask = (uint8_t)iclamp(mask, 0, 15);
            changed(m);
        }
        return;
    }
    if (!strcmp(key, "lock")) { set_lock(m, val, 0); changed(m); return; }
    if (!strcmp(key, "unlock")) { set_lock(m, val, 1); changed(m); return; }
    if (!strcmp(key, "clear_pattern") && v) {
        for (int ti = 0; ti < m->track_count; ++ti)
            for (int s = 0; s < MONO_STEPS; ++s) clear_step(&m->track[ti].steps[s]);
        changed(m);
        return;
    }
    if (!strcmp(key, "note_on")) {
        int note = 60, vel = 100;
        sscanf(val, "%d:%d", &note, &vel);
        mono_note_on(m, m->selected_track, note, vel);
        return;
    }
    if (!strcmp(key, "note_off")) {
        mono_note_off(m, m->selected_track, v);
        return;
    }
}

int mono_get_param(mono_t *m, const char *key, char *buf, int buf_len) {
    if (!m || !key || !buf || buf_len <= 0) return -1;
    if (!strcmp(key, "track")) return snprintf(buf, (size_t)buf_len, "%d", m->selected_track);
    if (!strcmp(key, "page")) return snprintf(buf, (size_t)buf_len, "%d", m->selected_page);
    if (!strcmp(key, "step_page")) return snprintf(buf, (size_t)buf_len, "%d", m->step_page);
    if (!strcmp(key, "pattern_len")) return snprintf(buf, (size_t)buf_len, "%d", m->pattern_len);
    if (!strcmp(key, "transport")) return snprintf(buf, (size_t)buf_len, "%d", m->transport);
    if (!strcmp(key, "record")) return snprintf(buf, (size_t)buf_len, "%d", m->record_locks);
    if (!strcmp(key, "play_step")) return snprintf(buf, (size_t)buf_len, "%d", m->seq_step);
    if (!strcmp(key, "bpm")) return snprintf(buf, (size_t)buf_len, "%.1f", bpm_now(m));
    if (!strcmp(key, "bpm_override")) return snprintf(buf, (size_t)buf_len, "%.1f", m->bpm_override);
    if (!strcmp(key, "master")) return snprintf(buf, (size_t)buf_len, "%.0f", m->master * 100.0f);
    mono_track_t *t = &m->track[m->selected_track];
    if (!strcmp(key, "machine")) return snprintf(buf, (size_t)buf_len, "%d", t->machine);
    int pid = param_id(m, key);
    if (pid >= 0) {
        return snprintf(buf, (size_t)buf_len, "%d", t->base[pid]);
    }
    if (!strcmp(key, "steps")) {
        int n = 0;
        int first = m->step_page * 16;
        for (int i = 0; i < 16; ++i) {
            mono_step_t *s = &t->steps[first + i];
            int state = s->lock_mask ? 2 : ((s->note >= 0 || s->trig_mask) ? 1 : 0);
            int wrote = snprintf(buf + n, (size_t)(buf_len - n), "%s%d", i ? "," : "", state);
            if (wrote < 0 || wrote >= buf_len - n) break;
            n += wrote;
        }
        return n;
    }
    if (!strcmp(key, "status"))
        return snprintf(buf, (size_t)buf_len, "%d:%d:%.1f:%d:%d:%d:%d:%d",
                        m->transport, m->seq_step, bpm_now(m), m->selected_track,
                        m->selected_page, t->machine, m->pattern_len, m->record_locks);
    if (!strcmp(key, "rui_poll"))
        return snprintf(buf, (size_t)buf_len, "%u:%d:%d:%.0f:%d",
                        m->revision, m->transport, m->seq_step, bpm_now(m),
                        m->record_locks);
    if (!strcmp(key, "rui_play"))
        return snprintf(buf, (size_t)buf_len, "%d:%d:%.0f:%d",
                        m->transport, m->seq_step, bpm_now(m), m->record_locks);
    if (!strcmp(key, "debug"))
        return snprintf(buf, (size_t)buf_len,
                        "%u:%d:%d:%u:%u:%u:%d:%d:%d:%d:%d:%d:%d",
                        m->note_events, m->render_peak, m->lifetime_peak,
                        m->render_blocks, m->nonzero_blocks, m->nonfinite_samples,
                        m->sample_rate, t->note, t->velocity, t->amp.stage,
                        (int)lrintf(t->amp.value * 1000.0f),
                        (int)lrintf(t->freq * 10.0f), t->base[13]);
    if (!strcmp(key, "state")) {
        char step_csv[96];
        int sn = 0;
        int first = m->step_page * 16;
        for (int i = 0; i < 16 && sn < (int)sizeof(step_csv); ++i) {
            mono_step_t *s = &t->steps[first + i];
            int state = s->lock_mask ? 2 : ((s->note >= 0 || s->trig_mask) ? 1 : 0);
            int wrote = snprintf(step_csv + sn, sizeof(step_csv) - (size_t)sn,
                                 "%s%d", i ? "," : "", state);
            if (wrote < 0 || wrote >= (int)sizeof(step_csv) - sn) break;
            sn += wrote;
        }
        int n = 0;
        if (!appendf(buf, buf_len, &n,
                     "{\"v\":4,\"track\":%d,\"page\":%d,\"step_page\":%d,"
                     "\"pattern_len\":%d,\"master\":%.0f,\"bpm_override\":%.1f,"
                     "\"machine\":%d,\"p1\":%d,\"p2\":%d,\"p3\":%d,\"p4\":%d,"
                     "\"p5\":%d,\"p6\":%d,\"p7\":%d,\"p8\":%d,"
                     "\"alt1\":%d,\"alt2\":%d,\"alt3\":%d,\"alt4\":%d,"
                     "\"alt5\":%d,\"alt6\":%d,\"alt7\":%d,\"alt8\":%d,"
                     "\"record\":%d,"
                     "\"debug\":\"%u:%d:%d\","
                     "\"steps\":\"%s\",\"data\":\"",
                     m->selected_track, m->selected_page, m->step_page,
                     m->pattern_len, m->master * 100.0f, m->bpm_override,
                     t->machine,
                     t->base[m->selected_page * 8], t->base[m->selected_page * 8 + 1],
                     t->base[m->selected_page * 8 + 2], t->base[m->selected_page * 8 + 3],
                     t->base[m->selected_page * 8 + 4], t->base[m->selected_page * 8 + 5],
                     t->base[m->selected_page * 8 + 6], t->base[m->selected_page * 8 + 7],
                     t->base[MONO_ALT_BASE], t->base[MONO_ALT_BASE + 1],
                     t->base[MONO_ALT_BASE + 2], t->base[MONO_ALT_BASE + 3],
                     t->base[MONO_ALT_BASE + 4], t->base[MONO_ALT_BASE + 5],
                     t->base[MONO_ALT_BASE + 6], t->base[MONO_ALT_BASE + 7],
                     m->record_locks,
                     m->note_events, m->render_peak, m->lifetime_peak,
                     step_csv)) return -1;
        for (int tr = 0; tr < m->track_count; ++tr) {
            const mono_track_t *saved = &m->track[tr];
            if (!appendf(buf, buf_len, &n, "T%X%02X", tr, saved->machine)) return -1;
            for (int pid = 0; pid < MONO_PARAMS; ++pid)
                if (!appendf(buf, buf_len, &n, "%02X", saved->base[pid])) return -1;
            for (int si = 0; si < MONO_STEPS; ++si) {
                const mono_step_t *step = &saved->steps[si];
                if (step->note < 0 && !step->trig_mask && !step->lock_mask) continue;
                if (!appendf(buf, buf_len, &n,
                             "S%X%02X%02X%02X%02X%02X%016llX",
                             tr, si, (unsigned)(uint8_t)step->note,
                             step->velocity, step->gate, step->trig_mask,
                             (unsigned long long)step->lock_mask)) return -1;
                for (int pid = 0; pid < MONO_PARAMS; ++pid)
                    if (step->lock_mask & (UINT64_C(1) << pid))
                        if (!appendf(buf, buf_len, &n, "%02X",
                                     step->lock_values[pid])) return -1;
            }
        }
        if (!appendf(buf, buf_len, &n, "\"}")) return -1;
        return n;
    }
    return -1;
}

int mono_debug_effective_param(mono_t *m, int track, int pid) {
    if (!m || track < 0 || track >= m->track_count || pid < 0 || pid >= MONO_PARAMS)
        return -1;
    return m->track[track].effective[pid];
}

float mono_debug_smoothed_param(mono_t *m, int track, int pid) {
    if (!m || track < 0 || track >= m->track_count || pid < 0 || pid >= MONO_PARAMS)
        return -1.0f;
    return m->track[track].smoothed[pid];
}
