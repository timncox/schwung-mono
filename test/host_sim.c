#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mono_core.h"

#define BLOCK 128
#define SPECTRAL_N 4096

static int bpm_calls;
static float sim_bpm(void) { bpm_calls++; return 120.0f; }

static host_api_v1_t host = {
    .api_version = MOVE_PLUGIN_API_VERSION,
    .sample_rate = MOVE_SAMPLE_RATE,
    .frames_per_block = BLOCK,
    .get_bpm = sim_bpm
};

static int64_t render_energy(mono_t *m, int blocks) {
    int16_t out[BLOCK * 2];
    int64_t energy = 0;
    for (int b = 0; b < blocks; ++b) {
        memset(out, 0, sizeof(out));
        mono_render(m, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) energy += llabs(out[i]);
    }
    return energy;
}

static uint64_t render_hash(mono_t *m) {
    int16_t out[BLOCK * 2];
    uint64_t h = UINT64_C(1469598103934665603);
    for (int b = 0; b < 8; ++b) {
        mono_render(m, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) {
            h ^= (uint16_t)out[i];
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

static uint64_t render_hash_long(mono_t *m) {
    int16_t out[BLOCK * 2];
    uint64_t h = UINT64_C(1469598103934665603);
    for (int b = 0; b < 64; ++b) {
        mono_render(m, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) {
            h ^= (uint16_t)out[i];
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

static uint64_t raw_hash(mono_t *m, int track, float frequency, int frames) {
    float out[2048];
    assert(frames > 0 && frames <= (int)(sizeof(out) / sizeof(out[0])));
    mono_debug_render_oscillator(m, track, frequency, out, frames);
    uint64_t h = UINT64_C(1469598103934665603);
    for (int i = 0; i < frames; ++i) {
        uint32_t bits;
        memcpy(&bits, &out[i], sizeof(bits));
        h ^= bits;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static double spectral_bin(const float *samples, int count, int bin) {
    double real = 0.0, imag = 0.0;
    for (int i = 0; i < count; ++i) {
        double phase = 2.0 * M_PI * bin * i / count;
        real += samples[i] * cos(phase);
        imag -= samples[i] * sin(phase);
    }
    return 2.0 * hypot(real, imag) / count;
}

static int get_int(mono_t *m, const char *key) {
    char buf[256];
    assert(mono_get_param(m, key, buf, sizeof(buf)) >= 0);
    return atoi(buf);
}

static void get_string(mono_t *m, const char *key, char *buf, size_t size) {
    assert(mono_get_param(m, key, buf, (int)size) >= 0);
}

static int debug_note(mono_t *m) {
    char debug[160];
    unsigned events, blocks, nonzero, nonfinite;
    int peak, lifetime, sample_rate, note;
    get_string(m, "debug", debug, sizeof(debug));
    assert(sscanf(debug, "%u:%d:%d:%u:%u:%u:%d:%d", &events, &peak,
                  &lifetime, &blocks, &nonzero, &nonfinite, &sample_rate,
                  &note) == 8);
    return note;
}

static void test_all_machines_sound_distinct(void) {
    uint64_t hashes[MONO_MACHINE_COUNT];
    for (int machine = 0; machine < MONO_MACHINE_COUNT; ++machine) {
        mono_t *m = mono_create(&host, 1);
        assert(m);
        char v[16];
        snprintf(v, sizeof(v), "%d", machine);
        mono_set_param(m, "machine", v);
        mono_note_on(m, 0, 48, 112);
        assert(render_energy(m, 2) > 1000);
        hashes[machine] = render_hash(m);
        mono_destroy(m);
    }
    for (int i = 0; i < MONO_MACHINE_COUNT; ++i)
        for (int j = i + 1; j < MONO_MACHINE_COUNT; ++j)
            assert(hashes[i] != hashes[j]);
}

static void test_superwave_saw_rejects_folded_harmonics(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "machine", "0");
    mono_set_param(m, "syn1", "0");
    mono_set_param(m, "syn3", "0");
    mono_set_param(m, "syn4", "0");
    mono_set_param(m, "syn5", "0");
    mono_set_param(m, "syn6", "0");
    mono_set_param(m, "alt1", "0");
    float samples[SPECTRAL_N];
    const int fundamental_bin = 700;
    float frequency = host.sample_rate * fundamental_bin / (float)SPECTRAL_N;
    mono_debug_render_oscillator(m, 0, frequency, samples, SPECTRAL_N);
    double fundamental = spectral_bin(samples, SPECTRAL_N, fundamental_bin);
    /* The third harmonic lies above Nyquist and would fold to bin 1996 in a
     * naive saw. PolyBLEP should keep that false component well below the
     * actual fundamental. */
    double folded_third = spectral_bin(samples, SPECTRAL_N,
                                       SPECTRAL_N - fundamental_bin * 3);
    assert(fundamental > 0.25);
    assert(folded_third < fundamental * 0.15);
    mono_destroy(m);
}

static void test_superwave_pulse_primary_controls_are_wired(void) {
    const char *keys[] = { "syn3", "syn4", "syn5", "syn6" };
    for (int control = 0; control < 4; ++control) {
        mono_t *low = mono_create(&host, 1);
        mono_t *high = mono_create(&host, 1);
        assert(low && high);
        mono_set_param(low, "machine", "1");
        mono_set_param(high, "machine", "1");
        mono_set_param(low, keys[control], "0");
        mono_set_param(high, keys[control], "127");
        assert(raw_hash(low, 0, 220.0f, 2048) !=
               raw_hash(high, 0, 220.0f, 2048));
        mono_destroy(low);
        mono_destroy(high);
    }
}

static void configure_ensemble(mono_t *m) {
    mono_set_param(m, "machine", "2");
    mono_set_param(m, "alt1", "127");
    mono_set_param(m, "alt2", "127");
    mono_set_param(m, "alt3", "127");
    mono_set_param(m, "alt4", "127");
}

static void test_ensemble_wave_and_chorus_controls_are_wired(void) {
    const char *keys[] = { "syn4", "syn5", "syn6", "syn7" };
    for (int control = 0; control < 4; ++control) {
        mono_t *low = mono_create(&host, 1);
        mono_t *high = mono_create(&host, 1);
        assert(low && high);
        configure_ensemble(low);
        configure_ensemble(high);
        /* Chorus width needs an audible chorus level to expose its spread. */
        if (control == 3) {
            mono_set_param(low, "syn6", "127");
            mono_set_param(high, "syn6", "127");
        }
        mono_set_param(low, keys[control], "0");
        mono_set_param(high, keys[control], "127");
        assert(raw_hash(low, 0, 220.0f, 2048) !=
               raw_hash(high, 0, 220.0f, 2048));
        mono_destroy(low);
        mono_destroy(high);
    }
}

static mono_t *configured_cross_track_machine(int machine, int previous_note,
                                               int source_param) {
    mono_t *m = mono_create(&host, 2);
    assert(m);
    mono_note_on(m, 0, previous_note, 110);
    mono_set_param(m, "track", "1");
    char value[16];
    snprintf(value, sizeof(value), "%d", machine);
    mono_set_param(m, "machine", value);
    if (machine == MONO_SID_6581) {
        mono_set_param(m, "syn4", "32"); /* saw */
        mono_set_param(m, "syn5", "96"); /* hard sync */
        mono_set_param(m, "syn6", source_param ? "127" : "0");
        mono_set_param(m, "syn7", "64");
    } else {
        mono_set_param(m, "syn5", source_param ? "127" : "64");
        mono_set_param(m, "syn6", "64");
    }
    mono_note_on(m, 1, 60, 110);
    return m;
}

static void test_sid_and_digipro_previous_track_sources(void) {
    for (int machine = MONO_SID_6581; machine <= MONO_DIGIPRO_WAVE; ++machine) {
        mono_t *low = configured_cross_track_machine(machine, 36, 1);
        mono_t *high = configured_cross_track_machine(machine, 84, 1);
        assert(raw_hash(low, 1, 261.6256f, 2048) !=
               raw_hash(high, 1, 261.6256f, 2048));
        mono_destroy(low);
        mono_destroy(high);
    }

    /* MFRQ/SFRQ mode must remain independent of the previous track. */
    for (int machine = MONO_SID_6581; machine <= MONO_DIGIPRO_WAVE; ++machine) {
        mono_t *low = configured_cross_track_machine(machine, 36, 0);
        mono_t *high = configured_cross_track_machine(machine, 84, 0);
        assert(raw_hash(low, 1, 261.6256f, 2048) ==
               raw_hash(high, 1, 261.6256f, 2048));
        mono_destroy(low);
        mono_destroy(high);
    }
}

static uint64_t sweep_after_retrigger(int machine, int restart) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    char value[16];
    snprintf(value, sizeof(value), "%d", machine);
    mono_set_param(m, "machine", value);
    if (machine == MONO_SWAVE_PULSE) {
        mono_set_param(m, "syn5", "80");  /* PW */
        mono_set_param(m, "syn6", "127"); /* PWAD */
        mono_set_param(m, "syn7", restart ? "127" : "0");
    } else if (machine == MONO_SID_6581) {
        mono_set_param(m, "syn2", "127"); /* PWAD */
        mono_set_param(m, "syn3", restart ? "127" : "0");
        mono_set_param(m, "syn4", "64");  /* pulse */
    } else {
        mono_set_param(m, "syn2", "24");  /* WP */
        mono_set_param(m, "syn3", "112"); /* WPM */
        mono_set_param(m, "syn4", restart ? "127" : "0");
    }
    mono_note_on(m, 0, 60, 110);
    (void)raw_hash(m, 0, 261.6256f, 1536);
    mono_note_on(m, 0, 60, 110);
    uint64_t hash = raw_hash(m, 0, 261.6256f, 1536);
    mono_destroy(m);
    return hash;
}

static void test_sid_and_digipro_restart_their_sweeps(void) {
    assert(sweep_after_retrigger(MONO_SWAVE_PULSE, 0) !=
           sweep_after_retrigger(MONO_SWAVE_PULSE, 1));
    assert(sweep_after_retrigger(MONO_SID_6581, 0) !=
           sweep_after_retrigger(MONO_SID_6581, 1));
    assert(sweep_after_retrigger(MONO_DIGIPRO_WAVE, 0) !=
           sweep_after_retrigger(MONO_DIGIPRO_WAVE, 1));
}

static void test_fm_volume_envelope_and_tone_shape_spectrum(void) {
    mono_t *carrier = mono_create(&host, 1);
    mono_t *complex = mono_create(&host, 1);
    assert(carrier && complex);
    mono_set_param(carrier, "machine", "5");
    mono_set_param(complex, "machine", "5");
    mono_set_param(carrier, "syn4", "0");
    mono_set_param(carrier, "syn6", "0");
    mono_set_param(carrier, "syn7", "0");
    mono_set_param(carrier, "alt4", "0");
    mono_set_param(complex, "syn1", "56");
    mono_set_param(complex, "syn3", "96");
    mono_set_param(complex, "syn4", "127");
    mono_set_param(complex, "syn5", "56");
    mono_set_param(complex, "syn6", "112");
    mono_set_param(complex, "syn7", "127");
    float plain[SPECTRAL_N], rich[SPECTRAL_N];
    const int fundamental_bin = 200;
    float frequency = host.sample_rate * fundamental_bin / (float)SPECTRAL_N;
    mono_debug_render_oscillator(carrier, 0, frequency, plain, SPECTRAL_N);
    mono_debug_render_oscillator(complex, 0, frequency, rich, SPECTRAL_N);
    double plain_upper = 0.0, rich_upper = 0.0;
    for (int harmonic = 2; harmonic <= 6; ++harmonic) {
        plain_upper += spectral_bin(plain, SPECTRAL_N, fundamental_bin * harmonic);
        rich_upper += spectral_bin(rich, SPECTRAL_N, fundamental_bin * harmonic);
    }
    assert(spectral_bin(plain, SPECTRAL_N, fundamental_bin) > 0.5);
    assert(rich_upper > plain_upper * 4.0);
    mono_destroy(carrier);
    mono_destroy(complex);
}

static void test_filter_uses_octave_steps_and_key_tracking(void) {
    mono_t *middle = mono_create(&host, 1);
    mono_t *upper = mono_create(&host, 1);
    assert(middle && upper);
    mono_set_param(middle, "flt1", "0");
    mono_set_param(middle, "flt2", "8");
    mono_set_param(middle, "flt9", "127");
    mono_set_param(upper, "flt1", "0");
    mono_set_param(upper, "flt2", "8");
    mono_set_param(upper, "flt9", "127");
    mono_note_on(middle, 0, 60, 100);
    mono_note_on(upper, 0, 72, 100);
    (void)render_energy(middle, 100);
    (void)render_energy(upper, 100);
    float middle_hp, middle_lp, upper_hp, upper_lp;
    mono_debug_filter_cutoffs(middle, 0, &middle_hp, &middle_lp);
    mono_debug_filter_cutoffs(upper, 0, &upper_hp, &upper_lp);
    assert(fabsf(middle_lp / middle_hp - 2.0f) < 0.02f);
    assert(fabsf(upper_hp / middle_hp - 2.0f) < 0.02f);
    assert(fabsf(upper_lp / middle_lp - 2.0f) < 0.02f);
    mono_destroy(middle);
    mono_destroy(upper);
}

static void test_note_release(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "amp4", "0");
    mono_note_on(m, 0, 60, 100);
    int64_t live = render_energy(m, 4);
    assert(live > 1000);
    mono_note_off(m, 0, 60);
    (void)render_energy(m, 16);
    assert(render_energy(m, 2) < live / 100);
    mono_destroy(m);
}

static void test_held_note_priority_and_block_tempo_lookup(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "amp4", "0");
    mono_note_on(m, 0, 60, 90);
    mono_note_on(m, 0, 67, 110);
    mono_note_off(m, 0, 67);

    char debug[256];
    get_string(m, "debug", debug, sizeof(debug));
    unsigned events, blocks, nonzero, nonfinite;
    int peak, lifetime, sample_rate, note, velocity, stage, env, freq, volume;
    assert(sscanf(debug, "%u:%d:%d:%u:%u:%u:%d:%d:%d:%d:%d:%d:%d",
                  &events, &peak, &lifetime, &blocks, &nonzero, &nonfinite,
                  &sample_rate, &note, &velocity, &stage, &env, &freq,
                  &volume) == 13);
    assert(note == 60);
    assert(velocity == 90);
    int64_t live = render_energy(m, 1);
    assert(live > 1000);

    bpm_calls = 0;
    int16_t out[BLOCK * 2];
    mono_render(m, out, BLOCK);
    assert(bpm_calls == 1);

    mono_note_off(m, 0, 60);
    (void)render_energy(m, 16);
    assert(render_energy(m, 2) < live / 100);
    mono_destroy(m);
}

static void test_filters_remain_finite_and_retrigger(void) {
    for (int machine = 0; machine < MONO_MACHINE_COUNT; ++machine) {
        mono_t *m = mono_create(&host, 1);
        assert(m);
        char value[16];
        snprintf(value, sizeof(value), "%d", machine);
        mono_set_param(m, "machine", value);
        mono_note_on(m, 0, 48 + machine, 112);
        assert(render_energy(m, 400) > 1000);

        char debug[256];
        unsigned notes, blocks, nonzero, nonfinite;
        int peak, lifetime;
        get_string(m, "debug", debug, sizeof(debug));
        assert(sscanf(debug, "%u:%d:%d:%u:%u:%u",
                      &notes, &peak, &lifetime, &blocks, &nonzero,
                      &nonfinite) == 6);
        assert(notes == 1);
        assert(lifetime > 0);
        assert(nonzero > 0);
        assert(nonfinite == 0);

        mono_note_on(m, 0, 60 + machine, 112);
        assert(render_energy(m, 4) > 1000);
        mono_destroy(m);
    }
}

static void test_parameter_aliases(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "page", "2");
    mono_set_param(m, "p1", "45");
    assert(get_int(m, "flt1") == 45);
    mono_set_param(m, "syn3", "99");
    assert(get_int(m, "syn3") == 99);
    mono_set_param(m, "machine", "4");
    assert(get_int(m, "machine") == 4);
    mono_set_param(m, "lfo1_1", "18");
    assert(get_int(m, "lfo1_1") == 18);
    mono_set_param(m, "page", "4");
    assert(get_int(m, "p1") == 18);
    mono_set_param(m, "p1", "30");
    assert(get_int(m, "lfo1_1") == 30);
    mono_set_param(m, "syn9", "73");
    assert(get_int(m, "alt1") != 73); /* ALT follows the selected Shift page. */
    mono_set_param(m, "alt1", "41");
    assert(get_int(m, "alt1") == 41);
    assert(get_int(m, "lfo1_9") == 41);
    assert(get_int(m, "syn9") == 73);
    mono_set_param(m, "page", "1");
    mono_set_param(m, "amp10", "87");
    assert(get_int(m, "alt2") == 87);
    mono_set_param(m, "page", "2");
    mono_set_param(m, "alt8", "52");
    assert(get_int(m, "flt16") == 52);
    mono_set_param(m, "page", "3");
    mono_set_param(m, "fx15", "61");
    assert(get_int(m, "alt7") == 61);
    mono_set_param(m, "page", "0");
    assert(get_int(m, "alt1") == 73);
    mono_set_param(m, "alt8", "19");
    assert(get_int(m, "syn16") == 19);
    mono_destroy(m);
}

static void test_all_lfo_destinations_round_trip(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "page", "4");
    for (int destination = 0; destination < MONO_LFO_DESTINATIONS; ++destination) {
        char value[16];
        snprintf(value, sizeof(value), "%d", destination);
        mono_set_param(m, "lfo1_1", value);
        assert(get_int(m, "lfo1_1") == destination);
        assert(get_int(m, "p1") == destination);
    }
    mono_set_param(m, "lfo1_1", "127");
    assert(get_int(m, "lfo1_1") == MONO_LFO_DESTINATIONS - 1);
    mono_destroy(m);
}

static void configure_cross_lfo(mono_t *m, int cross_depth) {
    char depth[16];
    snprintf(depth, sizeof(depth), "%d", cross_depth);
    mono_set_param(m, "flt1", "52");
    mono_set_param(m, "flt2", "50");
    mono_set_param(m, "lfo1_1", "46"); /* LFO 2 Speed (pid 44). */
    mono_set_param(m, "lfo1_3", "32");
    mono_set_param(m, "lfo1_5", "80");
    mono_set_param(m, "lfo1_7", depth);
    mono_set_param(m, "lfo2_1", "18"); /* Filter Base (pid 16). */
    mono_set_param(m, "lfo2_3", "64");
    mono_set_param(m, "lfo2_5", "24");
    mono_set_param(m, "lfo2_7", "100");
    mono_note_on(m, 0, 48, 112);
}

static void test_lfo_can_modulate_lfo_settings(void) {
    mono_t *plain = mono_create(&host, 1);
    mono_t *cross = mono_create(&host, 1);
    assert(plain && cross);
    configure_cross_lfo(plain, 0);
    configure_cross_lfo(cross, 127);
    assert(render_hash_long(plain) != render_hash_long(cross));
    mono_destroy(plain);
    mono_destroy(cross);

    mono_t *self = mono_create(&host, 1);
    assert(self);
    mono_set_param(self, "lfo1_1", "34"); /* LFO 1 Destination itself. */
    mono_set_param(self, "lfo1_7", "127");
    mono_note_on(self, 0, 48, 112);
    assert(render_energy(self, 400) > 1000);
    char debug[256];
    unsigned notes, blocks, nonzero, nonfinite;
    int peak, lifetime;
    get_string(self, "debug", debug, sizeof(debug));
    assert(sscanf(debug, "%u:%d:%d:%u:%u:%u", &notes, &peak, &lifetime,
                  &blocks, &nonzero, &nonfinite) == 6);
    assert(nonfinite == 0);
    mono_destroy(self);
}

static void test_shift_layer_controls_are_audible(void) {
    for (int machine = 0; machine < MONO_MACHINE_COUNT; ++machine) {
        mono_t *baseline = mono_create(&host, 1);
        mono_t *changed = mono_create(&host, 1);
        assert(baseline && changed);
        char value[16], key[16];
        snprintf(value, sizeof(value), "%d", machine);
        mono_set_param(baseline, "machine", value);
        mono_set_param(changed, "machine", value);
        for (int i = 0; i < 8; ++i) {
            snprintf(key, sizeof(key), "syn%d", i + 1);
            mono_set_param(baseline, key, "64");
            mono_set_param(changed, key, "64");
        }
        for (int i = 0; i < 4; ++i) {
            snprintf(key, sizeof(key), "alt%d", i + 1);
            mono_set_param(changed, key, get_int(changed, key) == 127 ? "0" : "127");
        }
        mono_note_on(baseline, 0, 48, 112);
        mono_note_on(changed, 0, 48, 112);
        uint64_t baseline_hash = render_hash_long(baseline);
        uint64_t changed_hash = render_hash_long(changed);
        if (baseline_hash == changed_hash)
            fprintf(stderr, "inaudible machine shift bank: machine=%d\n", machine);
        assert(baseline_hash != changed_hash);
        mono_destroy(baseline);
        mono_destroy(changed);
    }

    for (int alt = 5; alt <= 8; ++alt) {
        mono_t *baseline = mono_create(&host, 1);
        mono_t *changed = mono_create(&host, 1);
        assert(baseline && changed);
        char key[16];
        snprintf(key, sizeof(key), "alt%d", alt);
        mono_set_param(changed, key, "127");
        mono_note_on(baseline, 0, 48, 112);
        mono_note_on(changed, 0, 48, 112);
        assert(render_hash_long(baseline) != render_hash_long(changed));
        mono_destroy(baseline);
        mono_destroy(changed);
    }
}

static void test_every_secondary_page_is_audible(void) {
    for (int page = 1; page < MONO_PAGES; ++page) {
        mono_t *baseline = mono_create(&host, 1);
        mono_t *changed = mono_create(&host, 1);
        assert(baseline && changed);
        char value[16], key[16];
        snprintf(value, sizeof(value), "%d", page);
        mono_set_param(baseline, "page", value);
        mono_set_param(changed, "page", value);

        if (page == 1) {
            mono_set_param(baseline, "amp1", "64");
            mono_set_param(changed, "amp1", "64");
        } else if (page == 2) {
            mono_set_param(baseline, "flt1", "40");
            mono_set_param(baseline, "flt2", "80");
            mono_set_param(changed, "flt1", "40");
            mono_set_param(changed, "flt2", "80");
        } else if (page == 3) {
            mono_set_param(baseline, "fx4", "100");
            mono_set_param(changed, "fx4", "100");
        } else {
            int lfo = page - 3;
            snprintf(key, sizeof(key), "lfo%d_1", lfo);
            mono_set_param(baseline, key, "18");
            mono_set_param(changed, key, "18");
            snprintf(key, sizeof(key), "lfo%d_7", lfo);
            mono_set_param(baseline, key, "110");
            mono_set_param(changed, key, "110");
        }

        for (int i = 0; i < 8; ++i) {
            snprintf(key, sizeof(key), "alt%d", i + 1);
            int current = get_int(changed, key);
            snprintf(value, sizeof(value), "%d", current > 63 ? 0 : 127);
            mono_set_param(changed, key, value);
        }
        mono_note_on(baseline, 0, 48, 112);
        mono_note_on(changed, 0, 48, 112);
        uint64_t plain = render_hash_long(baseline);
        uint64_t shifted = render_hash_long(changed);
        if (plain == shifted) fprintf(stderr, "inaudible secondary page: %d\n", page);
        assert(plain != shifted);
        mono_destroy(baseline);
        mono_destroy(changed);
    }
}

static void test_sequencer_and_lock(void) {
    mono_t *m = mono_create(&host, 6);
    assert(m);
    mono_set_param(m, "track", "0");
    mono_set_param(m, "page", "0");
    mono_set_param(m, "syn1", "12");
    mono_set_param(m, "set_step", "0:60:110:100:15");
    mono_set_param(m, "lock", "0:0:0:101");
    assert(get_int(m, "syn1") == 12); /* a lock must not mutate the kit */
    mono_set_param(m, "step_page", "0");
    char steps[128];
    assert(mono_get_param(m, "steps", steps, sizeof(steps)) > 0);
    assert(!strncmp(steps, "2,", 2));

    mono_set_param(m, "transport", "1");
    assert(get_int(m, "play_step") == 0);
    for (int i = 0; i < 6; ++i) {
        uint8_t tick = 0xF8;
        mono_on_midi(m, &tick, 1, MOVE_MIDI_SOURCE_HOST);
    }
    assert(get_int(m, "play_step") == 1);
    assert(render_energy(m, 4) > 1000);
    mono_destroy(m);
}

static void test_pattern_window_and_play_orders(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "pattern_start", "16");
    mono_set_param(m, "pattern_len", "4");

    mono_set_param(m, "play_order", "0");
    mono_set_param(m, "transport", "1");
    assert(get_int(m, "play_step") == 16);
    mono_advance_step(m); assert(get_int(m, "play_step") == 17);
    mono_advance_step(m); assert(get_int(m, "play_step") == 18);
    mono_advance_step(m); assert(get_int(m, "play_step") == 19);
    mono_advance_step(m); assert(get_int(m, "play_step") == 16);

    mono_set_param(m, "play_order", "1");
    mono_advance_step(m); assert(get_int(m, "play_step") == 19);
    mono_advance_step(m); assert(get_int(m, "play_step") == 18);
    mono_advance_step(m); assert(get_int(m, "play_step") == 17);
    mono_advance_step(m); assert(get_int(m, "play_step") == 16);
    mono_advance_step(m); assert(get_int(m, "play_step") == 19);

    mono_set_param(m, "play_order", "2");
    const int pendulum[] = {16, 17, 18, 19, 18, 17, 16, 17};
    for (size_t i = 0; i < sizeof(pendulum) / sizeof(pendulum[0]); ++i) {
        mono_advance_step(m);
        assert(get_int(m, "play_step") == pendulum[i]);
    }

    mono_set_param(m, "play_order", "3");
    int seen = 0;
    for (int i = 0; i < 32; ++i) {
        mono_advance_step(m);
        int step = get_int(m, "play_step");
        assert(step >= 16 && step <= 19);
        seen |= 1 << (step - 16);
    }
    assert((seen & (seen - 1)) != 0);

    mono_set_param(m, "step_page", "3");
    mono_set_param(m, "toggle_step", "63");
    char all_steps[256];
    get_string(m, "all_steps", all_steps, sizeof(all_steps));
    assert(strlen(all_steps) == MONO_STEPS * 2 - 1);
    assert(all_steps[strlen(all_steps) - 1] == '1');
    mono_destroy(m);
}

static void test_per_track_windows_rotation_division_and_swing(void) {
    mono_t *m = mono_create(&host, 2);
    assert(m);
    mono_set_param(m, "pattern_start", "0");
    mono_set_param(m, "pattern_len", "8");
    mono_set_param(m, "track", "1");
    mono_set_param(m, "track_start", "16");
    mono_set_param(m, "track_len", "3");
    mono_set_param(m, "track_rotate", "1");
    mono_set_param(m, "track_div", "2");
    assert(get_int(m, "track_follow") == 0);

    mono_set_param(m, "transport", "1");
    mono_set_param(m, "track", "0");
    assert(get_int(m, "track_play_step") == 0);
    mono_set_param(m, "track", "1");
    assert(get_int(m, "track_play_step") == 17);
    mono_advance_step(m);
    assert(get_int(m, "track_play_step") == 17); /* divided clock holds */
    mono_advance_step(m);
    assert(get_int(m, "track_play_step") == 18);
    mono_set_param(m, "track_follow", "1");
    assert(get_int(m, "track_follow") == 1);
    assert(get_int(m, "track_start") == 0);
    assert(get_int(m, "track_len") == 8);

    mono_set_param(m, "swing", "127");
    mono_set_param(m, "transport", "0");
    uint8_t start = 0xFA, tick = 0xF8;
    mono_on_midi(m, &start, 1, MOVE_MIDI_SOURCE_HOST);
    assert(get_int(m, "play_step") == 0);
    for (int i = 0; i < 8; ++i) mono_on_midi(m, &tick, 1, MOVE_MIDI_SOURCE_HOST);
    assert(get_int(m, "play_step") == 0);
    mono_on_midi(m, &tick, 1, MOVE_MIDI_SOURCE_HOST);
    assert(get_int(m, "play_step") == 1);
    mono_destroy(m);
}

static void test_step_track_copy_paste_and_undo(void) {
    mono_t *m = mono_create(&host, 2);
    assert(m);
    mono_set_param(m, "track", "0");
    mono_set_param(m, "machine", "3");
    mono_set_param(m, "set_step", "2:64:111:80:15");
    mono_set_param(m, "lock", "0:2:56:99");
    mono_set_param(m, "copy_step", "0:2");
    assert(get_int(m, "can_paste_step") == 1);
    mono_set_param(m, "paste_step", "1:9");
    mono_set_param(m, "track", "1");
    mono_set_param(m, "step_page", "0");
    char steps[128];
    get_string(m, "steps", steps, sizeof(steps));
    assert(steps[18] == '2'); /* step 10 is locked */
    mono_set_param(m, "undo", "1");
    get_string(m, "steps", steps, sizeof(steps));
    assert(steps[18] == '0');
    mono_set_param(m, "undo", "1");
    get_string(m, "steps", steps, sizeof(steps));
    assert(steps[18] == '2'); /* undo toggles redo */

    mono_set_param(m, "track", "0");
    mono_set_param(m, "copy_track", "0");
    mono_set_param(m, "paste_track", "1");
    mono_set_param(m, "track", "1");
    assert(get_int(m, "machine") == 3);
    mono_set_param(m, "clear_step", "1:2");
    get_string(m, "steps", steps, sizeof(steps));
    assert(steps[4] == '0');
    mono_set_param(m, "undo", "1");
    get_string(m, "steps", steps, sizeof(steps));
    assert(steps[4] == '2');
    mono_destroy(m);
}

static void test_step_probability_condition_retrig_and_slide_state(void) {
    mono_t *m = mono_create(&host, 1);
    mono_t *restored = mono_create(&host, 1);
    assert(m && restored);
    mono_set_param(m, "set_step", "0:48:110:100:15");
    mono_set_param(m, "edit_step", "0");
    mono_set_param(m, "step_probability", "0");
    mono_set_param(m, "step_retrig", "4");
    mono_set_param(m, "step_condition", "3");
    mono_set_param(m, "step_slide", "96");
    assert(get_int(m, "step_probability") == 0);
    assert(get_int(m, "step_retrig") == 4);
    assert(get_int(m, "step_condition") == 3);
    assert(get_int(m, "step_slide") == 96);

    char state[16384];
    assert(mono_get_param(m, "state", state, sizeof(state)) > 0);
    mono_set_param(restored, "state", state);
    mono_set_param(restored, "edit_step", "0");
    assert(get_int(restored, "step_probability") == 0);
    assert(get_int(restored, "step_retrig") == 4);
    assert(get_int(restored, "step_condition") == 3);
    assert(get_int(restored, "step_slide") == 96);

    mono_set_param(m, "play_order", "0");
    mono_set_param(m, "transport", "1");
    assert(render_energy(m, 8) == 0); /* zero probability suppresses the trig */
    mono_destroy(restored);
    mono_destroy(m);
}

static void test_live_parameter_recording_and_smoothing(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "page", "2");
    mono_set_param(m, "p1", "20");
    (void)render_energy(m, 200);
    assert(fabsf(mono_debug_smoothed_param(m, 0, 16) - 20.0f) < 0.1f);

    mono_set_param(m, "record", "1");
    mono_set_param(m, "transport", "1");
    assert(get_int(m, "record") == 1);
    assert(get_int(m, "play_step") == 0);
    mono_set_param(m, "p1", "100");
    assert(mono_debug_effective_param(m, 0, 16) == 100);
    char steps[128];
    get_string(m, "steps", steps, sizeof(steps));
    assert(!strncmp(steps, "2,", 2));

    (void)render_energy(m, 1);
    float moving = mono_debug_smoothed_param(m, 0, 16);
    assert(moving > 20.0f && moving < 100.0f);
    (void)render_energy(m, 200);
    assert(fabsf(mono_debug_smoothed_param(m, 0, 16) - 100.0f) < 0.1f);

    /* Incoming Move notes must not erase the current automation target. */
    uint8_t note_on[3] = {0x90, 60, 112};
    mono_on_midi(m, note_on, 3, MOVE_MIDI_SOURCE_INTERNAL);
    assert(mono_debug_effective_param(m, 0, 16) == 100);

    mono_set_param(m, "record", "0");
    mono_set_param(m, "p1", "20");
    mono_set_param(m, "transport", "0");
    assert(mono_debug_effective_param(m, 0, 16) == 20);
    (void)render_energy(m, 200);
    assert(fabsf(mono_debug_smoothed_param(m, 0, 16) - 20.0f) < 0.1f);

    mono_set_param(m, "transport", "1");
    assert(mono_debug_effective_param(m, 0, 16) == 100);
    (void)render_energy(m, 1);
    moving = mono_debug_smoothed_param(m, 0, 16);
    assert(moving > 20.0f && moving < 100.0f);

    /* The arm switch is a performance state, never recalled with a patch. */
    char state[4096];
    assert(mono_get_param(m, "state", state, sizeof(state)) > 0);
    mono_set_param(m, "record", "1");
    mono_set_param(m, "state", state);
    assert(get_int(m, "record") == 0);
    mono_destroy(m);
}

