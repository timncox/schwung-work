/*
 * Work — the END-OF-CHAIN overtake build ("Overwork Mix").
 *
 * The same engine as Overwork, fed by what the MOVE IS PLAYING instead of the
 * jack. schwung decides an overtake module's role by which entry point its .so
 * exports: this file exports move_audio_fx_init_v2, so the shim loads it as
 * overtake_dsp_fx, and with capabilities.end_of_chain in module.json it runs
 * process_block IN PLACE on the final Move+ME mix, after the ME sum. That is
 * the only way an overtake module hears the Move's own tracks, and it is why
 * this is a second module rather than a setting: the generator role gets the
 * jack, the FX role gets the bus, and one .so cannot be both.
 *
 * Three things distinguish this from work_fx.c, and each is load-bearing:
 *
 *   .on_midi IS SET IN THE STRUCT. The chain host discovers MIDI by
 *   dlsym("move_audio_fx_on_midi") and ignores the field, which is why
 *   work_fx.c leaves it unset. The overtake shim does the opposite: it reads
 *   overtake_dsp_fx->on_midi and never dlsyms it. Copy work_fx.c naively and
 *   the UI draws perfectly with every pad, step and CC dead.
 *
 *   passthru 1 at create. The input is a bus, not a microphone, so neither
 *   reason for zeroing in_gain holds — and the source-machine rule would mute
 *   the Move the moment a sampler was loaded. See work_core.c.
 *
 *   hw_input is NOT set. That flag arms the feedback guard, which is about a
 *   mic feeding a speaker; there is no mic here. work_overtake.c sets it, this
 *   build must not.
 *
 * module_id is answered here, not in the core, exactly as work_overtake.c does:
 * schwung-manager keys the Tool tab's web_ui on overtake_dsp:module_id.
 */
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "audio_fx_api_v2.h"
#include "work_core.h"

#define MODULE_ID "overwork-mix"

static const host_api_v1_t *g_host;

static void *fx_create(const char *module_dir, const char *config_json) {
    (void)module_dir; (void)config_json;
    work_t *w = work_create(g_host);
    if (w) {
        work_set_param(w, "seq_on",   "1");   /* a full-surface build sequences */
        work_set_param(w, "passthru", "1");   /* the input is the Move: never zero it */
    }
    return w;
}
static void fx_destroy(void *inst) { work_destroy((work_t *)inst); }

static void fx_process(void *inst, int16_t *audio_inout, int frames) {
    work_process((work_t *)inst, audio_inout, audio_inout, frames);
}
static void fx_set_param(void *inst, const char *key, const char *val) {
    work_set_param((work_t *)inst, key, val);
}
static int fx_get_param(void *inst, const char *key, char *buf, int buf_len) {
    if (key && !strcmp(key, "module_id")) {
        if (buf_len <= 0) return 0;
        int n = snprintf(buf, (size_t)buf_len, "%s", MODULE_ID);
        return n < buf_len ? n : buf_len - 1;
    }
    return work_get_param((work_t *)inst, key, buf, buf_len);
}
static void fx_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    work_on_midi((work_t *)inst, msg, len, source);
}

static audio_fx_api_v2_t api = {
    .api_version      = AUDIO_FX_API_VERSION_2,
    .create_instance  = fx_create,
    .destroy_instance = fx_destroy,
    .process_block    = fx_process,
    .set_param        = fx_set_param,
    .get_param        = fx_get_param,
    .on_midi          = fx_on_midi,           /* the overtake shim reads THIS */
};

/* Kept as well: harmless for the overtake shim, and it means this .so also
 * behaves in a chain slot should anyone load it there. */
__attribute__((visibility("default")))
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    fx_on_midi(instance, msg, len, source);
}

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
