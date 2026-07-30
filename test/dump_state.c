/*
 * dump_state — emits a REAL state blob from the engine, for the browser
 * editor's harness to render.
 *
 * Same discipline as dump_contract: test/web_ui.mjs must not seed the page
 * from a blob someone wrote by hand, because a hand-written fixture encodes
 * what the page's author ASSUMED the engine emits. That is the mistake this
 * project has paid for repeatedly — a mock that agrees with the code under
 * test goes green while the feature is dead on hardware.
 *
 *     make build/dump_state && ./build/dump_state > build/state.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "work_core.h"

static float dump_bpm(void) { return 120.0f; }

static host_api_v1_t host = {
    .api_version      = MOVE_PLUGIN_API_VERSION,
    .sample_rate      = MOVE_SAMPLE_RATE,
    .frames_per_block = 128,
    .get_bpm          = dump_bpm
};

int main(void) {
    work_t *w = work_create(&host);
    if (!w) return 1;

    /* A patch worth looking at: three real machines on track 1, a couple of
     * other tracks carrying something so the rail is not eight blanks, a
     * modulated parameter so the effective-value readout has work to do, and
     * a pattern with locks. */
    work_set_param(w, "track", "0");
    work_set_param(w, "src",      "22");     /* Polysample                  */
    work_set_param(w, "machine1", "12");     /* Multimode Filter            */
    work_set_param(w, "machine2", "16");     /* Drive Delay                 */
    work_set_param(w, "fx1_p5", "40");
    work_set_param(w, "flfo1_dest",  "4");   /* insert 1 knob E             */
    work_set_param(w, "flfo1_depth", "90");
    work_set_param(w, "flfo1_spd",   "48");
    work_set_param(w, "vlfo1_dest",  "8");   /* the voice filter's base     */
    work_set_param(w, "vlfo1_depth", "70");
    work_set_param(w, "menv_dest",  "12");
    work_set_param(w, "menv_depth", "96");
    work_set_param(w, "vf_base",  "30");
    work_set_param(w, "vf_width", "90");
    work_set_param(w, "seq_len", "16");
    work_set_param(w, "seq_on",  "1");
    for (int i = 0; i < 16; i += 4) {
        char k[16];
        snprintf(k, sizeof k, "step%d", i);
        work_set_param(w, k, "1:0:0:0");
    }
    work_set_param(w, "step4",  "1:0:0:0:60");   /* 60% probability         */
    work_set_param(w, "locks8", "0=100,9=20");   /* a locked step           */
    work_set_param(w, "step8",  "1:0:0:0");

    work_set_param(w, "track", "1");
    work_set_param(w, "src",      "21");     /* One Shot                    */
    work_set_param(w, "machine1", "17");     /* Iron Room Reverb            */
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "track", "3");
    work_set_param(w, "src", "20");          /* Granulator                  */
    work_set_param(w, "track", "0");

    /* Render a few blocks so the effective values are resolved rather than
     * sitting at their base — the whole point of the fe* mirror. */
    static int16_t in[128 * 2], out[128 * 2];
    for (int b = 0; b < 8; ++b) {
        memset(in, 0, sizeof in);
        work_process(w, in, out, 128);
    }

    static char blob[1 << 20];
    int total = 0;
    char lenbuf[32];
    work_get_param(w, "state_len", lenbuf, sizeof lenbuf);
    const int want = atoi(lenbuf);

    /* Read it the way the UI must — through the window protocol — so the
     * harness also proves the assembled blob is whole. */
    while (total < want) {
        char key[32];
        if (total == 0) snprintf(key, sizeof key, "state");
        else            snprintf(key, sizeof key, "state@%d", total);
        int n = work_get_param(w, key, blob + total, 16384);
        if (n <= 0) break;
        total += n;
    }
    blob[total] = '\0';

    fputs(blob, stdout);
    fputc('\n', stdout);
    work_destroy(w);
    return 0;
}
