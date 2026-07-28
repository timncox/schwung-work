/*
 * Work audio_fx build — loads in Signal Chain slots and Master FX slots
 * (shadow_master_fx_slot_load uses the same audio_fx plugins).
 *
 * NOTE: the chain host loads audio FX as modules/audio_fx/work/work.so
 * without consulting module.json, so this .so MUST be named work.so.
 */
#include <stddef.h>

#include "audio_fx_api_v2.h"
#include "work_core.h"

static const host_api_v1_t *g_host;

static void *fx_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    return work_create(g_host);
}

static void fx_destroy(void *inst) { work_destroy((work_t *)inst); }

static void fx_process(void *inst, int16_t *audio_inout, int frames) {
    work_process((work_t *)inst, audio_inout, audio_inout, frames);
}

static void fx_set_param(void *inst, const char *key, const char *val) {
    work_set_param((work_t *)inst, key, val);
}

static int fx_get_param(void *inst, const char *key, char *buf, int buf_len) {
    return work_get_param((work_t *)inst, key, buf, buf_len);
}

static audio_fx_api_v2_t api = {
    .api_version      = AUDIO_FX_API_VERSION_2,
    .create_instance  = fx_create,
    .destroy_instance = fx_destroy,
    .process_block    = fx_process,
    .set_param        = fx_set_param,
    .get_param        = fx_get_param,
    /* on_midi deliberately unset: the chain host ignores the struct field and
     * discovers MIDI via dlsym("move_audio_fx_on_midi") below. Leaving it NULL
     * avoids ABI trouble with older six-field hosts (the ducker pattern). */
};

/* Without this symbol the FX build never receives MIDI clock, and every
 * tempo-synced machine free-runs at the right tempo but the wrong phase. */
__attribute__((visibility("default")))
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    work_on_midi((work_t *)instance, msg, len, source);
}

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