static void test_internal_clock(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "set_step", "0:48:100:100:15");
    mono_set_param(m, "transport", "1");
    int16_t out[BLOCK * 2];
    for (int i = 0; i < 90; ++i) mono_render(m, out, BLOCK);
    assert(get_int(m, "play_step") >= 0);
    mono_destroy(m);
}

static void test_clock_loss_falls_back(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "set_step", "0:48:100:100:15");
    mono_set_param(m, "set_step", "1:55:100:100:15");
    mono_set_param(m, "transport", "1");
    uint8_t tick = 0xF8;
    mono_on_midi(m, &tick, 1, MOVE_MIDI_SOURCE_HOST);
    int16_t out[BLOCK * 2];
    for (int i = 0; i < 420; ++i) mono_render(m, out, BLOCK);
    assert(get_int(m, "play_step") >= 1);
    mono_destroy(m);
}

static void test_six_tracks_render_together(void) {
    mono_t *m = mono_create(&host, 6);
    assert(m);
    for (int track = 0; track < 6; ++track) {
        char v[16];
        snprintf(v, sizeof(v), "%d", track);
        mono_set_param(m, "track", v);
        snprintf(v, sizeof(v), "%d", track % MONO_MACHINE_COUNT);
        mono_set_param(m, "machine", v);
        mono_note_on(m, track, 42 + track * 4, 100);
    }
    assert(render_energy(m, 32) > 100000);
    mono_destroy(m);
}

