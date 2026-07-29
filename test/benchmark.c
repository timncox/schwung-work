/*
 * Work — per-machine cost benchmark.
 *
 * Reports a realtime factor for each machine on THIS machine. That is a
 * relative ranking, not a Move measurement: the Move's A53 core is far slower
 * and shares the CPU with Move's own engine. Use this to find which machines
 * are expensive relative to each other, then measure the expensive ones on
 * hardware before claiming anything about headroom.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "work_core.h"

#define BLOCK   128
#define SECONDS 4

static float sim_bpm(void) { return 120.0f; }

static host_api_v1_t host = {
    .api_version      = MOVE_PLUGIN_API_VERSION,
    .sample_rate      = MOVE_SAMPLE_RATE,
    .frames_per_block = BLOCK,
    .get_bpm          = sim_bpm
};

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* The sample transfer is base64 over the param channel; the benchmark needs a
 * loaded buffer, so it uses the same protocol the UI does. */
static void send_pcm(work_t *w, const int16_t *pcm, int frames) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static char b64[16 * 1024];
    const int chunk = 2048;                       /* frames per chunk */
    for (int at = 0; at < frames; at += chunk) {
        int nf = frames - at; if (nf > chunk) nf = chunk;
        const uint8_t *in = (const uint8_t *)(pcm + at * 2);
        int n = nf * 2 * (int)sizeof(int16_t), o = 0;
        for (int i = 0; i < n; i += 3) {
            unsigned v = (unsigned)in[i] << 16;
            if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
            if (i + 2 < n) v |= in[i + 2];
            b64[o++] = T[(v >> 18) & 63];
            b64[o++] = T[(v >> 12) & 63];
            b64[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
            b64[o++] = (i + 2 < n) ? T[v & 63] : '=';
        }
        b64[o] = '\0';
        work_set_param(w, "sample_chunk", b64);
    }
    work_set_param(w, "sample_end", "1");
}

int main(void) {
    const int blocks = (WORK_SR * SECONDS) / BLOCK;

    int16_t buf[BLOCK * 2];
    double  ph = 0.0;
    for (int i = 0; i < BLOCK; ++i) {
        float v = 0.5f * sinf((float)(ph * 2.0 * M_PI));
        ph += 220.0 / WORK_SR;
        buf[i * 2] = buf[i * 2 + 1] = (int16_t)(v * 22000.0f);
    }

    printf("Work — per-machine cost (%d s of audio per machine)\n", SECONDS);
    printf("Realtime factor: higher is cheaper. Host machine only.\n\n");
    printf("  %-20s %10s  %s\n", "machine", "x realtime", "");

    double worst = 1e9;
    int    worst_mc = 0;

    for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
        work_t *w = work_create(&host);
        if (!w) { printf("allocation failed\n"); return 1; }

        char val[8];
        snprintf(val, sizeof(val), "%d", mc);
        work_set_param(w, "fx1", val);

        int16_t work_buf[BLOCK * 2];
        double t0 = now_sec();
        for (int b = 0; b < blocks; ++b) {
            memcpy(work_buf, buf, sizeof(buf));
            work_process(w, work_buf, work_buf, BLOCK);
        }
        double dt = now_sec() - t0;
        double rt = dt > 0.0 ? (double)SECONDS / dt : 0.0;

        printf("  %-20s %10.0f\n", work_machine_name(mc), rt);
        if (rt < worst) { worst = rt; worst_mc = mc; }

        work_destroy(w);
    }

    /* Both slots loaded with the most expensive machine — the realistic worst
     * case for a patch that uses the full FX section. */
    {
        work_t *w = work_create(&host);
        char val[8];
        snprintf(val, sizeof(val), "%d", worst_mc);
        for (int i = 1; i <= WORK_SLOTS; ++i) {
            char k[8];
            snprintf(k, sizeof(k), "fx%d", i);
            work_set_param(w, k, val);
        }

        int16_t work_buf[BLOCK * 2];
        double t0 = now_sec();
        for (int b = 0; b < blocks; ++b) {
            memcpy(work_buf, buf, sizeof(buf));
            work_process(w, work_buf, work_buf, BLOCK);
        }
        double dt = now_sec() - t0;
        printf("\n  worst case: %d x %s = %.0f x realtime\n", WORK_SLOTS,
               work_machine_name(worst_mc), dt > 0.0 ? (double)SECONDS / dt : 0.0);
        work_destroy(w);
    }

    /* What the voice filter costs when it is actually doing something. It is
     * two state-variable filters per channel per SOUNDING VOICE, so the price
     * is paid per note rather than per machine — eight held notes cost eight
     * times one. Printed rather than asserted: this host is not a Move. */
    {
        work_t *w = work_create(&host);
        static int16_t pcm[44100 * 2];
        for (int i = 0; i < 44100; ++i) {
            float v = sinf((float)i * 0.05f) * 0.5f;
            pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(v * 32000.0f);
        }
        work_set_param(w, "sample_begin", "44100:bench");
        send_pcm(w, pcm, 44100);

        work_set_param(w, "machine1", "22");        /* Polysample */
        work_set_param(w, "fx1_p6", "127");
        work_set_param(w, "vf_base", "40");
        work_set_param(w, "vf_width", "40");
        work_set_param(w, "vf_env", "100");
        for (int n = 0; n < 8; ++n) {
            uint8_t on[3] = { 0x90, (uint8_t)(48 + n * 3), 100 };
            work_on_midi(w, on, 3, 3);
        }

        int16_t work_buf[BLOCK * 2];
        double t0 = now_sec();
        for (int b = 0; b < blocks; ++b) {
            memcpy(work_buf, buf, sizeof(buf));
            work_process(w, work_buf, work_buf, BLOCK);
        }
        double dt = now_sec() - t0;
        printf("  8 voices + voice filter = %.0f x realtime\n",
               dt > 0.0 ? (double)SECONDS / dt : 0.0);
        work_destroy(w);
    }

    printf("\n  Not a Move measurement. Measure on hardware before\n"
           "  claiming headroom — see CLAUDE.md.\n");
    return 0;
}
