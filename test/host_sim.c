#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mono_core.h"

#define BLOCK 128

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

static int get_int(mono_t *m, const char *key) {
    char buf[256];
    assert(mono_get_param(m, key, buf, sizeof(buf)) >= 0);
    return atoi(buf);
}

static void get_string(mono_t *m, const char *key, char *buf, size_t size) {
    assert(mono_get_param(m, key, buf, (int)size) >= 0);
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
    mono_set_param(source, "step_page", "3");
    mono_set_param(source, "transport", "1");

    char state[16384], recalled[16384];
    int state_len = mono_get_param(source, "state", state, sizeof(state));
    assert(state_len > 0 && state_len < (int)sizeof(state));
    assert(strstr(state, "\"v\":5"));
    assert(strstr(state, "\"data\":\"T0"));

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
    char state[4096];
    assert(mono_get_param(source, "state", state, sizeof(state)) > 0);
    char *version = strstr(state, "\"v\":5");
    char *data = strstr(state, "\"data\":\"");
    assert(version && data);
    version[4] = '3';
    data += strlen("\"data\":\"");
    assert(!strncmp(data, "T000", 4));
    memcpy(data + 4 + 32 * 2, "10", 2); /* legacy Pitch */
    memcpy(data + 4 + 40 * 2, "20", 2); /* legacy Filter Base */
    memcpy(data + 4 + 48 * 2, "60", 2); /* legacy Delay */
    strcpy(data + 4 + 64 * 2, "\"}");

    mono_set_param(restored, "state", state);
    assert(get_int(restored, "lfo1_1") == 1);
    assert(get_int(restored, "lfo2_1") == 18);
    assert(get_int(restored, "lfo3_1") == 30);
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

int main(void) {
    test_all_machines_sound_distinct();
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
    test_live_parameter_recording_and_smoothing();
    test_internal_clock();
    test_clock_loss_falls_back();
    test_six_tracks_render_together();
    test_production_event_paths();
    test_remote_state_contract();
    test_full_state_round_trip();
    test_v3_lfo_destinations_migrate();
    test_v2_presets_receive_machine_shift_defaults();
    test_dense_pattern_fits_host_state_limit();
    test_maximum_lock_state_fits_overtake_channel();
    puts("mono host simulator: all tests passed");
    return 0;
}