static void test_production_event_paths(void) {
    uint8_t note_on[3] = {0x90, 60, 112};
    mono_t *voice = mono_create(&host, 1);
    assert(voice);
    mono_on_midi(voice, note_on, 3, MOVE_MIDI_SOURCE_INTERNAL);
    assert(render_energy(voice, 4) > 1000);
    char debug[64];
    get_string(voice, "debug", debug, sizeof(debug));
    assert(!strncmp(debug, "1:", 2));
    mono_destroy(voice);

    mono_t *full = mono_create(&host, 6);
    assert(full);
    uint8_t performance_pad[3] = {0x90, 68, 112};
    mono_on_midi(full, performance_pad, 3, MOVE_MIDI_SOURCE_INTERNAL);
    assert(render_energy(full, 4) > 1000);
    mono_destroy(full);

    full = mono_create(&host, 6);
    assert(full);
    uint8_t control_pad[3] = {0x90, 92, 112};
    mono_on_midi(full, control_pad, 3, MOVE_MIDI_SOURCE_INTERNAL);
    assert(render_energy(full, 4) == 0);
    mono_destroy(full);
}

static void test_remote_state_contract(void) {
    mono_t *m = mono_create(&host, 6);
    assert(m);
    mono_set_param(m, "rui_set", "track:2");
    mono_set_param(m, "rui_set", "page:3");
    mono_set_param(m, "rui_set", "pattern_start:8");
    mono_set_param(m, "rui_set", "pattern_len:24");
    mono_set_param(m, "rui_set", "play_order:3");
    mono_set_param(m, "rui_set", "p4:99");
    mono_set_param(m, "rui_set", "toggle_step:0");
    mono_set_param(m, "rui_set", "rui_set:track:0");
    mono_set_param(m, "note_on", "60:112");
    assert(render_energy(m, 4) > 1000);

    char state[16384], poll[128];
    get_string(m, "state", state, sizeof(state));
    get_string(m, "rui_poll", poll, sizeof(poll));
    assert(strstr(state, "\"track\":2"));
    assert(strstr(state, "\"page\":3"));
    assert(strstr(state, "\"pattern_start\":8"));
    assert(strstr(state, "\"pattern_len\":24"));
    assert(strstr(state, "\"play_order\":3"));
    assert(strstr(state, "\"p4\":99"));
    assert(strstr(state, "\"steps\":\"1,"));
    assert(strstr(state, "\"all_steps\":\"1,"));
    assert(strstr(state, "\"debug\":\"1:"));
    assert(strchr(poll, ':'));
    mono_set_param(m, "note_off", "60");
    mono_destroy(m);
}

