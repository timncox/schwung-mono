#include "mono_core.h"

#include <math.h>
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
    uint32_t noise;

    mono_env_t amp;
    mono_env_t filter_env;
    mono_lfo_t lfo[3];
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
    int seq_step;
    int tick_in_step;
    int external_clock;
    int external_clock_age;
    double internal_frames;
    float bpm_override;
    float master;
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
    t->machine = machine;
    memcpy(t->base, defs[machine], 8);
    memcpy(t->effective, t->base, MONO_PARAMS);
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
    float tune = ((int)t->effective[7] - 64) / 64.0f;
    t->target_freq = midi_freq(note + tune);
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
    memcpy(t->effective, t->base, MONO_PARAMS);
    track_trigger(m, t, iclamp(note, 0, 127), velocity, 15, 0);
    ++m->note_events;
}

void mono_note_off(mono_t *m, int track, int note) {
    if (!m || track < 0 || track >= m->track_count) return;
    mono_track_t *t = &m->track[track];
    if (note < 0 || t->note == note) {
        env_release(&t->amp, t->effective[11], m->sample_rate);
        t->gate_left = 0;
    }
}

static float osc_swavesaw(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    float width = 0.009f * pnorm(p[1]);
    float ext = width * 2.0f;
    float v = saw(t->phase[0]);
    phase_step(&t->phase[0], freq, m->sample_rate);
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
    v += pulse(t->phase[5], 0.5f) * pnorm(p[3]);
    v += sinf(2.0f * (float)M_PI * t->phase[6]) * pnorm(p[4]);
    v += sinf(2.0f * (float)M_PI * t->phase[7]) * pnorm(p[5]);
    return v / (1.0f + 2.0f * ul + 2.0f * ux + pnorm(p[3]) + pnorm(p[4]) + pnorm(p[5]));
}

static float osc_swavepulse(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    float det = 0.009f * pnorm(p[1]);
    float pw = fclamp(0.05f + 0.9f * pnorm(p[3]), 0.03f, 0.97f);
    float sweep = (pnorm(p[4]) - 0.5f) * 0.35f * sinf(2.0f * (float)M_PI * t->mod_phase[0]);
    phase_step(&t->mod_phase[0], 0.08f + 7.0f * pnorm(p[5]), m->sample_rate);
    pw = fclamp(pw + sweep, 0.03f, 0.97f);
    float v = 0.0f;
    float ratios[5] = { 1, 1-det, 1+det, 1-2*det, 1+2*det };
    float levels[5] = { 1, pnorm(p[0]), pnorm(p[0]), pnorm(p[2]), pnorm(p[2]) };
    float sum = 0.0f;
    for (int i = 0; i < 5; ++i) {
        phase_step(&t->phase[i], freq * ratios[i], m->sample_rate);
        v += pulse(t->phase[i], pw) * levels[i];
        sum += levels[i];
    }
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
    for (int i = 0; i < 4; ++i) {
        float semi = i == 0 ? 0.0f : chord_offset(p[i - 1]);
        float r = powf(2.0f, semi / 12.0f) * (1.0f + chorus * (i - 1.5f));
        phase_step(&t->phase[i], freq * r, m->sample_rate);
        float a = saw(t->phase[i]);
        float b = pulse(t->phase[i], pw);
        v += a + (b - a) * shape;
    }
    return v * 0.25f;
}

static float osc_sid(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
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
    if (mode == 1 || mode == 3) v *= t->mod_phase[0] < 0.5f ? -1.0f : 1.0f;
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
    return a + (b - a) * morph;
}

