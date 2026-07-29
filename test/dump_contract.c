/*
 * Emits the REAL get_param/set_param contract as JSON, for the UI harness to
 * mock against.
 *
 * The point: a UI test whose mock was written from the UI's own assumptions
 * proves nothing — it encodes the same misunderstanding twice and goes green
 * while the feature is broken on every device. That is exactly how Mono
 * shipped a preset browser that never listed a file (os.readdir returns
 * [names, errno], the mock returned a flat array). So the fixture is generated
 * from the engine itself, and the harness fails if the UI reads a key the
 * engine does not serve or writes one it does not accept.
 */
#include <stdio.h>
#include <string.h>

#include "work_core.h"

static float sim_bpm(void) { return 120.0f; }
static host_api_v1_t host = {
    .api_version = MOVE_PLUGIN_API_VERSION,
    .sample_rate = MOVE_SAMPLE_RATE,
    .frames_per_block = 128,
    .get_bpm = sim_bpm
};

static int first = 1;

static void emit(work_t *w, const char *key) {
    char buf[8192];
    int n = work_get_param(w, key, buf, sizeof(buf));
    if (n < 0) return;                       /* key not served — omit it */
    printf("%s\n    \"%s\": \"", first ? "" : ",", key);
    for (const char *c = buf; *c; ++c) {
        if (*c == '"' || *c == '\\') putchar('\\');
        putchar(*c);
    }
    printf("\"");
    first = 0;
}

/* A key counts as settable if writing a value changes what reading returns. */
static int settable(work_t *w, const char *key, const char *probe) {
    char before[8192], after[8192];
    int b = work_get_param(w, key, before, sizeof(before));
    work_set_param(w, key, probe);
    int a = work_get_param(w, key, after, sizeof(after));
    if (b < 0 || a < 0) return 0;
    return strcmp(before, after) != 0;
}

int main(void) {
    work_t *w = work_create(&host);
    if (!w) return 1;

    /* A representative engine state: two machines loaded, a pattern with
     * trigs, locks, a condition and micro timing. */
    work_set_param(w, "fx1", "1");            /* Chrono Pitch */
    work_set_param(w, "fx2", "18");           /* Supervoid Reverb */
    work_set_param(w, "seq_on", "1");
    work_set_param(w, "seq_len", "32");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "step3", "1:9:-5:2");
    work_set_param(w, "locks3", "0=100,4=20,18=64");
    work_set_param(w, "step7", "1:1:0:0");

    /* Run a block so the effective-parameter view is populated: eff1/eff2 are
     * computed during processing, and a fixture captured before any audio ran
     * would record all zeros and teach the harness the wrong shape. */
    {
        int16_t buf[128 * 2];
        memset(buf, 0, sizeof(buf));
        work_process(w, buf, buf, 128);
    }

    printf("{\n  \"get\": {");
    const char *simple[] = {
        "machines", "conds", "fx1", "fx2", "labels1", "labels2",
        "eff1", "eff2", "effm", "mix", "seq_on", "seq_len", "fill",
        "seq_pos", "meter", "state", "live_rec",
        "menv_dest", "menv_atk", "menv_hold", "menv_dec", "menv_depth",
        "pattern", "page_mask", "song_on", "song_len", "song_pos", "undo_state",
        "monitor", "hw_input"
    };
    for (size_t i = 0; i < sizeof(simple) / sizeof(simple[0]); ++i) emit(w, simple[i]);

    char key[32];
    for (int s = 1; s <= 2; ++s)
        for (int p = 1; p <= 8; ++p) {
            snprintf(key, sizeof(key), "fx%d_p%d", s, p);
            emit(w, key);
        }
    for (int i = 0; i < WORK_STEPS; ++i) {
        snprintf(key, sizeof(key), "step%d", i);   emit(w, key);
        snprintf(key, sizeof(key), "locks%d", i);  emit(w, key);
    }
    for (int i = 0; i < WORK_LOCKABLE; ++i) {
        snprintf(key, sizeof(key), "locklabel%d", i); emit(w, key);
    }
    for (int i = 0; i < WORK_STEPS; ++i) {
        snprintf(key, sizeof(key), "prob%d", i);     emit(w, key);
        snprintf(key, sizeof(key), "trigtype%d", i); emit(w, key);
    }
    for (int i = 0; i < WORK_SONG_ROWS; ++i) {
        snprintf(key, sizeof(key), "song_row%d", i); emit(w, key);
    }
    for (int n = 1; n <= WORK_LFOS; ++n) {
        const char *f[] = {"dest", "spd", "mult", "wave", "depth", "phase", "trig"};
        for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); ++i) {
            snprintf(key, sizeof(key), "lfo%d_%s", n, f[i]);
            emit(w, key);
        }
    }
    /* Every individual lock the UI can read: a value when locked, -1 when
     * not. Emitted for a step that has locks and one that has none. */
    for (int s = 0; s < WORK_STEPS; ++s)
        for (int i = 0; i < WORK_LOCKABLE; ++i) {
            snprintf(key, sizeof(key), "lock%d_%d", s, i);
            emit(w, key);
        }
    printf("\n  },\n  \"settable\": [");

    /* Probe the writable keys with values that must change the read-back. */
    struct { const char *key; const char *probe; } probes[] = {
        {"fx1", "5"}, {"fx2", "3"}, {"mix", "77"},
        {"seq_on", "0"}, {"seq_len", "9"}, {"fill", "1"}, {"seq_clear", "1"},
        {"fx1_p1", "42"}, {"fx2_p8", "13"},
        {"lfo1_dest", "4"}, {"lfo1_spd", "99"}, {"lfo1_wave", "3"},
        {"lfo2_depth", "20"}, {"lfo1_trig", "1"}, {"lfo3_dest", "6"}, {"lfo3_spd", "70"},
        {"live_rec", "1"}, {"menv_dest", "3"}, {"menv_atk", "20"}, {"menv_hold", "30"},
        {"menv_dec", "40"}, {"menv_depth", "90"}, {"prob5", "37"}, {"pattern", "3"}, {"page_mask", "7"}, {"song_on", "1"},
        {"song_len", "8"}, {"song_row2", "5:3:0"}, {"trigtype4", "1"},
        {"transform", "reverse"}, {"quantize", "127"}, {"monitor", "0"}, {"hw_input", "1"},
        {"step5", "1:2:3:1"}, {"locks5", "1=64"}, {"lock5_2", "31"},
        {"state", "{\"v\":1,\"mix\":64}"}
    };
    int sfirst = 1;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        if (!settable(w, probes[i].key, probes[i].probe)) continue;
        printf("%s\n    \"%s\"", sfirst ? "" : ",", probes[i].key);
        sfirst = 0;
    }
    /* Write-only commands have no readable value, so the round-trip probe
     * cannot see them. They are still legal writes — list them separately
     * rather than letting the harness flag them as unknown keys. */
    printf("\n  ],\n  \"commands\": [\"seq_clear\", \"undo\", \"redo\", "
           "\"memorize\", \"recall\", \"transform\", \"quantize\"]\n}\n");

    work_destroy(w);
    return 0;
}