static void test_full_state_round_trip(void) {
    mono_t *source = mono_create(&host, 6);
    mono_t *restored = mono_create(&host, 6);
    assert(source && restored);

    mono_set_param(source, "pattern_len", "64");
    mono_set_param(source, "pattern_start", "12");
    mono_set_param(source, "pattern_len", "20");
    mono_set_param(source, "play_order", "2");
    mono_set_param(source, "master", "137");
    mono_set_param(source, "bpm_override", "123.5");
    mono_set_param(source, "track", "0");
    mono_set_param(source, "machine", "2");
    mono_set_param(source, "syn1", "17");
    mono_set_param(source, "set_step", "0:48:101:77:15");
    mono_set_param(source, "lock", "0:0:0:99");
    mono_set_param(source, "lock", "0:0:32:48");
    mono_set_param(source, "track", "4");
    mono_set_param(source, "machine", "5");
    mono_set_param(source, "page", "6");
    mono_set_param(source, "p8", "111");
    mono_set_param(source, "alt1", "87");
    mono_set_param(source, "alt8", "29");
    mono_set_param(source, "set_step", "63:72:127:64:9");
    mono_set_param(source, "lock", "4:63:55:33");
    mono_set_param(source, "lock", "4:63:111:91");
    mono_set_param(source, "edit_step", "63");
    mono_set_param(source, "step_micro", "-13");
    mono_set_param(source, "step_tie", "1");
    mono_set_param(source, "step_accent", "76");
    mono_set_param(source, "arp_enabled", "1");
    mono_set_param(source, "arp_latch", "1");
    mono_set_param(source, "arp_mode", "5");
    mono_set_param(source, "arp_offset", "3:-7");
    mono_set_param(source, "route_mode", "4");
    mono_set_param(source, "route_amount", "93");
    mono_set_param(source, "track_fx_type", "4");
    mono_set_param(source, "track_fx_mix", "88");
    mono_set_param(source, "morph_capture_a", "1");
    mono_set_param(source, "syn1", "62");
    mono_set_param(source, "morph_capture_b", "1");
    mono_set_param(source, "morph", "32");
    mono_set_param(source, "song_edit_row", "1");
    mono_set_param(source, "song_length", "2");
    mono_set_param(source, "song_start", "24");
    mono_set_param(source, "song_row_length", "8");
    mono_set_param(source, "song_repeats", "3");
    mono_set_param(source, "song_transpose", "-5");
    mono_set_param(source, "song_enabled", "1");
    mono_set_param(source, "step_page", "3");
    mono_set_param(source, "transport", "1");

    char state[16384], recalled[16384];
    int state_len = mono_get_param(source, "state", state, sizeof(state));
    assert(state_len > 0 && state_len < (int)sizeof(state));
    assert(strstr(state, "\"v\":10"));
    assert(strstr(state, "\"data\":\"G"));

    mono_set_param(restored, "transport", "1");
    mono_set_param(restored, "state", state);
    assert(get_int(restored, "transport") == 0); /* recall never auto-starts */
    assert(get_int(restored, "track") == 4);
    assert(get_int(restored, "page") == 6);
    assert(get_int(restored, "step_page") == 3);
    assert(get_int(restored, "pattern_start") == 12);
    assert(get_int(restored, "pattern_len") == 20);
    assert(get_int(restored, "play_order") == 2);
    assert(get_int(restored, "master") == 137);
    assert(get_int(restored, "machine") == 5);
    assert(get_int(restored, "p8") == 111);
    assert(get_int(restored, "alt1") == 87);
    assert(get_int(restored, "alt8") == 29);
    assert(get_int(restored, "step_micro") == -13);
    assert(get_int(restored, "step_tie") == 1);
    assert(get_int(restored, "step_accent") == 76);
    assert(get_int(restored, "arp_enabled") == 1);
    assert(get_int(restored, "arp_mode") == 5);
    assert(get_int(restored, "route_mode") == 4);
    assert(get_int(restored, "track_fx_type") == 4);
    assert(get_int(restored, "morph_valid") == 3);
    assert(get_int(restored, "song_enabled") == 1);
    assert(get_int(restored, "song_length") == 2);
    assert(get_int(restored, "song_edit_row") == 1);
    assert(get_int(restored, "song_start") == 24);
    assert(get_int(restored, "song_repeats") == 3);
    char steps[128];
    get_string(restored, "steps", steps, sizeof(steps));
    assert(steps[strlen(steps) - 1] == '2');

    mono_set_param(restored, "track", "0");
    mono_set_param(restored, "page", "0");
    mono_set_param(restored, "step_page", "0");
    assert(get_int(restored, "machine") == 2);
    assert(get_int(restored, "syn1") == 17);
    get_string(restored, "steps", steps, sizeof(steps));
    assert(!strncmp(steps, "2,", 2));

    /* Restore the saved UI selection before comparing canonical snapshots. */
    mono_set_param(restored, "track", "4");
    mono_set_param(restored, "page", "6");
    mono_set_param(restored, "step_page", "3");
    int recalled_len = mono_get_param(restored, "state", recalled, sizeof(recalled));
    assert(recalled_len == state_len);
    assert(!strcmp(recalled, state));

    mono_set_param(restored, "state", "{\"v\":2,\"data\":\"T0BAD\"}");
    assert(get_int(restored, "machine") == 5); /* malformed snapshots are ignored */
    mono_destroy(source);
    mono_destroy(restored);
}

