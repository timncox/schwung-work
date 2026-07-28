/*
 * Overwork — the overtake build of Work.
 *
 * Takes over Move's whole surface and processes the hardware audio input
 * (mic / line / USB-C, whatever the XMOS is routing) through the same FX
 * engine the audio_fx build uses, with the step sequencer driving parameter
 * locks.
 *
 * Input comes from the host mailbox (host->mapped_memory + audio_in_offset),
 * the same way the in-tree linein module and smack-in read it. The shim
 * restores raw hardware audio_in for overtake DSPs unconditionally, so this
 * works regardless of what the module metadata claims about consuming line
 * input — the metadata flag only feeds schwung's feedback-guard heuristic.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "plugin_api_v1.h"
#include "work_core.h"

static const host_api_v1_t *g_host;

static void *overtake_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    work_t *w = work_create(g_host);
    /* The overtake surface is a sequencer first — start with it running so a
     * freshly placed trig does something without hunting for a switch. */
    if (w) work_set_param(w, "seq_on", "1");
    return w;
}

static void overtake_destroy(void *instance) { work_destroy((work_t *)instance); }

static void overtake_midi(void *instance, const uint8_t *msg, int len, int source) {
    work_on_midi((work_t *)instance, msg, len, source);
}

static void overtake_set(void *instance, const char *key, const char *val) {
    work_set_param((work_t *)instance, key, val);
}

static int overtake_get(void *instance, const char *key, char *buf, int buf_len) {
    /* schwung-manager discovers the active overtake tool by probing
     * overtake_dsp:module_id (remote_ui.go activeOvertakeToolID); the shim
     * forwards it straight to the DSP, so the plugin must answer or the
     * manager decides no tool is loaded. */
    if (!strcmp(key, "module_id"))
        return snprintf(buf, (size_t)buf_len, "overwork");
    return work_get_param((work_t *)instance, key, buf, buf_len);
}

static int overtake_error(void *instance, char *buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

static void overtake_render(void *instance, int16_t *out_lr, int frames) {
    const int16_t *in = NULL;
    if (g_host && g_host->mapped_memory)
        in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    if (!in) {
        memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    work_process((work_t *)instance, in, out_lr, frames);
}

static plugin_api_v2_t api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = overtake_create,
    .destroy_instance = overtake_destroy,
    .on_midi          = overtake_midi,
    .set_param        = overtake_set,
    .get_param        = overtake_get,
    .get_error        = overtake_error,
    .render_block     = overtake_render,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