static float osc_fm(mono_t *m, mono_track_t *t, float freq) {
    const uint8_t *p = t->effective;
    static const float ratios[16] = {
        0.25f, 0.3333f, 0.5f, 0.6667f, 0.75f, 1, 1.25f, 1.5f,
        2, 2.5f, 3, 4, 5, 6, 8, 12
    };
    int r1 = (p[0] * 16) / 128;
    int r2 = (p[4] * 16) / 128;
    float fine = powf(2.0f, ((int)p[1] - 64) / (64.0f * 12.0f));
    phase_step(&t->mod_phase[0], freq * ratios[r1] * fine, m->sample_rate);
    phase_step(&t->mod_phase[1], freq * ratios[r2], m->sample_rate);
    float fb = pnorm(p[2]) * 3.5f;
    float m1 = sinf(2.0f * (float)M_PI * t->mod_phase[0] + t->fm_feedback * fb);
    float m2 = sinf(2.0f * (float)M_PI * t->mod_phase[1]);
    float index = pnorm(p[3]) * 8.0f;
    float index2 = pnorm(p[5]) * 8.0f;
    phase_step(&t->phase[0], freq, m->sample_rate);
    float v = sinf(2.0f * (float)M_PI * t->phase[0] + m1 * index + m2 * index2);
    t->fm_feedback = m1;
    float tone = 0.2f + 0.8f * pnorm(p[6]);
    return tanhf(v / tone) * tone;
}

static float oscillator(mono_t *m, mono_track_t *t, float freq) {
    switch (t->machine) {
    case MONO_SWAVE_PULSE: return osc_swavepulse(m, t, freq);
    case MONO_SWAVE_ENSEMBLE: return osc_ensemble(m, t, freq);
    case MONO_SID_6581: return osc_sid(m, t, freq);
    case MONO_DIGIPRO_WAVE: return osc_digipro(m, t, freq);
    case MONO_FM_STATIC: return osc_fm(m, t, freq);
    default: return osc_swavesaw(m, t, freq);
    }
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
    if (t->gate_left > 0 && --t->gate_left == 0)
        env_release(&t->amp, t->effective[11], m->sample_rate);

    float bpm = bpm_now(m);
    float pitch_mod = 0.0f, base_mod = 0.0f, width_mod = 0.0f;
    float vol_mod = 0.0f, pan_mod = 0.0f, delay_mod = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const uint8_t *lp = t->effective + (4 + i) * 8;
        int dest = (lp[0] * 8) / 128;
        float v = lfo_tick(&t->lfo[i], lp, bpm, m->sample_rate);
        switch (dest) {
        case 1: pitch_mod += v * 24.0f; break;
        case 2: base_mod += v * 64.0f; break;
        case 3: width_mod += v * 64.0f; break;
        case 4: vol_mod += v; break;
        case 5: pan_mod += v; break;
        case 6: delay_mod += v * 64.0f; break;
        default: break;
        }
    }

    float port_sec = time_from_param(t->effective[15], 3.0f);
    if (port_sec > 0.0f)
        t->freq += (t->target_freq - t->freq) / fmaxf(1.0f, port_sec * m->sample_rate);
    else
        t->freq = t->target_freq;
    float freq = t->freq * powf(2.0f, pitch_mod / 12.0f);
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

    float base = fp[0] + base_mod + (((int)fp[6] - 64) * fenv);
    float width = fp[1] + width_mod + (((int)fp[7] - 64) * fenv);
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

    float volume = fclamp(pnorm(ap[5]) * (1.0f + vol_mod), 0.0f, 1.5f);
    float pan = fclamp(((int)ap[6] - 64) / 64.0f + pan_mod, -1.0f, 1.0f);
    float lg = sqrtf(0.5f * (1.0f - pan));
    float rg = sqrtf(0.5f * (1.0f + pan));
    float left = x * volume * lg;
    *right = x * volume * rg;

    int delay_frames = m->sample_rate * MONO_DELAY_SECONDS;
    float delay_p = fclamp(ep[4] + delay_mod, 0.0f, 127.0f);
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
    if (st == 0xFC) { m->transport = 0; return; }
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
        s->lock_values[pid] = (uint8_t)iclamp(value, 0, 127);
    }
}