static void test_v3_lfo_destinations_migrate(void) {
    mono_t *source = mono_create(&host, 1);
    mono_t *restored = mono_create(&host, 1);
    assert(source && restored);
    uint8_t params[64] = {0};
    char key[16];
    for (int page = 0; page < MONO_PAGES; ++page) {
        snprintf(key, sizeof(key), "%d", page);
        mono_set_param(source, "page", key);
        for (int i = 0; i < 8; ++i) {
            snprintf(key, sizeof(key), "p%d", i + 1);
            params[page * 8 + i] = (uint8_t)get_int(source, key);
        }
    }
    mono_set_param(source, "page", "0");
    for (int i = 0; i < 8; ++i) {
        snprintf(key, sizeof(key), "syn%d", i + 9);
        params[56 + i] = (uint8_t)get_int(source, key);
    }
    params[32] = 0x10; /* legacy Pitch */
    params[40] = 0x20; /* legacy Filter Base */
    params[48] = 0x60; /* legacy Delay */
    char state[4096];
    int used = snprintf(state, sizeof(state), "{\"v\":3,\"data\":\"T000");
    for (int i = 0; i < 64; ++i)
        used += snprintf(state + used, sizeof(state) - (size_t)used, "%02X", params[i]);
    snprintf(state + used, sizeof(state) - (size_t)used, "\"}");

    mono_set_param(restored, "state", state);
    assert(get_int(restored, "lfo1_1") == 1);
    assert(get_int(restored, "lfo2_1") == 18);
    assert(get_int(restored, "lfo3_1") == 30);
    mono_destroy(source);
    mono_destroy(restored);
}

