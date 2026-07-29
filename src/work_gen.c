/*
 * Work In — the sound_generator build.
 *
 * Where the audio_fx build processes whatever is upstream of it in a Signal
 * Chain, this one IS the source: it occupies a slot and reads the hardware
 * input directly from the host mailbox, like the in-tree linein module and
 * smack-in. Input routing follows whatever was last selected in stock Move.
 *
 * Three reasons this build exists rather than being redundant with work.so:
 *
 *   1. It can sit at the head of a chain, so Move's own effects can come
 *      AFTER the FX machines rather than only before them.
 *   2. Declaring capabilities.audio_in gets schwung's boot feedback
 *      protection for line-input-consuming sound generators automatically.
 *   3. schwung-manager wires custom web UIs only for slot synths and overtake
 *      tools — a chain audio_fx can never have one. This build is the route
 *      to a browser editor for the non-overtake case.
 *
 * Sound generators loaded by the chain are hardcoded to dsp.so, so unlike the
 * audio_fx build this file's shared library is NOT named after the module id.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "work_core.h"

static const host_api_v1_t *g_host;

static void *gen_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    work_t *w = work_create(g_host);
    if (w) work_set_param(w, "hw_input", "1");
    return w;
}

static void gen_destroy(void *inst) { work_destroy((work_t *)inst); }

static void gen_render(void *inst, int16_t *out_lr, int frames) {
    const int16_t *in = NULL;
    if (g_host && g_host->mapped_memory)
        in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    if (!in) {
        memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    work_process((work_t *)inst, in, out_lr, frames);
}

static void gen_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    work_on_midi((work_t *)inst, msg, len, source);
}

static void gen_set_param(void *inst, const char *key, const char *val) {
    work_set_param((work_t *)inst, key, val);
}

static int gen_get_param(void *inst, const char *key, char *buf, int buf_len) {
    /* Answered for the manager's remote-UI probe. Nothing reaches this key on
     * the slot-synth load path, so replying is harmless there and correct if
     * the same .so is ever loaded another way. */
    if (!strcmp(key, "module_id"))
        return snprintf(buf, (size_t)buf_len, "work-in");
    return work_get_param((work_t *)inst, key, buf, buf_len);
}

static int gen_get_error(void *inst, char *buf, int buf_len) {
    (void)inst; (void)buf; (void)buf_len;
    return 0;
}

static plugin_api_v2_t api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = gen_create,
    .destroy_instance = gen_destroy,
    .on_midi          = gen_on_midi,
    .set_param        = gen_set_param,
    .get_param        = gen_get_param,
    .get_error        = gen_get_error,
    .render_block     = gen_render,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
