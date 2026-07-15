#ifndef MONO_CORE_H
#define MONO_CORE_H

#include <stdint.h>
#include "plugin_api_v1.h"

#define MONO_MAX_TRACKS 6
#define MONO_STEPS 64
#define MONO_PAGES 7
#define MONO_PAGE_PARAMS 8
#define MONO_PRIMARY_PARAMS (MONO_PAGES * MONO_PAGE_PARAMS)
#define MONO_ALT_PARAMS 8
#define MONO_ALT_BASE MONO_PRIMARY_PARAMS
#define MONO_PARAMS (MONO_PRIMARY_PARAMS + MONO_ALT_PARAMS)
#define MONO_LFO_DESTINATIONS (MONO_PARAMS + 2) /* Off, Pitch, then every param */

typedef enum {
    MONO_SWAVE_SAW = 0,
    MONO_SWAVE_PULSE,
    MONO_SWAVE_ENSEMBLE,
    MONO_SID_6581,
    MONO_DIGIPRO_WAVE,
    MONO_FM_STATIC,
    MONO_MACHINE_COUNT
} mono_machine_t;

typedef struct mono mono_t;

mono_t *mono_create(const host_api_v1_t *host, int track_count);
void mono_destroy(mono_t *m);
void mono_render(mono_t *m, int16_t *out_lr, int frames);
void mono_on_midi(mono_t *m, const uint8_t *msg, int len, int source);
void mono_set_param(mono_t *m, const char *key, const char *val);
int mono_get_param(mono_t *m, const char *key, char *buf, int buf_len);

/* Native simulator hooks; production wrappers use the parameter/MIDI API. */
void mono_note_on(mono_t *m, int track, int note, int velocity);
void mono_note_off(mono_t *m, int track, int note);
void mono_advance_step(mono_t *m);
int mono_debug_effective_param(mono_t *m, int track, int pid);
float mono_debug_smoothed_param(mono_t *m, int track, int pid);

#endif
