/*
 * Work — how many TRACKS fit?
 *
 * The per-machine benchmark answers "which machine is expensive". This one
 * answers the question that decides Work's architecture: if a track is one SRC
 * machine plus two insert FX — the shape the reference device uses — how many
 * of those can run at once before the audio thread runs out of block?
 *
 * A track is modelled as one work_t. That is not an approximation of the
 * eventual multi-track engine, it IS the per-track cost: work_t already holds
 * exactly one machine chain, one voice pool, one voice filter and one set of
 * modulators. Whatever a restructured engine shares between tracks can only
 * make this cheaper, so the numbers here are an upper bound on the real cost.
 *
 * BUILD FOR THE MOVE AND RUN IT THERE. A laptop figure from this file settles
 * nothing — the whole point is that the A53 has never been measured. See
 * `make bench-tracks-arm`.
 *
 * What the numbers mean:
 *   x realtime   how many times faster than the audio it renders. 1.0 is
 *                exactly keeping up with nothing to spare.
 *   %core        share of one core the same work needs, = 100 / x realtime.
 *
 * Move runs its own audio engine alongside whatever a module does, so treat
 * anything over ~50 %core as unshippable and the 25-50 %core band as needing
 * a listen for crackle rather than a number.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "work_core.h"

#define BLOCK    128
#define SECONDS  4
#define MAX_TRK  16

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

/* Same base64-over-the-param-channel route the UI uses, because that is the
 * only way audio gets into the engine — work_set_param must never touch a
 * file, it runs on the audio thread. */
static void send_pcm(work_t *w, const int16_t *pcm, int frames) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static char b64[16 * 1024];
    const int chunk = 2048;
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

/* One track: an SRC machine, then two insert FX, then the voice filter doing
 * real work. Slot 0 stands in for the dedicated SRC stage a multi-track engine
 * would have — the cost is the same either way, it is the same DSP. */
typedef struct { const char *name; int src, fx1, fx2; int voices; } profile_t;

static const profile_t PROFILE[] = {
    /* A quiet track: a one-shot and two cheap shaping effects. */
    { "light  1shot+tilt+fbank",   WORK_FX_ONESHOT,    WORK_FX_TILT,
                                   WORK_FX_FBANK,      2 },
    /* What most tracks in a real patch would look like. */
    { "mid    poly+mmf+drivedly",  WORK_FX_POLYSAMPLE, WORK_FX_MMF,
                                   WORK_FX_DRIVEDELAY, 4 },
    /* Every track doing the most expensive thing it can. Nobody patches this,
     * but if it fits then nothing else can fail to. */
    { "heavy  poly+roomtone+void", WORK_FX_POLYSAMPLE, WORK_FX_ROOMTONE,
                                   WORK_FX_VOIDSPACE,  8 },
};
#define N_PROFILE ((int)(sizeof(PROFILE) / sizeof(PROFILE[0])))

static const int TRACK_COUNTS[] = { 1, 2, 4, 8, 12, 16 };
#define N_COUNTS ((int)(sizeof(TRACK_COUNTS) / sizeof(TRACK_COUNTS[0])))

static void set_slot(work_t *w, int slot, int machine) {
    char k[8], v[8];
    snprintf(k, sizeof(k), "fx%d", slot + 1);
    snprintf(v, sizeof(v), "%d", machine);
    work_set_param(w, k, v);
}

/* Render `n` tracks for SECONDS of audio and return the realtime factor.
 * Tracks are summed into one bus, as they would be on the way to the mixer,
 * so the sum is not optimised away. */
static double run_tracks(const profile_t *p, int n, const int16_t *src,
                         const int16_t *pcm, int pcm_frames) {
    work_t *trk[MAX_TRK];
    for (int i = 0; i < n; ++i) {
        trk[i] = work_create(&host);
        if (!trk[i]) { fprintf(stderr, "allocation failed at track %d\n", i); exit(1); }

        work_set_param(trk[i], "sample_begin", "44100:bench");
        send_pcm(trk[i], pcm, pcm_frames);

        set_slot(trk[i], 0, p->src);
        set_slot(trk[i], 1, p->fx1);
        set_slot(trk[i], 2, p->fx2);
        /* Any slot the profile does not name stays Bypass — a four-slot build
         * must not quietly charge this measurement for a slot the target
         * architecture does not have. */
        for (int s = 3; s < WORK_STAGES; ++s) set_slot(trk[i], s, WORK_FX_BYPASS);

        /* Give the voice filter something to do; wide open it is skipped. */
        work_set_param(trk[i], "vf_base",  "40");
        work_set_param(trk[i], "vf_width", "40");
        work_set_param(trk[i], "vf_env",   "100");

        for (int v = 0; v < p->voices; ++v) {
            uint8_t on[3] = { 0x90, (uint8_t)(48 + v * 3), 100 };
            work_on_midi(trk[i], on, 3, 3);
        }
    }

    const int blocks = (WORK_SR * SECONDS) / BLOCK;
    int16_t  tbuf[BLOCK * 2];
    int32_t  bus[BLOCK * 2];
    volatile int64_t sink = 0;

    double t0 = now_sec();
    for (int b = 0; b < blocks; ++b) {
        memset(bus, 0, sizeof(bus));
        for (int i = 0; i < n; ++i) {
            memcpy(tbuf, src, sizeof(tbuf));
            work_process(trk[i], tbuf, tbuf, BLOCK);
            for (int f = 0; f < BLOCK * 2; ++f) bus[f] += tbuf[f];
        }
        for (int f = 0; f < BLOCK * 2; ++f) sink += bus[f];
    }
    double dt = now_sec() - t0;
    (void)sink;

    for (int i = 0; i < n; ++i) work_destroy(trk[i]);
    return dt > 0.0 ? (double)SECONDS / dt : 0.0;
}

int main(void) {
    int16_t src[BLOCK * 2];
    double  ph = 0.0;
    for (int i = 0; i < BLOCK; ++i) {
        float v = 0.5f * sinf((float)(ph * 2.0 * M_PI));
        ph += 220.0 / WORK_SR;
        src[i * 2] = src[i * 2 + 1] = (int16_t)(v * 22000.0f);
    }

    static int16_t pcm[44100 * 2];
    for (int i = 0; i < 44100; ++i) {
        float v = sinf((float)i * 0.05f) * 0.5f;
        pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(v * 32000.0f);
    }

    printf("Work — track-count cost\n");
    printf("One track = 1 SRC machine + 2 insert FX + voice filter + voices.\n");
    printf("%d s of audio per cell. Lower %%core is better; over ~50%% will not ship.\n\n",
           SECONDS);

    printf("  %-26s", "profile");
    for (int c = 0; c < N_COUNTS; ++c) printf("%9d trk", TRACK_COUNTS[c]);
    printf("\n");

    for (int p = 0; p < N_PROFILE; ++p) {
        printf("  %-26s", PROFILE[p].name);
        for (int c = 0; c < N_COUNTS; ++c) {
            double rt = run_tracks(&PROFILE[p], TRACK_COUNTS[c], src, pcm, 44100);
            printf("%8.0f%%   ", rt > 0.0 ? 100.0 / rt : 999.0);
        }
        printf("\n");
    }

    printf("\n  Cells are %%of one core. Move runs its own engine alongside\n"
           "  this, so budget well under 100%%.\n");
    return 0;
}
