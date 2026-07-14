#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_api_v1.h"

#define BLOCK 128

static float probe_bpm(void) { return 120.0f; }

static void probe_log(const char *message) {
    fprintf(stderr, "plugin: %s\n", message ? message : "(null)");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/dsp.so\n", argv[0]);
        return 2;
    }

    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 3;
    }

    void *symbol = dlsym(handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    move_plugin_init_v2_fn init = NULL;
    if (symbol) memcpy(&init, &symbol, sizeof(init));
    if (!symbol || !init) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        dlclose(handle);
        return 4;
    }

    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = BLOCK;
    host.log = probe_log;
    host.get_bpm = probe_bpm;

    plugin_api_v2_t *api = init(&host);
    if (!api || api->api_version != MOVE_PLUGIN_API_VERSION_2) {
        fprintf(stderr, "bad API\n");
        dlclose(handle);
        return 5;
    }

    void *instance = api->create_instance("/tmp", NULL);
    if (!instance) {
        fprintf(stderr, "create_instance returned NULL\n");
        dlclose(handle);
        return 6;
    }

    char module_id[64] = {0};
    if (api->get_param)
        api->get_param(instance, "module_id", module_id, sizeof(module_id));

    /* Exercise both supported trigger paths: MIDI for Mono Voice and the
     * parameter bridge used by Mono's overtake JavaScript. */
    const uint8_t note_on[3] = {0x90, 60, 112};
    if (api->on_midi)
        api->on_midi(instance, note_on, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    if (api->set_param)
        api->set_param(instance, "note_on", "60:112");

    int16_t out[BLOCK * 2];
    int peak = 0;
    int64_t energy = 0;
    for (int block = 0; block < 64; ++block) {
        memset(out, 0, sizeof(out));
        api->render_block(instance, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) {
            int sample = out[i] < 0 ? -out[i] : out[i];
            if (sample > peak) peak = sample;
            energy += sample;
        }
    }

    printf("module=%s peak=%d energy=%lld\n",
           module_id[0] ? module_id : "?", peak, (long long)energy);

    api->destroy_instance(instance);
    dlclose(handle);
    return peak > 32 && energy > 10000 ? 0 : 7;
}