static void test_v8_pulse_panel_and_locks_migrate(void) {
    mono_t *source = mono_create(&host, 1);
    mono_t *restored = mono_create(&host, 1);
    assert(source && restored);
    mono_set_param(source, "machine", "1");
    mono_set_param(source, "set_step", "0:60:110:100:15");
    mono_set_param(source, "lock", "0:0:2:88");  /* old UNIX: discarded */
    mono_set_param(source, "lock", "0:0:3:31");  /* old PW */
    mono_set_param(source, "lock", "0:0:4:41");  /* old PWAD */
    mono_set_param(source, "lock", "0:0:5:127"); /* old PWRS */
    mono_set_param(source, "lock", "0:0:6:77");  /* old unused slot */

    char state[8192];
    assert(mono_get_param(source, "state", state, sizeof(state)) > 0);
    char *version = strstr(state, "\"v\":10");
    char *data = strstr(state, "\"data\":\"");
    assert(version && data);
    version[4] = '8';
    memmove(version + 5, version + 6, strlen(version + 6) + 1);
    data = strstr(state, "\"data\":\"");
    assert(data);
    data += strlen("\"data\":\"");
    assert(data[0] == 'G');
    memmove(data, data + 133, strlen(data + 133) + 1); /* strip v10 song record */
    assert(!strncmp(data, "T001", 4));
    memmove(data + 18, data + 82, strlen(data + 82) + 1); /* strip arp/route */
    const char legacy_panel[] = "0A0B63212C7F4D42";
    memcpy(data + 18, legacy_panel, sizeof(legacy_panel) - 1);
    char *pulse_memory = strstr(data, "M001");
    assert(pulse_memory);
    memcpy(pulse_memory + 4, legacy_panel, sizeof(legacy_panel) - 1);
    char *step_record = strstr(data, "S0");
    assert(step_record);
    memmove(step_record + 13, step_record + 14,
            strlen(step_record + 14) + 1); /* strip v10 extended-step flags */

    mono_set_param(restored, "state", state);
    assert(get_int(restored, "machine") == MONO_SWAVE_PULSE);
    assert(get_int(restored, "syn1") == 10);
    assert(get_int(restored, "syn2") == 11);
    assert(get_int(restored, "syn3") == 32);  /* new SUB1 default */
    assert(get_int(restored, "syn4") == 0);   /* new SUB2 default */
    assert(get_int(restored, "syn5") == 33);  /* old PW */
    assert(get_int(restored, "syn6") == 44);  /* old PWAD */
    assert(get_int(restored, "syn7") == 127); /* old PWRS */
    assert(get_int(restored, "syn8") == 66);

    mono_set_param(restored, "machine", "0");
    mono_set_param(restored, "machine", "1");
    assert(get_int(restored, "syn5") == 33);  /* machine memory migrated too */
    mono_set_param(restored, "transport", "1");
    assert(mono_debug_effective_param(restored, 0, 2) == 32);
    assert(mono_debug_effective_param(restored, 0, 3) == 0);
    assert(mono_debug_effective_param(restored, 0, 4) == 31);
    assert(mono_debug_effective_param(restored, 0, 5) == 41);
    assert(mono_debug_effective_param(restored, 0, 6) == 127);
    mono_destroy(source);
    mono_destroy(restored);
}