void mono_set_param(mono_t *m, const char *key, const char *val) {
    if (!m || !key || !val) return;
    int v = atoi(val);
    if (!strcmp(key, "track")) { m->selected_track = iclamp(v, 0, m->track_count - 1); changed(m); return; }
    if (!strcmp(key, "page")) { m->selected_page = iclamp(v, 0, MONO_PAGES - 1); changed(m); return; }
    if (!strcmp(key, "step_page")) { m->step_page = iclamp(v, 0, 3); changed(m); return; }
    if (!strcmp(key, "pattern_len")) { m->pattern_len = iclamp(v, 1, MONO_STEPS); changed(m); return; }
    if (!strcmp(key, "bpm_override")) { m->bpm_override = fclamp(strtof(val, NULL), 0, 400); changed(m); return; }
    if (!strcmp(key, "master")) { m->master = fclamp(strtof(val, NULL) / 100.0f, 0, 2); changed(m); return; }
    if (!strcmp(key, "transport")) {
        if (v && !m->transport) {
            m->seq_step = -1;
            m->internal_frames = 0;
            mono_advance_step(m);
        }
        m->transport = v != 0;
        if (!m->transport) { m->external_clock = 0; m->external_clock_age = 0; }
        changed(m);
        return;
    }
    mono_track_t *t = &m->track[m->selected_track];
    if (!strcmp(key, "machine")) { select_machine(t, v); changed(m); return; }
    int pid = param_id(m, key);
    if (pid >= 0) {
        t->base[pid] = (uint8_t)iclamp(v, 0, 127);
        t->effective[pid] = t->base[pid];
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
    if (!strcmp(key, "play_step")) return snprintf(buf, (size_t)buf_len, "%d", m->seq_step);
    if (!strcmp(key, "bpm")) return snprintf(buf, (size_t)buf_len, "%.1f", bpm_now(m));
    if (!strcmp(key, "bpm_override")) return snprintf(buf, (size_t)buf_len, "%.1f", m->bpm_override);
    if (!strcmp(key, "master")) return snprintf(buf, (size_t)buf_len, "%.0f", m->master * 100.0f);
    mono_track_t *t = &m->track[m->selected_track];
    if (!strcmp(key, "machine")) return snprintf(buf, (size_t)buf_len, "%d", t->machine);
    int pid = param_id(m, key);
    if (pid >= 0) return snprintf(buf, (size_t)buf_len, "%d", t->base[pid]);
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
        return snprintf(buf, (size_t)buf_len, "%d:%d:%.1f:%d:%d:%d:%d",
                        m->transport, m->seq_step, bpm_now(m), m->selected_track,
                        m->selected_page, t->machine, m->pattern_len);
    if (!strcmp(key, "rui_poll"))
        return snprintf(buf, (size_t)buf_len, "%u:%d:%d:%.0f",
                        m->revision, m->transport, m->seq_step, bpm_now(m));
    if (!strcmp(key, "rui_play"))
        return snprintf(buf, (size_t)buf_len, "%d:%d:%.0f",
                        m->transport, m->seq_step, bpm_now(m));
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
        return snprintf(buf, (size_t)buf_len,
                        "{\"track\":%d,\"page\":%d,\"step_page\":%d,"
                        "\"pattern_len\":%d,\"transport\":%d,\"play_step\":%d,"
                        "\"bpm\":%.1f,\"master\":%.0f,\"machine\":%d,"
                        "\"p1\":%d,\"p2\":%d,\"p3\":%d,\"p4\":%d,"
                        "\"p5\":%d,\"p6\":%d,\"p7\":%d,\"p8\":%d,"
                        "\"steps\":\"%s\","
                        "\"debug\":\"%u:%d:%d:%u:%u:%u:%d:%d:%d:%d:%d:%d:%d\"}",
                        m->selected_track, m->selected_page, m->step_page,
                        m->pattern_len, m->transport, m->seq_step,
                        bpm_now(m), m->master * 100.0f, t->machine,
                        t->base[m->selected_page * 8], t->base[m->selected_page * 8 + 1],
                        t->base[m->selected_page * 8 + 2], t->base[m->selected_page * 8 + 3],
                        t->base[m->selected_page * 8 + 4], t->base[m->selected_page * 8 + 5],
                        t->base[m->selected_page * 8 + 6], t->base[m->selected_page * 8 + 7],
                        step_csv, m->note_events, m->render_peak, m->lifetime_peak,
                        m->render_blocks, m->nonzero_blocks, m->nonfinite_samples,
                        m->sample_rate, t->note, t->velocity, t->amp.stage,
                        (int)lrintf(t->amp.value * 1000.0f),
                        (int)lrintf(t->freq * 10.0f), t->base[13]);
    }
    return -1;
}
