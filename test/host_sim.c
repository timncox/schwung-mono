#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mono_core.h"

#define BLOCK 128

static float sim_bpm(void) { return 120.0f; }

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
    mono_set_param(m, "lfo1_1", "3");
    assert(get_int(m, "lfo1_1") == 3);
    mono_set_param(m, "page", "4");
    assert(get_int(m, "p1") == 48);
    mono_set_param(m, "p1", "96");
    assert(get_int(m, "lfo1_1") == 6);
    mono_destroy(m);
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
    mono_set_param(m, "track", "2");
    mono_set_param(m, "page", "3");
    mono_set_param(m, "p4", "99");
    mono_set_param(m, "toggle_step", "0");

    char state[16384], poll[128];
    get_string(m, "state", state, sizeof(state));
    get_string(m, "rui_poll", poll, sizeof(poll));
    assert(strstr(state, "\"track\":2"));
    assert(strstr(state, "\"page\":3"));
    assert(strstr(state, "\"p4\":99"));
    assert(strstr(state, "\"steps\":\"1,"));
    assert(strchr(poll, ':'));
    mono_destroy(m);
}

static void test_full_state_round_trip(void) {
    mono_t *source = mono_create(&host, 6);
    mono_t *restored = mono_create(&host, 6);
    assert(source && restored);

    mono_set_param(source, "pattern_len", "64");
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
    mono_set_param(source, "set_step", "63:72:127:64:9");
    mono_set_param(source, "lock", "4:63:55:33");
    mono_set_param(source, "step_page", "3");
    mono_set_param(source, "transport", "1");

    char state[16384], recalled[16384];
    int state_len = mono_get_param(source, "state", state, sizeof(state));
    assert(state_len > 0 && state_len < (int)sizeof(state));
    assert(strstr(state, "\"v\":2"));
    assert(strstr(state, "\"data\":\"T0"));

    mono_set_param(restored, "transport", "1");
    mono_set_param(restored, "state", state);
    assert(get_int(restored, "transport") == 0); /* recall never auto-starts */
    assert(get_int(restored, "track") == 4);
    assert(get_int(restored, "page") == 6);
    assert(get_int(restored, "step_page") == 3);
    assert(get_int(restored, "pattern_len") == 64);
    assert(get_int(restored, "master") == 137);
    assert(get_int(restored, "machine") == 5);
    assert(get_int(restored, "p8") == 111);
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
    free(state);
    mono_destroy(m);
}

int main(void) {
    test_all_machines_sound_distinct();
    test_note_release();
    test_filters_remain_finite_and_retrigger();
    test_parameter_aliases();
    test_sequencer_and_lock();
    test_internal_clock();
    test_clock_loss_falls_back();
    test_six_tracks_render_together();
    test_production_event_paths();
    test_remote_state_contract();
    test_full_state_round_trip();
    test_dense_pattern_fits_host_state_limit();
    test_maximum_lock_state_fits_overtake_channel();
    puts("mono host simulator: all tests passed");
    return 0;
}