static void test_v2_presets_receive_machine_shift_defaults(void) {
    mono_t *source = mono_create(&host, 1);
    mono_t *restored = mono_create(&host, 1);
    assert(source && restored);
    char state[4096];
    int used = snprintf(state, sizeof(state),
                        "{\"v\":2,\"track\":0,\"page\":0,\"step_page\":0,"
                        "\"pattern_len\":16,\"master\":70,\"data\":\"T000");
    for (int page = 0; page < MONO_PAGES; ++page) {
        char value[16];
        snprintf(value, sizeof(value), "%d", page);
        mono_set_param(source, "page", value);
        for (int param = 1; param <= 8; ++param) {
            char key[16];
            snprintf(key, sizeof(key), "p%d", param);
            used += snprintf(state + used, sizeof(state) - (size_t)used,
                             "%02X", get_int(source, key));
        }
    }
    used += snprintf(state + used, sizeof(state) - (size_t)used, "\"}");
    assert(used > 0 && used < (int)sizeof(state));

    mono_set_param(restored, "machine", "5");
    mono_set_param(restored, "alt1", "99");
    mono_set_param(restored, "page", "1");
    mono_set_param(restored, "alt1", "3");
    mono_set_param(restored, "page", "0");
    mono_set_param(restored, "state", state);
    assert(get_int(restored, "machine") == 0);
    assert(get_int(restored, "alt1") == 0);
    assert(get_int(restored, "alt2") == 21);
    assert(get_int(restored, "alt3") == 64);
    mono_set_param(restored, "page", "1");
    assert(get_int(restored, "alt1") == 64);
    assert(get_int(restored, "alt4") == 127);
    mono_destroy(source);
    mono_destroy(restored);
}

static void test_machine_memory_and_track_mute_solo(void) {
    mono_t *m = mono_create(&host, 2);
    mono_t *restored = mono_create(&host, 2);
    assert(m && restored);

    mono_set_param(m, "syn1", "11");
    mono_set_param(m, "alt1", "22");
    mono_set_param(m, "machine", "1");
    mono_set_param(m, "syn1", "33");
    mono_set_param(m, "alt1", "44");
    mono_set_param(m, "machine", "0");
    assert(get_int(m, "syn1") == 11);
    assert(get_int(m, "alt1") == 22);
    mono_set_param(m, "machine", "1");
    assert(get_int(m, "syn1") == 33);
    assert(get_int(m, "alt1") == 44);

    mono_set_param(m, "track_mute_toggle", "0");
    mono_set_param(m, "track", "1");
    mono_set_param(m, "track_mute", "1");
    mono_set_param(m, "note_on", "48:110");
    mono_set_param(m, "track", "0");
    mono_set_param(m, "note_on", "60:110");
    assert(render_energy(m, 4) == 0);
    mono_set_param(m, "track_solo_toggle", "1");
    assert(render_energy(m, 4) > 1000); /* solo overrides the mute mix */

    char state[16384];
    assert(mono_get_param(m, "state", state, sizeof(state)) > 0);
    mono_set_param(restored, "state", state);
    assert(get_int(restored, "machine") == 1);
    assert(get_int(restored, "syn1") == 33);
    assert(get_int(restored, "alt1") == 44);
    assert(get_int(restored, "track_mute") == 1);
    mono_set_param(restored, "machine", "0");
    assert(get_int(restored, "syn1") == 11);
    assert(get_int(restored, "alt1") == 22);
    mono_set_param(restored, "track", "1");
    assert(get_int(restored, "track_mute") == 1);
    assert(get_int(restored, "track_solo") == 1);

    mono_destroy(m);
    mono_destroy(restored);
}

static void test_dense_pattern_fits_host_state_limit(void) {
    mono_t *m = mono_create(&host, 6);
    assert(m);
    for (int tr = 0; tr < 6; ++tr) {
        char value[64];
        snprintf(value, sizeof(value), "%d", tr);
        mono_set_param(m, "track", value);
        for (int step = 0; step < MONO_STEPS; ++step) {
            snprintf(value, sizeof(value), "%d:%d:100:100:15", step,
                     36 + (step % 36));
            mono_set_param(m, "set_step", value);
        }
    }
    char state[16384];
    int state_len = mono_get_param(m, "state", state, sizeof(state));
    assert(state_len > 0 && state_len < (int)sizeof(state));
    mono_destroy(m);
}

static void test_maximum_lock_state_fits_overtake_channel(void) {
    mono_t *m = mono_create(&host, 6);
    assert(m);
    char value[64];
    for (int tr = 0; tr < 6; ++tr) {
        snprintf(value, sizeof(value), "%d", tr);
        mono_set_param(m, "track", value);
        for (int step = 0; step < MONO_STEPS; ++step) {
            snprintf(value, sizeof(value), "%d:%d:100:100:15", step,
                     36 + (step % 36));
            mono_set_param(m, "set_step", value);
            for (int pid = 0; pid < MONO_PARAMS; ++pid) {
                snprintf(value, sizeof(value), "%d:%d:%d:%d", tr, step, pid,
                         (tr * 17 + step * 3 + pid) & 127);
                mono_set_param(m, "lock", value);
            }
        }
    }
    char *state = malloc(65536);
    assert(state);
    int state_len = mono_get_param(m, "state", state, 65536);
    assert(state_len > 0 && state_len < 65536);
    mono_t *restored = mono_create(&host, 6);
    assert(restored);
    mono_set_param(restored, "state", state);
    mono_set_param(restored, "track", "5");
    assert(get_int(restored, "alt8") == get_int(m, "alt8"));
    mono_destroy(restored);
    free(state);
    mono_destroy(m);
}

