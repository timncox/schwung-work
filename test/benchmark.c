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
        work_set_param(w, "fx1", val);
        work_set_param(w, "fx2", val);

        int16_t work_buf[BLOCK * 2];
        double t0 = now_sec();
        for (int b = 0; b < blocks; ++b) {
            memcpy(work_buf, buf, sizeof(buf));
            work_process(w, work_buf, work_buf, BLOCK);
        }
        double dt = now_sec() - t0;
        printf("\n  worst case: 2 x %s = %.0f x realtime\n",
               work_machine_name(worst_mc), dt > 0.0 ? (double)SECONDS / dt : 0.0);
        work_destroy(w);
    }

    printf("\n  Not a Move measurement. Measure on hardware before\n"
           "  claiming headroom — see CLAUDE.md.\n");
    return 0;
}
