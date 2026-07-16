#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mono_core.h"

#define BLOCK 128
#define SECONDS 20

static float benchmark_bpm(void) { return 124.0f; }

static host_api_v1_t host = {
    .api_version = MOVE_PLUGIN_API_VERSION,
    .sample_rate = MOVE_SAMPLE_RATE,
    .frames_per_block = BLOCK,
    .get_bpm = benchmark_bpm
};

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

int main(void) {
    mono_t *m = mono_create(&host, MONO_MAX_TRACKS);
    if (!m) return 1;
    for (int track = 0; track < MONO_MAX_TRACKS; ++track) {
        char value[32];
        snprintf(value, sizeof(value), "%d", track);
        mono_set_param(m, "track", value);
        snprintf(value, sizeof(value), "%d", track % MONO_MACHINE_COUNT);
        mono_set_param(m, "machine", value);
        mono_set_param(m, "amp2", "127");
        mono_set_param(m, "amp3", "127");
        mono_set_param(m, "flt3", "96");
        mono_set_param(m, "flt4", "104");
        mono_set_param(m, "flt13", "127");
        mono_set_param(m, "flt14", "127");
        mono_set_param(m, "fx4", "96");
        mono_set_param(m, "fx6", "88");
        mono_set_param(m, "fx12", "96");
        mono_set_param(m, "lfo1_1", "18");
        mono_set_param(m, "lfo1_7", "72");
        mono_set_param(m, "lfo2_1", "19");
        mono_set_param(m, "lfo2_7", "64");
        mono_set_param(m, "lfo3_1", "30");
        mono_set_param(m, "lfo3_7", "48");
        mono_note_on(m, track, 42 + track * 4, 112);
    }

    int16_t out[BLOCK * 2];
    int blocks = SECONDS * MOVE_SAMPLE_RATE / BLOCK;
    double start = now_seconds();
    for (int block = 0; block < blocks; ++block) {
        memset(out, 0, sizeof(out));
        mono_render(m, out, BLOCK);
    }
    double elapsed = now_seconds() - start;
    double rendered = blocks * BLOCK / (double)MOVE_SAMPLE_RATE;
    printf("mono benchmark: %.2fs audio in %.3fs (%.1fx realtime)\n",
           rendered, elapsed, rendered / elapsed);
    mono_destroy(m);
    return elapsed > 0.0 ? 0 : 1;
}