static void test_arp_latch_modes_and_offsets(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "arp_enabled", "1");
    mono_set_param(m, "arp_latch", "1");
    mono_set_param(m, "arp_rate", "7");
    mono_set_param(m, "arp_octaves", "2");
    mono_set_param(m, "arp_gate", "110");
    mono_set_param(m, "arp_length", "2");
    mono_set_param(m, "arp_offset", "0:0");
    mono_set_param(m, "arp_offset", "1:7");
    mono_note_on(m, 0, 48, 90);
    mono_note_on(m, 0, 52, 110);
    assert(render_energy(m, 8) > 1000);
    int first = debug_note(m);
    mono_note_off(m, 0, 48);
    mono_note_off(m, 0, 52);
    assert(render_energy(m, 32) > 1000); /* latched chord keeps clocking */
    mono_note_on(m, 0, 60, 100);         /* a new physical chord replaces it */
    assert(render_energy(m, 16) > 1000);
    assert(debug_note(m) >= 60 && debug_note(m) != first);
    mono_set_param(m, "arp_clear", "1");
    assert(debug_note(m) == -1);
    assert(get_int(m, "arp_octaves") == 2);
    char offsets[128];
    get_string(m, "arp_offsets", offsets, sizeof(offsets));
    assert(!strncmp(offsets, "0,7,", 4));
    mono_destroy(m);
}

static void test_patch_library_randomization_and_morph(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    assert(get_int(m, "patch_count") >= 12);
    char names[512];
    get_string(m, "patch_names", names, sizeof(names));
    assert(strstr(names, "Chrome Bass") && strstr(names, "Soft Operator"));
    mono_set_param(m, "patch_load", "0");
    int a = get_int(m, "syn1");
    mono_set_param(m, "morph_capture_a", "1");
    mono_set_param(m, "syn1", "20");
    mono_set_param(m, "morph_capture_b", "1");
    mono_set_param(m, "morph", "64");
    int halfway = get_int(m, "syn1");
    assert(get_int(m, "morph_valid") == 3);
    assert(halfway > 20 && halfway < a);
    mono_set_param(m, "patch_randomize", "1");
    assert(get_int(m, "morph_valid") == 0);
    mono_destroy(m);
}

static void test_user_wave_upload_and_persistence(void) {
    const char *path = "/tmp/mono-user-wave-test.bin";
    remove(path);
    mono_t *m = mono_create_with_storage(&host, 1, path);
    assert(m);
    mono_set_param(m, "wave_begin", "0");
    for (int chunk = 0; chunk < 8; ++chunk) {
        char payload[256];
        int used = snprintf(payload, sizeof(payload), "0:%d:", chunk * 64);
        for (int i = 0; i < 64; ++i) {
            int sample = chunk * 64 + i;
            int encoded = 2048 + ((sample * 8) % 2048) - 1024;
            used += snprintf(payload + used, sizeof(payload) - (size_t)used,
                             "%03X", encoded);
        }
        mono_set_param(m, "wave_chunk", payload);
    }
    mono_set_param(m, "wave_commit", "0");
    assert(mono_debug_user_wave_mask(m) == 1);
    mono_destroy(m);
    m = mono_create_with_storage(&host, 1, path);
    assert(m && mono_debug_user_wave_mask(m) == 1);
    mono_set_param(m, "machine", "4");
    mono_set_param(m, "syn1", "100"); /* selects one of the user slots */
    mono_note_on(m, 0, 48, 110);
    assert(render_energy(m, 8) > 1000);
    mono_set_param(m, "wave_clear", "0");
    assert(mono_debug_user_wave_mask(m) == 0);
    mono_destroy(m);
    remove(path);
}

static void test_neighbor_routing_and_track_fx(void) {
    mono_t *plain = mono_create(&host, 2);
    mono_t *routed = mono_create(&host, 2);
    assert(plain && routed);
    mono_note_on(plain, 0, 48, 110);
    mono_note_on(plain, 1, 55, 110);
    mono_note_on(routed, 0, 48, 110);
    mono_note_on(routed, 1, 55, 110);
    mono_set_param(routed, "track", "1");
    mono_set_param(routed, "route_mode", "3");
    mono_set_param(routed, "route_amount", "127");
    mono_set_param(routed, "track_fx_type", "6");
    mono_set_param(routed, "track_fx_amount", "96");
    mono_set_param(routed, "track_fx_tone", "20");
    mono_set_param(routed, "track_fx_mix", "127");
    assert(render_hash_long(plain) != render_hash_long(routed));
    mono_destroy(plain);
    mono_destroy(routed);
}

static void test_microtiming_accent_ties_and_song_mode(void) {
    mono_t *late = mono_create(&host, 1);
    assert(late);
    mono_set_param(late, "set_step", "0:48:70:100:15");
    mono_set_param(late, "edit_step", "0");
    mono_set_param(late, "step_micro", "23");
    mono_set_param(late, "step_accent", "127");
    mono_set_param(late, "transport", "1");
    assert(render_energy(late, 8) == 0);
    assert(render_energy(late, 24) > 1000);
    mono_destroy(late);

    mono_t *song = mono_create(&host, 1);
    assert(song);
    mono_set_param(song, "set_step", "0:48:90:100:15");
    mono_set_param(song, "edit_step", "0");
    mono_set_param(song, "step_tie", "1");
    mono_set_param(song, "song_edit_row", "0");
    mono_set_param(song, "song_start", "0");
    mono_set_param(song, "song_row_length", "1");
    mono_set_param(song, "song_repeats", "2");
    mono_set_param(song, "song_transpose", "12");
    mono_set_param(song, "song_enabled", "1");
    mono_set_param(song, "transport", "1");
    assert(debug_note(song) == 60);
    assert(get_int(song, "step_tie") == 1);
    assert(get_int(song, "song_repeats") == 2);
    mono_destroy(song);
}

static void test_calibration_generators_and_metrics(void) {
    mono_t *m = mono_create(&host, 1);
    assert(m);
    mono_set_param(m, "calibration_level", "64");
    mono_set_param(m, "calibration_mode", "1");
    assert(render_energy(m, 4) > 1000);
    char metrics[128];
    get_string(m, "calibration_metrics", metrics, sizeof(metrics));
    assert(strchr(metrics, ':'));
    mono_set_param(m, "calibration_reset", "1");
    get_string(m, "calibration_metrics", metrics, sizeof(metrics));
    assert(!strncmp(metrics, "0:0:0:0:0", 9));
    mono_set_param(m, "calibration_mode", "0");
    mono_destroy(m);
}

int main(void) {
    test_all_machines_sound_distinct();
    test_superwave_saw_rejects_folded_harmonics();
    test_superwave_pulse_primary_controls_are_wired();
    test_ensemble_wave_and_chorus_controls_are_wired();
    test_sid_and_digipro_previous_track_sources();
    test_sid_and_digipro_restart_their_sweeps();
    test_fm_volume_envelope_and_tone_shape_spectrum();
    test_filter_uses_octave_steps_and_key_tracking();
    test_note_release();
    test_held_note_priority_and_block_tempo_lookup();
    test_filters_remain_finite_and_retrigger();
    test_parameter_aliases();
    test_all_lfo_destinations_round_trip();
    test_lfo_can_modulate_lfo_settings();
    test_shift_layer_controls_are_audible();
    test_every_secondary_page_is_audible();
    test_sequencer_and_lock();
    test_pattern_window_and_play_orders();
    test_per_track_windows_rotation_division_and_swing();
    test_step_track_copy_paste_and_undo();
    test_step_probability_condition_retrig_and_slide_state();
    test_live_parameter_recording_and_smoothing();
    test_internal_clock();
    test_clock_loss_falls_back();
    test_six_tracks_render_together();
    test_production_event_paths();
    test_remote_state_contract();
    test_full_state_round_trip();
    test_v3_lfo_destinations_migrate();
    test_v8_pulse_panel_and_locks_migrate();
    test_v2_presets_receive_machine_shift_defaults();
    test_machine_memory_and_track_mute_solo();
    test_dense_pattern_fits_host_state_limit();
    test_maximum_lock_state_fits_overtake_channel();
    test_arp_latch_modes_and_offsets();
    test_patch_library_randomization_and_morph();
    test_user_wave_upload_and_persistence();
    test_neighbor_routing_and_track_fx();
    test_microtiming_accent_ties_and_song_mode();
    test_calibration_generators_and_metrics();
    puts("mono host simulator: all tests passed");
    return 0;
}
