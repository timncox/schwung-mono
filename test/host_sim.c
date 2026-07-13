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

int main(void) {
    test_all_machines_sound_distinct();
    test_note_release();
    test_parameter_aliases();
    test_sequencer_and_lock();
    test_internal_clock();
    test_clock_loss_falls_back();
    puts("mono host simulator: all tests passed");
    return 0;
}
