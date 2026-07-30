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
/* One track: an SRC machine, then two insert FX, then the voice filter doing
 * real work, with voices sounding. */
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

/* Load a machine into a STAGE, by the stage's own key.
 *
 * "src", then "fx1".."fxN" — and note that fx1 is stage 1, not stage 0. This
 * used to write "fx%d" for every stage including the source, so the source
 * machine went to insert 1, where the family gate refused it: the profiles
 * measured two effects over an EMPTY source stage with no voices sounding at
 * all. Every figure recorded before 2026-07-30 is that, not what its label
 * says. The gate did its job silently, which is exactly why a benchmark has to
 * check what it actually loaded. */
static void set_stage(work_t *w, int stage, int machine) {
    char k[8], v[8];
    if (stage == WORK_STAGE_SRC) snprintf(k, sizeof(k), "src");
    else                         snprintf(k, sizeof(k), "fx%d", stage);
    snprintf(v, sizeof(v), "%d", machine);
    work_set_param(w, k, v);

    work_get_param(w, k, v, sizeof(v));
    if (atoi(v) != machine) {
        fprintf(stderr, "bench: stage %d refused machine %d (holds %s) — the "
                        "profile is not what it says it is\n", stage, machine, v);
        exit(1);
    }
}

/* Configure the SELECTED track of `w` to the profile, and start its voices.
 * `chan` is the MIDI channel the notes arrive on, which is how the engine
 * routes them to a track. */
static void arm_track(work_t *w, const profile_t *p, int chan,
                      const int16_t *pcm, int pcm_frames)
{
    work_set_param(w, "sample_begin", "44100:bench");
    send_pcm(w, pcm, pcm_frames);

    set_stage(w, WORK_STAGE_SRC,     p->src);
    set_stage(w, WORK_STAGE_FX1,     p->fx1);
    set_stage(w, WORK_STAGE_FX1 + 1, p->fx2);

    /* Give the voice filter something to do; wide open it is skipped. */
    work_set_param(w, "vf_base",  "40");
    work_set_param(w, "vf_width", "40");
    work_set_param(w, "vf_env",   "100");

    for (int v = 0; v < p->voices; ++v) {
        uint8_t on[3] = { (uint8_t)(0x90 | (chan & 0x0F)), (uint8_t)(48 + v * 3), 100 };
        work_on_midi(w, on, 3, 3);
    }
}

/* ------------------------------------------------------------------ modes
 *
 * Two ways to render n tracks, and the difference between them is the whole
 * point of this file now that the engine has real tracks.
 *
 *   instances   n separate work_t, summed by the caller. What this benchmark
 *               measured before the engine had more than one track, and still
 *               the only way to go past WORK_TRACKS.
 *   engine      ONE work_t with n of its own tracks, summed internally.
 *
 * The engine mode is the real cost. The instance mode is an upper bound on it,
 * because everything a single instance shares between its tracks — the
 * sequencer, the transport, the pattern, the mix stage — is paid n times over
 * when there are n instances. How much that is worth is the delta.
 */
static double run_instances(const profile_t *p, int n, const int16_t *src,
                            const int16_t *pcm, int pcm_frames) {
    work_t *trk[MAX_TRK];
    for (int i = 0; i < n; ++i) {
        trk[i] = work_create(&host);
        if (!trk[i]) { fprintf(stderr, "allocation failed at track %d\n", i); exit(1); }
        arm_track(trk[i], p, 0, pcm, pcm_frames);
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

static double run_engine(const profile_t *p, int n, const int16_t *src,
                         const int16_t *pcm, int pcm_frames) {
    if (n > WORK_TRACKS) return 0.0;             /* not expressible; caller prints - */

    work_t *w = work_create(&host);
    if (!w) { fprintf(stderr, "allocation failed\n"); exit(1); }

    for (int i = 0; i < n; ++i) {
        char t[8];
        snprintf(t, sizeof(t), "%d", i);
        work_set_param(w, "track", t);
        arm_track(w, p, i, pcm, pcm_frames);
    }
    work_set_param(w, "track", "0");

    const int blocks = (WORK_SR * SECONDS) / BLOCK;
    int16_t  tbuf[BLOCK * 2];
    volatile int64_t sink = 0;

    double t0 = now_sec();
    for (int b = 0; b < blocks; ++b) {
        memcpy(tbuf, src, sizeof(tbuf));
        work_process(w, tbuf, tbuf, BLOCK);      /* every track, one call */
        for (int f = 0; f < BLOCK * 2; ++f) sink += tbuf[f];
    }
    double dt = now_sec() - t0;
    (void)sink;

    work_destroy(w);
    return dt > 0.0 ? (double)SECONDS / dt : 0.0;
}

static void print_row(const char *label,
                      double (*run)(const profile_t *, int, const int16_t *,
                                    const int16_t *, int),
                      const profile_t *p, const int16_t *src,
                      const int16_t *pcm, double *out)
{
    printf("  %-26s", label);
    for (int c = 0; c < N_COUNTS; ++c) {
        double rt = run(p, TRACK_COUNTS[c], src, pcm, 44100);
        out[c] = rt;
        if (rt <= 0.0) printf("%9s   ", "-");
        else           printf("%8.0f%%   ", 100.0 / rt);
    }
    printf("\n");
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
    printf("One track = 1 SRC machine + 2 insert FX + voice filter + %d..%d voices.\n",
           PROFILE[0].voices, PROFILE[N_PROFILE - 1].voices);
    printf("%d s of audio per cell. Lower %%core is better; over ~50%% will not ship.\n",
           SECONDS);
    printf("This engine has WORK_TRACKS = %d, so 'engine' stops there.\n\n", WORK_TRACKS);

    for (int p = 0; p < N_PROFILE; ++p) {
        double inst[N_COUNTS], eng[N_COUNTS];

        printf("  %s\n", PROFILE[p].name);
        printf("  %-26s", "");
        for (int c = 0; c < N_COUNTS; ++c) printf("%9d trk", TRACK_COUNTS[c]);
        printf("\n");

        print_row("  N instances", run_instances, &PROFILE[p], src, pcm, inst);
        print_row("  1 engine, N tracks", run_engine,    &PROFILE[p], src, pcm, eng);

        /* The delta the restructure was supposed to buy. Positive means the
         * real engine is cheaper than N copies of a one-track one. */
        printf("  %-26s", "  delta");
        for (int c = 0; c < N_COUNTS; ++c) {
            if (eng[c] <= 0.0 || inst[c] <= 0.0) { printf("%9s   ", "-"); continue; }
            double a = 100.0 / inst[c], b = 100.0 / eng[c];
            printf("%8.0f%%   ", a - b);
        }
        printf("\n\n");
    }

    printf("  Cells are %%of one core. Move runs its own engine alongside\n"
           "  this, so budget well under 100%%.\n");
    return 0;
}
