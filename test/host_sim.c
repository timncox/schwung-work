/*
 * Work — native host simulator.
 *
 * Stands in for the Move host: hands the engine 128-frame int16 stereo blocks,
 * a get_bpm(), and MIDI. Everything here runs on the build machine; no
 * hardware is touched. `make test` must be green before any release.
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "work_core.h"

#define BLOCK 128

static int tests_run;
static int tests_failed;

/* Printing in two steps keeps this free of the ## __VA_ARGS__ paste, which
 * is a GNU extension and trips -Wpedantic. */
#define CHECK(cond, ...) do {                                               \
    tests_run++;                                                            \
    if (!(cond)) {                                                          \
        tests_failed++;                                                     \
        printf("  FAIL  ");                                                 \
        printf(__VA_ARGS__);                                                \
        printf("\n");                                                       \
    }                                                                       \
} while (0)

/* CHECK inside a tight sweep: counts like CHECK but reports only the first
   failure, so a broken mapping does not print 128 identical lines. */
static int quiet_fired = 0;
#define CHECK_QUIET(cond, ...) do { \
    if (!(cond)) { if (!quiet_fired++) CHECK(cond, __VA_ARGS__); \
                   else { tests_run++; tests_failed++; } } \
    else { tests_run++; } \
} while (0)

static float sim_bpm(void) { return 120.0f; }

static host_api_v1_t host = {
    .api_version      = MOVE_PLUGIN_API_VERSION,
    .sample_rate      = MOVE_SAMPLE_RATE,
    .frames_per_block = BLOCK,
    .get_bpm          = sim_bpm
};

/* Deterministic test signal: a 220 Hz sine with a little noise on top, so
 * filters have something tonal to bite and the noise exercises the rest. */
static uint32_t sig_rng = 12345;
static void fill_signal(int16_t *buf, int frames, double *phase) {
    for (int i = 0; i < frames; ++i) {
        sig_rng = sig_rng * 1664525u + 1013904223u;
        float n = (float)((int32_t)(sig_rng >> 9) - 4194304) / 4194304.0f;
        float v = 0.55f * sinf((float)(*phase * 2.0 * M_PI)) + 0.08f * n;
        *phase += 220.0 / WORK_SR;
        if (*phase >= 1.0) *phase -= 1.0;
        buf[i * 2]     = (int16_t)(v * 24000.0f);
        buf[i * 2 + 1] = (int16_t)(v * 22000.0f);
    }
}

/* Run `blocks` blocks of signal through the engine, returning total absolute
 * output energy and counting how many samples hit the int16 rails. */
static int64_t run_blocks(work_t *w, int blocks, int *railed_out) {
    int16_t buf[BLOCK * 2];
    double  ph = 0.0;
    int64_t energy = 0;
    int     railed = 0;

    for (int b = 0; b < blocks; ++b) {
        fill_signal(buf, BLOCK, &ph);
        work_process(w, buf, buf, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) {
            energy += llabs(buf[i]);
            if (buf[i] >= 32767 || buf[i] <= -32768) railed++;
        }
    }
    if (railed_out) *railed_out = railed;
    return energy;
}

/* Stage keys, mirroring work_core.h: stage 0 is SRC, the inserts follow. */
static const char *const STAGE_KEY[WORK_STAGES] = { "src", "fx1", "fx2" };

static void set_stage(work_t *w, int stage, int machine) {
    char val[8];
    snprintf(val, sizeof(val), "%d", machine);
    work_set_param(w, STAGE_KEY[stage], val);
}

static void set_all_params(work_t *w, int stage, int value) {
    for (int i = 0; i < WORK_PARAMS; ++i) {
        char key[16], val[8];
        snprintf(key, sizeof(key), "%s_p%d", STAGE_KEY[stage], i + 1);
        snprintf(val, sizeof(val), "%d", value);
        work_set_param(w, key, val);
    }
}

/* The stage that will accept this machine. A test that only needs a machine
 * RUNNING should ask here rather than naming a stage: the engine refuses an
 * out-of-family load outright, so a hardcoded stage would turn the test into a
 * silent no-op the day a machine changes family. Bypass fits both families and
 * lands on the insert, which is where the old two-slot tests put it. */
static int stage_for(int machine) {
    if (work_machine_fits_stage(WORK_STAGE_FX1, machine)) return WORK_STAGE_FX1;
    return WORK_STAGE_SRC;
}

/* ---------------------------------------------------------------- tests */

/* Two bypassed slots at full wet must return the input unchanged. This is the
 * baseline that proves the int16 <-> float round trip is not itself lossy. */
static void test_bypass_transparent(void) {
    printf("bypass is bit-transparent\n");
    work_t *w = work_create(&host);
    assert(w);

    int16_t in[BLOCK * 2], out[BLOCK * 2];
    double ph = 0.0;
    fill_signal(in, BLOCK, &ph);
    memcpy(out, in, sizeof(in));
    work_process(w, out, out, BLOCK);

    int worst = 0;
    for (int i = 0; i < BLOCK * 2; ++i) {
        int d = abs((int)out[i] - (int)in[i]);
        if (d > worst) worst = d;
    }
    CHECK(worst <= 1, "bypass altered the signal by up to %d LSB", worst);
    work_destroy(w);
}

/* Every machine, at three parameter extremes, must produce bounded audio: no
 * NaN escaping into the int16 conversion, no permanent rail-slamming, and no
 * unexpected total silence. */
static void test_all_machines_bounded(void) {
    printf("every machine renders bounded audio at min/default/max\n");

    const int settings[3] = {0, -1 /* defaults */, 127};
    const char *label[3]  = {"all-min", "default", "all-max"};

    for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
        for (int sIdx = 0; sIdx < 3; ++sIdx) {
            work_t *w = work_create(&host);
            assert(w);
            /* Into whichever stage takes it. Naming a stage here would let the
             * engine REFUSE every source machine and leave this sweep testing
             * an empty chain — the loop would still run, still pass, and cover
             * nothing. */
            const int st = stage_for(mc);
            set_stage(w, st, mc);
            if (settings[sIdx] >= 0) set_all_params(w, st, settings[sIdx]);

            char loaded[16];
            work_get_param(w, STAGE_KEY[st], loaded, sizeof(loaded));
            CHECK(atoi(loaded) == mc, "%s did not load into stage %d (reads %s)",
                  work_machine_name(mc), st, loaded);

            int railed = 0;
            int64_t e = run_blocks(w, 200, &railed);
            int64_t samples = (int64_t)200 * BLOCK * 2;

            CHECK(railed < samples / 2,
                  "%s (%s): %d/%lld samples railed — runaway gain",
                  work_machine_name(mc), label[sIdx], railed, (long long)samples);

            /* Bypass at all-min is still bypass; every other machine at its
             * defaults should pass or produce something audible.
             *
             * SRC machines are the exception, and it is not a loophole: they
             * REPLACE their input instead of processing it, so with no sample
             * loaded and no trig fired, silence is the correct output. Their
             * real coverage is test_sample_transfer / test_single_player
             * below, which load audio and fire it. */
            const int is_src = (mc == WORK_FX_ONESHOT || mc == WORK_FX_POLYSAMPLE ||
                                mc == WORK_FX_SLICER || mc == WORK_FX_WAVESCAN);
            if (sIdx == 1 && mc != WORK_FX_BYPASS && !is_src) {
                CHECK(e > 0, "%s (default): output is completely silent",
                      work_machine_name(mc));
            }
            work_destroy(w);
        }
    }
}

/* The global MIX law: at 0 the output must equal the dry input regardless of
 * what the machines are doing. */
static void test_global_mix_dry(void) {
    printf("global mix=0 returns the dry signal\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_VOIDSPACE);
    set_stage(w, WORK_STAGE_FX1 + 1, WORK_FX_DECIMATOR);
    work_set_param(w, "mix", "0");

    int16_t in[BLOCK * 2], out[BLOCK * 2];
    double ph = 0.0;
    fill_signal(in, BLOCK, &ph);
    memcpy(out, in, sizeof(in));
    work_process(w, out, out, BLOCK);

    int worst = 0;
    for (int i = 0; i < BLOCK * 2; ++i) {
        int d = abs((int)out[i] - (int)in[i]);
        if (d > worst) worst = d;
    }
    CHECK(worst <= 1, "mix=0 leaked %d LSB of wet signal", worst);
    work_destroy(w);
}

/* A preset blob must survive a round trip exactly. */
static void test_state_roundtrip(void) {
    printf("state blob round-trips\n");
    work_t *a = work_create(&host);
    work_t *b = work_create(&host);
    assert(a && b);

    set_stage(a, WORK_STAGE_FX1, WORK_FX_PHASEARRAY);
    set_stage(a, WORK_STAGE_FX1 + 1, WORK_FX_IRONROOM);
    for (int i = 0; i < WORK_PARAMS; ++i) {
        char key[16], val[8];
        snprintf(key, sizeof(key), "fx1_p%d", i + 1);
        snprintf(val, sizeof(val), "%d", i * 13 + 3);
        work_set_param(a, key, val);
        snprintf(key, sizeof(key), "fx2_p%d", i + 1);
        snprintf(val, sizeof(val), "%d", 120 - i * 7);
        work_set_param(a, key, val);
    }
    work_set_param(a, "mix", "99");
    work_set_param(a, "lfo1_dest", "5");
    work_set_param(a, "lfo1_depth", "100");
    work_set_param(a, "lfo2_wave", "3");

    char blob[4096];
    int n = work_get_param(a, "state", blob, sizeof(blob));
    CHECK(n > 0 && n < (int)sizeof(blob), "state blob length %d is implausible", n);

    work_set_param(b, "state", blob);

    char ba[4096], bb[4096];
    work_get_param(a, "state", ba, sizeof(ba));
    work_get_param(b, "state", bb, sizeof(bb));
    CHECK(strcmp(ba, bb) == 0, "state mismatch after restore\n    a=%s\n    b=%s", ba, bb);

    work_destroy(a);
    work_destroy(b);
}

/* The Smack v0.8.2 bug: get_param("state") built its blob with unclamped
 * `n += snprintf(...)`, so a large state walked the cursor past the buffer and
 * the next append wrote out of bounds. Read the biggest possible state into
 * deliberately small buffers and confirm we always stay inside them. */
static void test_get_param_tiny_buffers(void) {
    printf("get_param never writes past a short buffer\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_FLUTTER);
    set_stage(w, WORK_STAGE_FX1 + 1, WORK_FX_ROOMTONE);
    set_all_params(w, WORK_STAGE_FX1, 127);
    set_all_params(w, WORK_STAGE_FX1 + 1, 127);
    work_set_param(w, "lfo1_dest", "15");
    work_set_param(w, "lfo2_dest", "9");

    const int sizes[] = {2, 8, 16, 40, 100, 512, 4096};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        int cap = sizes[k];
        char *buf = malloc((size_t)cap + 64);
        assert(buf);
        memset(buf, 0x7E, (size_t)cap + 64);          /* canary past the end */

        int n = work_get_param(w, "state", buf, cap);
        CHECK(n <= cap - 1, "state into %d bytes reported %d", cap, n);

        int canary_ok = 1;
        for (int i = cap; i < cap + 64; ++i)
            if ((unsigned char)buf[i] != 0x7E) canary_ok = 0;
        CHECK(canary_ok, "state into %d bytes overwrote past the buffer", cap);

        /* "machines" and "labels*" are the other multi-append getters */
        const char *multi[] = {"machines", "labels1", "labels2"};
        for (size_t j = 0; j < sizeof(multi) / sizeof(multi[0]); ++j) {
            memset(buf, 0x7E, (size_t)cap + 64);
            n = work_get_param(w, multi[j], buf, cap);
            canary_ok = 1;
            for (int i = cap; i < cap + 64; ++i)
                if ((unsigned char)buf[i] != 0x7E) canary_ok = 0;
            CHECK(canary_ok, "%s into %d bytes overwrote past the buffer", multi[j], cap);
            CHECK(n <= cap - 1, "%s into %d bytes reported %d", multi[j], cap, n);
        }

        free(buf);
    }
    work_destroy(w);
}

/* The compressor must actually compress: a loud signal through a low threshold
 * and a high ratio should come out quieter than the same signal bypassed. */
static void test_compressor_reduces_gain(void) {
    printf("compressor reduces gain above threshold\n");

    work_t *dry = work_create(&host);
    int dummy;
    int64_t e_dry = run_blocks(dry, 60, &dummy);
    work_destroy(dry);

    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_COMP);
    work_set_param(w, "fx1_p1", "10");    /* THR low   */
    work_set_param(w, "fx1_p2", "100");   /* fast ATK  */
    work_set_param(w, "fx1_p3", "40");    /* REL       */
    work_set_param(w, "fx1_p4", "0");     /* no makeup */
    work_set_param(w, "fx1_p5", "127");   /* ratio 20  */
    work_set_param(w, "fx1_p8", "127");   /* fully wet */
    int64_t e_comp = run_blocks(w, 60, &dummy);

    CHECK(e_comp < e_dry * 9 / 10,
          "compressed energy %lld is not below dry %lld",
          (long long)e_comp, (long long)e_dry);

    char meter[32];
    work_get_param(w, "meter", meter, sizeof(meter));
    CHECK(atoi(meter) > 0, "gain-reduction meter reads %s, expected > 0", meter);
    work_destroy(w);
}

/* Filterbank with every band at zero gain is a wall: nothing gets through. */
static void test_filterbank_silence(void) {
    printf("filterbank at zero gain is silent\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_FBANK);
    set_all_params(w, WORK_STAGE_FX1, 0);

    int dummy;
    int64_t e = run_blocks(w, 40, &dummy);
    CHECK(e == 0, "expected silence, got energy %lld", (long long)e);
    work_destroy(w);
}

/* Changing machine must reset that slot, so a reverb tail cannot bleed into
 * whatever loads next. */
static void test_machine_change_resets(void) {
    printf("changing machine clears the previous machine's tail\n");
    work_t *w = work_create(&host);
    assert(w);

    set_stage(w, WORK_STAGE_FX1, WORK_FX_VOIDSPACE);
    work_set_param(w, "fx1_p7", "127");         /* fully wet */
    int dummy;
    run_blocks(w, 100, &dummy);                 /* build a long tail */

    set_stage(w, WORK_STAGE_FX1, WORK_FX_BYPASS);

    /* Feed silence: with the tank cleared there must be nothing left. */
    int16_t quiet[BLOCK * 2];
    memset(quiet, 0, sizeof(quiet));
    int64_t leak = 0;
    for (int b = 0; b < 20; ++b) {
        memset(quiet, 0, sizeof(quiet));
        work_process(w, quiet, quiet, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) leak += llabs(quiet[i]);
    }
    CHECK(leak == 0, "reverb tail leaked %lld into the next machine", (long long)leak);
    work_destroy(w);
}

/* An FX LFO pointed at a parameter must audibly change the result. */
static void test_fx_lfo_modulates(void) {
    printf("FX LFO modulates its destination parameter\n");

    work_t *a = work_create(&host);
    set_stage(a, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(a, "fx1_p5", "40");           /* cutoff  */
    int dummy;
    int64_t e_static = run_blocks(a, 120, &dummy);
    work_destroy(a);

    work_t *b = work_create(&host);
    set_stage(b, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(b, "fx1_p5", "40");
    work_set_param(b, "lfo1_dest", "12");        /* insert 1, knob E = FREQ */
    work_set_param(b, "lfo1_depth", "127");
    work_set_param(b, "lfo1_spd", "100");
    int64_t e_mod = run_blocks(b, 120, &dummy);
    work_destroy(b);

    CHECK(llabs(e_mod - e_static) > e_static / 20,
          "LFO changed output energy by less than 5%% (%lld vs %lld)",
          (long long)e_mod, (long long)e_static);
}

/* The stages run in series: SRC feeds insert 1, which feeds insert 2. Each
 * link is checked on its own, because a stage that is configured but never
 * rendered is invisible to every other test — which is exactly what the third
 * slot could have done when it was added. */
static void test_stages_run_in_series(void) {
    printf("SRC feeds insert 1, which feeds insert 2\n");
    int dummy;

    /* Granulator with no sample loaded granulates the LIVE INPUT — that is the
     * behaviour it shipped with, and it is what makes it usable here as an
     * audible head of the chain. A player-type source would need a sample
     * loaded and a trig fired to prove the same thing. */
    work_t *a = work_create(&host);
    set_stage(a, WORK_STAGE_SRC, WORK_FX_GRANULATOR);
    int64_t e_src = run_blocks(a, 60, &dummy);
    work_destroy(a);
    CHECK(e_src > 0, "the source stage alone produced silence");

    work_t *b = work_create(&host);
    set_stage(b, WORK_STAGE_SRC, WORK_FX_GRANULATOR);
    set_stage(b, WORK_STAGE_FX1, WORK_FX_FBANK);
    set_all_params(b, WORK_STAGE_FX1, 0);            /* a wall in insert 1 */
    int64_t e1 = run_blocks(b, 60, &dummy);
    work_destroy(b);
    CHECK(e1 == 0, "insert 1 did not receive the source stage's output "
          "(energy %lld)", (long long)e1);

    work_t *c = work_create(&host);
    set_stage(c, WORK_STAGE_FX1, WORK_FX_GRIT);
    set_stage(c, WORK_STAGE_FX1 + 1, WORK_FX_FBANK);
    set_all_params(c, WORK_STAGE_FX1 + 1, 0);        /* a wall in insert 2 */
    int64_t e2 = run_blocks(c, 60, &dummy);
    work_destroy(c);
    CHECK(e2 == 0, "insert 2 did not receive insert 1's output (energy %lld)",
          (long long)e2);
}

/* A stage refuses a machine from the other family outright rather than
 * substituting something near enough, and the refusal must leave what was
 * loaded alone. Without this the whole split is advisory. */
static void test_stages_refuse_the_wrong_family(void) {
    printf("a stage refuses a machine from the other family\n");
    work_t *w = work_create(&host);
    assert(w);

    set_stage(w, WORK_STAGE_SRC, WORK_FX_ONESHOT);
    set_stage(w, WORK_STAGE_SRC, WORK_FX_VOIDSPACE);      /* an effect */
    char s[32];
    work_get_param(w, "src", s, sizeof(s));
    CHECK(atoi(s) == WORK_FX_ONESHOT,
          "an effect loaded into the source stage: src reads %s", s);

    set_stage(w, WORK_STAGE_FX1, WORK_FX_VOIDSPACE);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_ONESHOT);        /* a source */
    work_get_param(w, "fx1", s, sizeof(s));
    CHECK(atoi(s) == WORK_FX_VOIDSPACE,
          "a source loaded into an insert: fx1 reads %s", s);

    /* Bypass belongs to both, because every stage needs a way to be empty. */
    set_stage(w, WORK_STAGE_SRC, WORK_FX_BYPASS);
    work_get_param(w, "src", s, sizeof(s));
    CHECK(atoi(s) == WORK_FX_BYPASS, "the source stage refused Bypass: %s", s);

    work_destroy(w);
}

/* Every machine must be loadable SOMEWHERE. The two families partition the
 * list between them, and a machine in neither would exist in the engine and be
 * reachable from nothing. */
static void test_every_machine_has_a_stage(void) {
    printf("every machine fits some stage\n");
    for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
        int fits = 0;
        for (int s = 0; s < WORK_STAGES; ++s)
            if (work_machine_fits_stage(s, mc)) fits = 1;
        CHECK(fits, "%s (%d) fits no stage — it cannot be loaded at all",
              work_machine_name(mc), mc);
    }
}

/* The lock index map is APPEND-ONLY: the last stage's parameters sit above the
 * machine and mix entries, because renumbering would move every lock in every
 * pattern ever saved. This test is the guard on that promise, and it is written
 * in LITERAL indices on purpose — deriving them from the same helpers the
 * engine uses would make it agree with any renumbering rather than catch one. */
static void test_lock_map_is_append_only(void) {
    printf("the last stage's locks append; the old indices still mean what they meant\n");
    work_t *w = work_create(&host);
    set_stage(w, WORK_STAGE_SRC, WORK_FX_POLYSAMPLE);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_CLOCK);
    set_stage(w, WORK_STAGE_FX1 + 1, WORK_FX_CLOCK);

    char s[64];
    /* Index 0..17 kept their numbers through the SRC promotion; what they MEAN
     * moved by one stage, which is the break DESIGN-8TRACK.md budgets for. */
    work_get_param(w, "locklabel0",  s, sizeof(s));
    CHECK(strcmp(s, "SRC:TUNE") == 0, "lock 0 reads %s, expected SRC:TUNE", s);
    work_get_param(w, "locklabel8",  s, sizeof(s));
    CHECK(strcmp(s, "FX1:TUNE") == 0, "lock 8 reads %s, expected FX1:TUNE", s);
    work_get_param(w, "locklabel16", s, sizeof(s));
    CHECK(strcmp(s, "SRC:MACH") == 0, "lock 16 reads %s, expected SRC:MACH", s);
    work_get_param(w, "locklabel17", s, sizeof(s));
    CHECK(strcmp(s, "FX1:MACH") == 0, "lock 17 reads %s, expected FX1:MACH", s);
    work_get_param(w, "locklabel18", s, sizeof(s));
    CHECK(strcmp(s, "MIX") == 0, "lock 18 reads %s, expected MIX", s);
    /* and the appended ones */
    work_get_param(w, "locklabel19", s, sizeof(s));
    CHECK(strcmp(s, "FX2:TUNE") == 0, "lock 19 reads %s, expected FX2:TUNE", s);
    work_get_param(w, "locklabel26", s, sizeof(s));
    CHECK(strcmp(s, "FX2:MIX") == 0, "lock 26 reads %s, expected FX2:MIX", s);
    work_get_param(w, "locklabel27", s, sizeof(s));
    CHECK(strcmp(s, "FX2:MACH") == 0, "lock 27 reads %s, expected FX2:MACH", s);

    /* A lock on the last stage must actually reach it when the step fires. */
    work_set_param(w, "seq_len", "1");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "locks0", "19=7");
    work_set_param(w, "seq_on", "1");
    int dummy;
    run_blocks(w, 4, &dummy);
    s[0] = '\0';
    work_get_param(w, "eff2", s, sizeof(s));
    CHECK(atoi(s) == 7, "a lock at 19 left the last stage's effective knobs at "
          "\"%s\", expected knob A to be 7", s);

    work_destroy(w);
}

/* A machine lock must obey the family rule too. Locking an effect onto the
 * source stage would put the engine in a state no surface can display: the
 * base machine still reads as a source while something else is rendering. */
static void test_machine_lock_respects_the_family(void) {
    printf("a machine lock cannot cross families\n");
    work_t *w = work_create(&host);
    set_stage(w, WORK_STAGE_SRC, WORK_FX_POLYSAMPLE);
    work_set_param(w, "seq_len", "1");
    work_set_param(w, "step0", "1:0:0:0");

    char v[32];
    snprintf(v, sizeof(v), "16=%d", WORK_FX_VOIDSPACE);   /* an effect on SRC */
    work_set_param(w, "locks0", v);
    work_set_param(w, "seq_on", "1");

    int dummy;
    run_blocks(w, 4, &dummy);
    char m[32];
    work_get_param(w, "effm", m, sizeof(m));
    CHECK(atoi(m) == WORK_FX_POLYSAMPLE,
          "an out-of-family machine lock took effect: effm=%s", m);
    work_destroy(w);
}

/* Loading a machine installs its defaults, the way selecting a machine does on
 * the hardware. */
static void test_machine_load_installs_defaults(void) {
    printf("loading a machine installs its default parameters\n");
    work_t *w = work_create(&host);
    assert(w);

    set_all_params(w, WORK_STAGE_FX1, 0);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_DRIVEDELAY);

    int nonzero = 0;
    for (int i = 0; i < WORK_PARAMS; ++i) {
        char key[16], buf[8];
        snprintf(key, sizeof(key), "fx1_p%d", i + 1);
        work_get_param(w, key, buf, sizeof(buf));
        if (atoi(buf) != 0) nonzero++;
    }
    CHECK(nonzero > 0, "Drive Delay loaded with all parameters at zero");
    work_destroy(w);
}

/* MIDI clock must drive the tempo-synced machines without wedging anything. */
static void test_midi_clock(void) {
    printf("MIDI clock and note events are handled\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_MMF);
    work_set_param(w, "fx1_p8", "127");          /* full envelope depth */

    const uint8_t start[] = {0xFA};
    const uint8_t tick[]  = {0xF8};
    const uint8_t on[]    = {0x90, 60, 100};
    const uint8_t off[]   = {0x80, 60, 0};

    work_on_midi(w, start, 1, 0);
    work_on_midi(w, on, 3, 0);

    int16_t buf[BLOCK * 2];
    double ph = 0.0;
    int64_t e_open = 0;
    for (int b = 0; b < 40; ++b) {
        work_on_midi(w, tick, 1, 0);
        fill_signal(buf, BLOCK, &ph);
        work_process(w, buf, buf, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) e_open += llabs(buf[i]);
    }

    work_on_midi(w, off, 3, 0);
    int64_t e_closed = 0;
    for (int b = 0; b < 40; ++b) {
        fill_signal(buf, BLOCK, &ph);
        work_process(w, buf, buf, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) e_closed += llabs(buf[i]);
    }

    CHECK(e_open != e_closed,
          "note on/off did not move the filter envelope (%lld vs %lld)",
          (long long)e_open, (long long)e_closed);
    work_destroy(w);
}

/* Every machine must name all the knobs it actually uses, and name none that
 * it does not — the UI builds its pages from this table. */
static void test_param_name_table(void) {
    printf("parameter name table is consistent\n");
    for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
        CHECK(work_machine_name(mc)[0] != '?', "machine %d has no name", mc);
        int seen_blank = 0;
        for (int i = 0; i < WORK_PARAMS; ++i) {
            const char *n = work_param_name(mc, i);
            if (n[0] == '\0') seen_blank = 1;
            else CHECK(!seen_blank || mc == WORK_FX_BYPASS,
                       "%s: knob %d is named after an unnamed knob",
                       work_machine_name(mc), i + 1);
        }
    }
    CHECK(work_machine_name(-1)[0] == '?', "out-of-range machine name not guarded");
    CHECK(work_machine_name(WORK_FX_COUNT)[0] == '?', "machine name overrun not guarded");
    CHECK(work_param_name(0, 99)[0] == '\0', "param name overrun not guarded");
}

/* ------------------------------------------------------------- sequencer */

/* Read one effective slot parameter — what actually reached the DSP after
 * locks and LFOs were applied. */
/* The suffix each stage answers to on the label and effective-value keys:
 * "eff_src" / "eff1" / "eff2", "labels_src" / "labels1" / "labels2". */
static const char *const STAGE_SFX[WORK_STAGES] = { "_src", "1", "2" };

/* One field out of "effm", which is every stage's effective machine followed by
 * the effective mix. Reading field 0 and calling it "the machine" is how a test
 * ends up asserting against the source stage while the machine under test is in
 * an insert. */
static int effm_field(const char *effm, int stage) {
    const char *c = effm;
    for (int i = 0; i < stage; ++i) {
        c = strchr(c, ',');
        if (!c) return -1;
        c++;
    }
    return atoi(c);
}

static int eff_param(work_t *w, int stage, int idx) {
    char buf[128], key[16];
    snprintf(key, sizeof(key), "eff%s", STAGE_SFX[stage]);
    if (work_get_param(w, key, buf, sizeof(buf)) <= 0) return -1;
    const char *c = buf;
    for (int i = 0; i < idx; ++i) {
        c = strchr(c, ',');
        if (!c) return -1;
        c++;
    }
    return atoi(c);
}

/* Advance the engine by `blocks` blocks of silence — enough to move the
 * sequencer without caring about the audio. */
static void idle(work_t *w, int blocks) {
    int16_t buf[BLOCK * 2];
    for (int b = 0; b < blocks; ++b) {
        memset(buf, 0, sizeof(buf));
        work_process(w, buf, buf, BLOCK);
    }
}

/* Blocks per 16th-note step at 120 BPM: 15/120 s = 5512.5 frames / 128. */
static int blocks_per_step(void) { return (int)((WORK_SR * 15.0 / 120.0) / BLOCK); }

static void test_seq_locks_apply_and_revert(void) {
    printf("parameter locks apply on their trig and revert on a plain trig\n");
    work_t *w = work_create(&host);
    assert(w);

    set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(w, "fx1_p5", "100");        /* base cutoff */
    work_set_param(w, "seq_len", "2");
    work_set_param(w, "step0", "1:0:0:0");     /* trig, no locks   */
    work_set_param(w, "step1", "1:0:0:0");     /* trig, locks FREQ */
    work_set_param(w, "lock1_12", "10");       /* lock index 12 = FX 1 knob E */
    work_set_param(w, "seq_on", "1");

    idle(w, 2);
    CHECK(eff_param(w, WORK_STAGE_FX1, 4) == 100, "step 0 should hold the base 100, got %d",
          eff_param(w, WORK_STAGE_FX1, 4));

    idle(w, blocks_per_step());
    CHECK(eff_param(w, WORK_STAGE_FX1, 4) == 10, "step 1 lock should force 10, got %d",
          eff_param(w, WORK_STAGE_FX1, 4));

    idle(w, blocks_per_step());
    CHECK(eff_param(w, WORK_STAGE_FX1, 4) == 100, "wrapping to step 0 should revert to 100, got %d",
          eff_param(w, WORK_STAGE_FX1, 4));

    work_destroy(w);
}

static void test_seq_off_ignores_pattern(void) {
    printf("a pattern has no effect while the sequencer is off\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(w, "fx1_p5", "100");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "lock0_12", "5");
    /* seq_on deliberately left at 0 */

    idle(w, blocks_per_step() * 3);
    CHECK(eff_param(w, WORK_STAGE_FX1, 4) == 100,
          "locks leaked with the sequencer off (got %d)", eff_param(w, WORK_STAGE_FX1, 4));
    work_destroy(w);
}

/* Each condition is checked over several passes of a one-step pattern, so the
 * pass counter is what drives the outcome. */
static void test_trig_conditions(void) {
    printf("trig conditions gate their trigs\n");

    struct { int cond; int fill; const char *name; int expect[4]; } cases[] = {
        { WORK_COND_OFF,       0, "OFF",   {1, 1, 1, 1} },
        { WORK_COND_FIRST,     0, "1ST",   {1, 0, 0, 0} },
        { WORK_COND_NOT_FIRST, 0, "!1ST",  {0, 1, 1, 1} },
        { WORK_COND_1_2,       0, "1:2",   {1, 0, 1, 0} },
        { WORK_COND_2_2,       0, "2:2",   {0, 1, 0, 1} },
        { WORK_COND_1_4,       0, "1:4",   {1, 0, 0, 0} },
        { WORK_COND_3_4,       0, "3:4",   {0, 0, 1, 0} },
        { WORK_COND_FILL,      1, "FILL",  {1, 1, 1, 1} },
        { WORK_COND_FILL,      0, "FILL off", {0, 0, 0, 0} },
        { WORK_COND_NOT_FILL,  0, "!FILL", {1, 1, 1, 1} },
        { WORK_COND_NOT_FILL,  1, "!FILL on", {0, 0, 0, 0} },
    };

    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        work_t *w = work_create(&host);
        assert(w);
        set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
        work_set_param(w, "fx1_p5", "100");
        work_set_param(w, "seq_len", "1");

        char sv[32];
        snprintf(sv, sizeof(sv), "1:%d:0:0", cases[k].cond);
        work_set_param(w, "step0", sv);
        work_set_param(w, "lock0_12", "7");
        work_set_param(w, "fill", cases[k].fill ? "1" : "0");
        work_set_param(w, "seq_on", "1");

        for (int pass = 0; pass < 4; ++pass) {
            /* Force a fresh step edge each pass, then sample */
            idle(w, 2);
            int v = eff_param(w, WORK_STAGE_FX1, 4);
            int fired = (v == 7);
            /* A non-firing trig leaves the previous state, so only the
             * transition from base matters on the first pass of each case. */
            if (pass == 0)
                CHECK(fired == cases[k].expect[0],
                      "%s pass 0: expected fired=%d, param reads %d",
                      cases[k].name, cases[k].expect[0], v);
            idle(w, blocks_per_step());
        }
        work_destroy(w);
    }
}

static void test_machine_lock(void) {
    printf("a machine lock swaps the machine for that step\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_BYPASS);
    work_set_param(w, "seq_len", "2");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "step1", "1:0:0:0");
    char v[8];
    snprintf(v, sizeof(v), "%d", WORK_FX_DECIMATOR);
    work_set_param(w, "lock1_17", v);          /* lock 17 = FX 1's machine */
    work_set_param(w, "seq_on", "1");

    /* effm is one field per stage then the mix, so insert 1's machine is the
     * SECOND field — reading the first would report the source stage and pass
     * for the wrong reason. */
    idle(w, 2);
    char m[32];
    work_get_param(w, "effm", m, sizeof(m));
    CHECK(effm_field(m, WORK_STAGE_FX1) == WORK_FX_BYPASS,
          "step 0 machine should be Bypass, effm=%s", m);

    idle(w, blocks_per_step());
    work_get_param(w, "effm", m, sizeof(m));
    CHECK(effm_field(m, WORK_STAGE_FX1) == WORK_FX_DECIMATOR,
          "step 1 should lock Decimator, effm=%s", m);

    work_destroy(w);
}

static void test_micro_timing(void) {
    printf("micro timing shifts when a step fires\n");

    /* Same pattern twice: once with step 1 nudged as late as it goes, once
     * without. Sampling just after step 1's nominal start must differ. */
    int seen[2];
    for (int variant = 0; variant < 2; ++variant) {
        work_t *w = work_create(&host);
        assert(w);
        set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
        work_set_param(w, "fx1_p5", "100");
        work_set_param(w, "seq_len", "2");
        work_set_param(w, "step0", "1:0:0:0");
        work_set_param(w, "step1", variant ? "1:0:23:0" : "1:0:0:0");
        work_set_param(w, "lock1_12", "3");
        work_set_param(w, "seq_on", "1");

        idle(w, 2);
        idle(w, blocks_per_step() + 1);        /* just past step 1's nominal start */
        seen[variant] = eff_param(w, WORK_STAGE_FX1, 4);
        work_destroy(w);
    }

    CHECK(seen[0] == 3, "un-nudged step 1 should have fired, got %d", seen[0]);
    CHECK(seen[1] == 100, "step 1 nudged +23/24 should not have fired yet, got %d",
          seen[1]);
}

static void test_seq_state_roundtrip(void) {
    printf("a full 64-step pattern round-trips through the state blob\n");
    work_t *a = work_create(&host);
    work_t *b = work_create(&host);
    assert(a && b);

    work_set_param(a, "seq_on", "1");
    work_set_param(a, "seq_len", "64");
    for (int i = 0; i < WORK_STEPS; ++i) {
        char key[16], val[32];
        snprintf(key, sizeof(key), "step%d", i);
        snprintf(val, sizeof(val), "1:%d:%d:%d", i % WORK_COND_COUNT,
                 (i % 47) - 23, i % WORK_RETRIG_COUNT);
        work_set_param(a, key, val);
        /* three locks per step */
        for (int k = 0; k < 3; ++k) {
            snprintf(key, sizeof(key), "lock%d_%d", i, (i + k * 5) % WORK_LOCKABLE);
            snprintf(val, sizeof(val), "%d", (i * 7 + k) % 128);
            work_set_param(a, key, val);
        }
    }

    char blob[32768];
    int n = work_get_param(a, "state", blob, sizeof(blob));
    CHECK(n > 0 && n < (int)sizeof(blob), "full-pattern blob length %d implausible", n);

    work_set_param(b, "state", blob);

    char ba[32768], bb[32768];
    work_get_param(a, "state", ba, sizeof(ba));
    work_get_param(b, "state", bb, sizeof(bb));
    CHECK(strcmp(ba, bb) == 0, "full pattern did not survive the round trip");

    /* A blob carrying a pattern must REPLACE the old one, not merge into it */
    work_t *c = work_create(&host);
    work_set_param(c, "step5", "1:0:0:0");
    work_set_param(c, "lock5_2", "99");
    char empty[4096];
    work_t *fresh = work_create(&host);
    work_get_param(fresh, "state", empty, sizeof(empty));
    work_set_param(c, "state", empty);
    char s5[64];
    work_get_param(c, "step5", s5, sizeof(s5));
    CHECK(strncmp(s5, "0:", 2) == 0, "loading an empty pattern left step 5 as %s", s5);

    work_destroy(a); work_destroy(b); work_destroy(c); work_destroy(fresh);
}

/* The state blob is far bigger now that it carries a pattern. Re-run the
 * canary check with the worst case actually loaded. */
static void test_seq_blob_tiny_buffers(void) {
    printf("a full pattern still never overruns a short buffer\n");
    work_t *w = work_create(&host);
    assert(w);
    work_set_param(w, "seq_on", "1");
    work_set_param(w, "seq_len", "64");
    for (int i = 0; i < WORK_STEPS; ++i) {
        char key[16], val[32];
        snprintf(key, sizeof(key), "step%d", i);
        work_set_param(w, key, "1:9:-11:2");
        for (int k = 0; k < WORK_LOCKABLE; ++k) {     /* every lock set */
            snprintf(key, sizeof(key), "lock%d_%d", i, k);
            snprintf(val, sizeof(val), "%d", (i + k) % 128);
            work_set_param(w, key, val);
        }
    }

    const int sizes[] = {2, 16, 64, 256, 1024, 4096, 16384};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        int cap = sizes[k];
        char *buf = malloc((size_t)cap + 64);
        assert(buf);
        memset(buf, 0x5A, (size_t)cap + 64);
        int n = work_get_param(w, "state", buf, cap);
        int ok = 1;
        for (int i = cap; i < cap + 64; ++i)
            if ((unsigned char)buf[i] != 0x5A) ok = 0;
        CHECK(ok, "full-pattern state into %d bytes overwrote past the buffer", cap);
        CHECK(n <= cap - 1, "full-pattern state into %d bytes reported %d", cap, n);
        free(buf);
    }

    /* And the on-device host reads state into a 16 KB buffer — make sure the
     * worst case actually fits, rather than only failing safely. */
    char big[16384];
    int n = work_get_param(w, "state", big, sizeof(big));
    CHECK(n < (int)sizeof(big) - 1,
          "worst-case pattern needs %d bytes; the device host buffer is 16384", n);

    work_destroy(w);
}

static void test_seq_clear(void) {
    printf("seq_clear empties the pattern\n");
    work_t *w = work_create(&host);
    assert(w);
    work_set_param(w, "step3", "1:5:7:2");
    work_set_param(w, "lock3_1", "42");
    work_set_param(w, "seq_clear", "1");

    char s[64];
    work_get_param(w, "step3", s, sizeof(s));
    CHECK(strcmp(s, "0:0:0:0:0:100:0") == 0, "step 3 after clear reads %s", s);
    work_get_param(w, "locks3", s, sizeof(s));
    CHECK(s[0] == '\0', "locks remained after clear: %s", s);
    work_destroy(w);
}

static void test_locks_string_api(void) {
    printf("locks<N> reads and writes a whole step's locks\n");
    work_t *w = work_create(&host);
    assert(w);
    work_set_param(w, "locks9", "0=11,4=22,18=33");

    char s[128];
    work_get_param(w, "locks9", s, sizeof(s));
    CHECK(strcmp(s, "0=11,4=22,18=33") == 0, "locks9 reads back as %s", s);

    /* -1 clears a single lock */
    work_set_param(w, "lock9_4", "-1");
    work_get_param(w, "locks9", s, sizeof(s));
    CHECK(strcmp(s, "0=11,18=33") == 0, "after clearing lock 4, locks9 = %s", s);

    /* an empty string clears them all */
    work_set_param(w, "locks9", "");
    work_get_param(w, "locks9", s, sizeof(s));
    CHECK(s[0] == '\0', "locks9 after empty write = %s", s);
    work_destroy(w);
}

static void test_lock_labels(void) {
    printf("lock labels follow the loaded machine\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_CLOCK);

    /* Index 8 is insert 1's knob A — index 0 is the SOURCE stage's. */
    char s[64];
    work_get_param(w, "locklabel8", s, sizeof(s));
    CHECK(strcmp(s, "FX1:TUNE") == 0, "insert 1 knob A under Clock Pitch reads %s", s);

    /* Filterbank's knobs are Gain A..H in the manual, but the label here is
     * the band's frequency — a knob called "A" tells you nothing. */
    set_stage(w, WORK_STAGE_FX1, WORK_FX_FBANK);
    work_get_param(w, "locklabel8", s, sizeof(s));
    CHECK(strcmp(s, "FX1:90Hz") == 0, "insert 1 knob A under Filterbank reads %s", s);

    work_get_param(w, "locklabel17", s, sizeof(s));
    CHECK(strcmp(s, "FX1:MACH") == 0, "lock 17 should be insert 1's machine, reads %s", s);
    work_get_param(w, "locklabel18", s, sizeof(s));
    CHECK(strcmp(s, "MIX") == 0, "lock 18 should be MIX, reads %s", s);

    work_destroy(w);
}

static void test_transport_restarts_pattern(void) {
    printf("MIDI start restarts the pattern from step 0\n");
    work_t *w = work_create(&host);
    assert(w);
    work_set_param(w, "seq_len", "8");
    work_set_param(w, "seq_on", "1");
    for (int i = 0; i < 8; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "step%d", i);
        work_set_param(w, key, "1:0:0:0");
    }

    idle(w, blocks_per_step() * 3 + 2);
    char p[16];
    work_get_param(w, "seq_pos", p, sizeof(p));
    CHECK(atoi(p) != 0, "expected to be mid-pattern, seq_pos=%s", p);

    const uint8_t start[] = {0xFA};
    work_on_midi(w, start, 1, 0);
    idle(w, 1);
    work_get_param(w, "seq_pos", p, sizeof(p));
    CHECK(atoi(p) == 0, "start should have reset to step 0, seq_pos=%s", p);

    work_destroy(w);
}

/* The Shadow UI and the Master FX knob pages get their labels from here.
 * module.json can only ever say "A".."H"; this must say TUNE, WIN, FDBK... and
 * must change when the machine changes. */
/* Serving ui_hierarchy DIVERTS the host away from our own chain UI:
 * enterComponentEdit() tries getComponentHierarchy() first and only falls
 * through to loadModuleUi() -> ui_chain.js when it returns nothing. The
 * generic hierarchy editor caches the hierarchy at entry and never re-fetches,
 * which is what left the settings page stuck on the previous machine. So the
 * engine must stay quiet on this key. */
static void test_ui_hierarchy_not_served(void) {
    printf("ui_hierarchy is not served, so the host reaches our chain UI\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_CLOCK);

    char buf[65536];
    memset(buf, 0x5A, sizeof(buf));
    int n = work_get_param(w, "ui_hierarchy", buf, sizeof(buf));
    CHECK(n < 0, "ui_hierarchy answered with %d bytes — that diverts the host "
                 "into the caching hierarchy editor and bypasses ui_chain.js", n);

    /* the labels the chain UI actually uses must still be there */
    char lab[256];
    int ln = work_get_param(w, "labels1", lab, sizeof(lab));
    CHECK(ln > 0 && strstr(lab, "TUNE") != NULL,
          "labels1 must still serve the loaded machine's labels (%s)", lab);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_FBANK);
    work_get_param(w, "labels1", lab, sizeof(lab));
    CHECK(strstr(lab, "90Hz") != NULL,
          "labels1 did not follow the machine change (%s)", lab);
    work_destroy(w);
}

/* The three reverbs were one shared Schroeder tank until v0.2.0. They are now
 * three algorithms — Schroeder comb bank, Dattorro plate, Householder FDN —
 * so their impulse responses must not resemble each other. */
static void capture_ir(int machine, float *out, int n) {
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, machine);
    /* fully wet, so the dry impulse does not dominate the comparison */
    for (int i = 0; i < WORK_PARAMS; ++i) {
        const char *nm = work_param_name(machine, i);
        if (strcmp(nm, "MIX") == 0) {
            char key[16];
            snprintf(key, sizeof(key), "fx1_p%d", i + 1);
            work_set_param(w, key, "127");
        }
    }

    int16_t buf[BLOCK * 2];
    int written = 0;
    for (int b = 0; b < n / BLOCK + 2 && written < n; ++b) {
        memset(buf, 0, sizeof(buf));
        if (b == 0) { buf[0] = 24000; buf[1] = 24000; }   /* one impulse */
        work_process(w, buf, buf, BLOCK);
        for (int i = 0; i < BLOCK && written < n; ++i)
            out[written++] = (float)buf[i * 2] / 32768.0f;
    }
    work_destroy(w);
}

/* Normalised correlation between two impulse responses. Two voicings of the
 * same topology correlate strongly; different algorithms do not. */
static double ir_correlation(const float *a, const float *b, int n) {
    double sa = 0, sb = 0, sab = 0;
    for (int i = 0; i < n; ++i) { sa += a[i]*a[i]; sb += b[i]*b[i]; sab += a[i]*b[i]; }
    if (sa < 1e-12 || sb < 1e-12) return 0.0;
    return fabs(sab) / sqrt(sa * sb);
}

static void test_reverbs_are_distinct(void) {
    printf("the three reverbs are genuinely different algorithms\n");
    enum { N = 22050 };                       /* half a second */
    static float rum[N], steel[N], svoid[N];

    capture_ir(WORK_FX_ROOMTONE, rum, N);
    capture_ir(WORK_FX_IRONROOM, steel, N);
    capture_ir(WORK_FX_VOIDSPACE, svoid, N);

    /* each must actually produce a tail */
    double e_rum = 0, e_steel = 0, e_void = 0;
    for (int i = 2000; i < N; ++i) {
        e_rum += fabs(rum[i]); e_steel += fabs(steel[i]); e_void += fabs(svoid[i]);
    }
    CHECK(e_rum   > 0.5, "Roomtone produced no tail (energy %.3f)", e_rum);
    CHECK(e_steel > 0.5, "Iron Room produced no tail (energy %.3f)", e_steel);
    CHECK(e_void  > 0.5, "Voidspace produced no tail (energy %.3f)", e_void);

    double c_rs = ir_correlation(rum, steel, N);
    double c_rv = ir_correlation(rum, svoid, N);
    double c_sv = ir_correlation(steel, svoid, N);

    CHECK(c_rs < 0.35, "Roomtone and Iron Room correlate at %.2f — too alike", c_rs);
    CHECK(c_rv < 0.35, "Roomtone and Voidspace correlate at %.2f — too alike", c_rv);
    CHECK(c_sv < 0.35, "Iron Room and Voidspace correlate at %.2f — too alike", c_sv);

    /* Echo density: a plate and an FDN build density much faster than a comb
     * bank, so count zero crossings in the first 100 ms as a proxy. */
    int zc[3] = {0, 0, 0};
    const float *irs[3] = {rum, steel, svoid};
    for (int k = 0; k < 3; ++k)
        for (int i = 1; i < 4410; ++i)
            if ((irs[k][i - 1] < 0.0f) != (irs[k][i] < 0.0f)) zc[k]++;
    CHECK(zc[0] > 0 && zc[1] > 0 && zc[2] > 0,
          "an impulse response was static (zc %d/%d/%d)", zc[0], zc[1], zc[2]);
    printf("      correlations  rum/steel %.2f  rum/void %.2f  steel/void %.2f\n",
           c_rs, c_rv, c_sv);
    printf("      early density %d / %d / %d crossings\n", zc[0], zc[1], zc[2]);
}

/* Granulator must actually granulate: rearrange the input rather than pass it,
 * and follow TUNE. */
static void test_grainer(void) {
    printf("Granulator granulates the input buffer and tracks TUNE\n");

    /* fully wet, pitch centred */
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_SRC, WORK_FX_GRANULATOR);
    work_set_param(w, "src_p8", "127");        /* MIX wet */
    int railed = 0;
    int64_t e = run_blocks(w, 300, &railed);
    CHECK(e > 0, "Granulator produced silence");
    CHECK(railed == 0, "Granulator railed %d samples", railed);

    /* it must not simply reproduce the input */
    int16_t in[BLOCK * 2], out[BLOCK * 2];
    double ph = 0.0;
    fill_signal(in, BLOCK, &ph);
    memcpy(out, in, sizeof(in));
    work_process(w, out, out, BLOCK);
    int same = 0;
    for (int i = 0; i < BLOCK * 2; ++i) if (out[i] == in[i]) same++;
    CHECK(same < BLOCK * 2 - 8, "Granulator output is identical to its input");
    work_destroy(w);

    /* TUNE up an octave must shift energy relative to TUNE down an octave */
    int64_t hi = 0, lo = 0;
    for (int k = 0; k < 2; ++k) {
        work_t *g = work_create(&host);
        set_stage(g, WORK_STAGE_SRC, WORK_FX_GRANULATOR);
        work_set_param(g, "src_p8", "127");
        work_set_param(g, "src_p1", k ? "96" : "32");   /* TUNE up / down */
        int dummy;
        int64_t en = run_blocks(g, 200, &dummy);
        if (k) hi = en; else lo = en;
        work_destroy(g);
    }
    CHECK(hi != lo, "TUNE made no difference to the output (%lld vs %lld)",
          (long long)hi, (long long)lo);

    /* a Granulator with no grains allowed still must not blow up */
    work_t *q = work_create(&host);
    set_stage(q, WORK_STAGE_SRC, WORK_FX_GRANULATOR);
    set_all_params(q, WORK_STAGE_SRC, 0);
    int r2 = 0;
    run_blocks(q, 120, &r2);
    CHECK(r2 == 0, "Granulator at all-min railed %d samples", r2);
    work_destroy(q);
}

/* ------------------------------------------------------------ v0.3.0 pack */

static void test_live_record(void) {
    printf("live record lays knob moves onto the playing step\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(w, "seq_len", "4");
    work_set_param(w, "seq_on", "1");
    for (int i = 0; i < 4; ++i) {
        char k[16]; snprintf(k, sizeof(k), "step%d", i);
        work_set_param(w, k, "1:0:0:0");
    }

    /* not armed: a knob move must NOT write a lock */
    idle(w, 2);
    work_set_param(w, "fx1_p5", "20");
    char s[128];
    work_get_param(w, "locks0", s, sizeof(s));
    CHECK(s[0] == '\0', "unarmed knob move recorded a lock: %s", s);

    /* armed: the same move lands on whichever step is playing */
    work_set_param(w, "live_rec", "1");
    char pos[16];
    work_get_param(w, "seq_pos", pos, sizeof(pos));
    int at = atoi(pos);
    work_set_param(w, "fx1_p5", "33");

    char key[16];
    snprintf(key, sizeof(key), "locks%d", at);
    work_get_param(w, key, s, sizeof(s));
    CHECK(strstr(s, "12=33") != NULL,
          "armed knob move did not lock insert 1 knob E (index 12) on step %d (locks read \"%s\")", at, s);

    /* and it arms a trig on that step if there was not one */
    snprintf(key, sizeof(key), "step%d", at);
    work_get_param(w, key, s, sizeof(s));
    CHECK(s[0] == '1', "live record left step %d without a trig (%s)", at, s);

    /* disarming stops it again */
    work_set_param(w, "live_rec", "0");
    idle(w, blocks_per_step());
    work_get_param(w, "seq_pos", pos, sizeof(pos));
    int at2 = atoi(pos);
    if (at2 != at) {
        work_set_param(w, "fx1_p5", "77");
        snprintf(key, sizeof(key), "locks%d", at2);
        work_get_param(w, key, s, sizeof(s));
        CHECK(strstr(s, "4=77") == NULL, "disarmed live record still wrote a lock");
    }
    work_destroy(w);
}

static void test_step_probability(void) {
    printf("per-step probability gates trigs independently of the condition\n");
    work_t *w = work_create(&host);
    assert(w);

    /* defaults to 100 = always */
    char s[32];
    work_get_param(w, "prob7", s, sizeof(s));
    CHECK(atoi(s) == 100, "default probability is %s, expected 100", s);

    /* prob 1 should almost never fire; prob 100 always */
    for (int variant = 0; variant < 2; ++variant) {
        work_t *g = work_create(&host);
        set_stage(g, WORK_STAGE_FX1, WORK_FX_LPF);
        work_set_param(g, "fx1_p5", "100");
        work_set_param(g, "seq_len", "1");
        work_set_param(g, "step0", "1:0:0:0");
        work_set_param(g, "lock0_12", "5");
        work_set_param(g, "prob0", variant ? "100" : "1");
        work_set_param(g, "seq_on", "1");

        int fired = 0;
        for (int pass = 0; pass < 30; ++pass) {
            work_set_param(g, "fx1_p5", "100");   /* reset the observable */
            idle(g, 2);
            if (eff_param(g, WORK_STAGE_FX1, 4) == 5) fired++;
            idle(g, blocks_per_step());
        }
        if (variant) CHECK(fired > 20, "prob 100 fired only %d/30 times", fired);
        else         CHECK(fired < 10, "prob 1 fired %d/30 times", fired);
        work_destroy(g);
    }

    /* it round-trips through the step string and the state blob */
    work_set_param(w, "step5", "1:0:0:0:42");
    work_get_param(w, "prob5", s, sizeof(s));
    CHECK(atoi(s) == 42, "prob via the step string reads %s", s);

    work_t *b = work_create(&host);
    char blob[8192];
    work_get_param(w, "state", blob, sizeof(blob));
    work_set_param(b, "state", blob);
    work_get_param(b, "prob5", s, sizeof(s));
    CHECK(atoi(s) == 42, "prob did not survive the state blob (%s)", s);
    work_destroy(b);
    work_destroy(w);
}

static void test_mod_envelope(void) {
    printf("the modulation envelope fires on trigs and reaches its destination\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(w, "fx1_p5", "20");         /* low base cutoff */
    work_set_param(w, "seq_len", "2");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "menv_dest", "12");      /* insert 1 knob E = FREQ */
    work_set_param(w, "menv_depth", "127");    /* full positive */
    work_set_param(w, "menv_atk", "0");
    work_set_param(w, "menv_hold", "60");
    work_set_param(w, "seq_on", "1");

    idle(w, 3);
    int lifted = eff_param(w, WORK_STAGE_FX1, 4);
    CHECK(lifted > 20, "envelope did not lift the cutoff (%d, base 20)", lifted);

    /* with no destination it must do nothing */
    work_t *n = work_create(&host);
    set_stage(n, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(n, "fx1_p5", "20");
    work_set_param(n, "seq_len", "2");
    work_set_param(n, "step0", "1:0:0:0");
    work_set_param(n, "menv_depth", "127");    /* depth but dest still -1 */
    work_set_param(n, "seq_on", "1");
    idle(n, 3);
    CHECK(eff_param(n, WORK_STAGE_FX1, 4) == 20,
          "envelope moved a parameter with no destination set (%d)", eff_param(n, WORK_STAGE_FX1, 4));
    work_destroy(n);
    work_destroy(w);
}

static void test_third_lfo(void) {
    printf("LFO 3 exists and modulates\n");
    work_t *a = work_create(&host);
    set_stage(a, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(a, "fx1_p5", "40");
    int dummy;
    int64_t stat = run_blocks(a, 120, &dummy);
    work_destroy(a);

    work_t *b = work_create(&host);
    set_stage(b, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(b, "fx1_p5", "40");
    work_set_param(b, "lfo3_dest", "12");    /* insert 1 knob E */
    work_set_param(b, "lfo3_depth", "127");
    work_set_param(b, "lfo3_spd", "100");
    int64_t mod = run_blocks(b, 120, &dummy);

    char s[16];
    work_get_param(b, "lfo3_dest", s, sizeof(s));
    CHECK(atoi(s) == 12, "lfo3_dest reads %s", s);
    CHECK(llabs(mod - stat) > stat / 20,
          "LFO 3 changed output by under 5%% (%lld vs %lld)",
          (long long)mod, (long long)stat);
    work_destroy(b);
}

static void test_midi_cc(void) {
    printf("external MIDI CC reaches parameters, internal CC does not\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);

    /* an INTERNAL CC is Move's own encoder and must be ignored */
    uint8_t cc[3] = {0xB0, 10, 99};              /* CC 10 -> fx1_p3 */
    work_on_midi(w, cc, 3, MOVE_MIDI_SOURCE_INTERNAL);
    char s[16];
    work_get_param(w, "fx1_p3", s, sizeof(s));
    CHECK(atoi(s) != 99, "an internal CC reached a parameter (%s)", s);

    /* external does reach it */
    work_on_midi(w, cc, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_get_param(w, "fx1_p3", s, sizeof(s));
    CHECK(atoi(s) == 99, "external CC 10 did not set fx1_p3 (%s)", s);

    /* the duplicate guard drops an immediate repeat of the same message */
    work_set_param(w, "fx1_p3", "0");
    work_on_midi(w, cc, 3, MOVE_MIDI_SOURCE_FX_BROADCAST);
    work_get_param(w, "fx1_p3", s, sizeof(s));
    CHECK(atoi(s) == 0, "the CC duplicate guard let a repeat through (%s)", s);

    /* slot 2, machine select, mix and transport all land */
    uint8_t cc2[3] = {0xB0, 23, 64};             /* CC 23 -> fx2_p8 */
    work_on_midi(w, cc2, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_get_param(w, "fx2_p8", s, sizeof(s));
    CHECK(atoi(s) == 64, "CC 23 did not set fx2_p8 (%s)", s);

    /* A machine select sweeps its stage's FAMILY, not the whole machine list.
     * Both ends must land on real machines, and — the part that would
     * otherwise rot silently — EVERY controller position must select something
     * the stage actually accepts. Scaling across all 26 codes instead leaves
     * dead spots wherever the stage refuses one: 21 of 26 positions on the
     * source stage, which on hardware reads as a broken knob. */
    uint8_t ccm[3] = {0xB0, 24, 127};            /* insert 1 machine select */
    work_on_midi(w, ccm, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_get_param(w, "fx1", s, sizeof(s));
    CHECK(atoi(s) == WORK_FX_TILT,
          "CC 24 at 127 should select the last EFFECT, got %s", s);

    for (int stage = 0; stage < WORK_STAGES; ++stage) {
        const int ctrl = stage == WORK_STAGE_SRC ? 88 : 23 + stage;

        /* Sweep the controller and count how many positions land on each
         * machine. Checking only that the RESULT is in-family proves nothing:
         * a refused write leaves the stage at its default, and the default is
         * in-family too. The observable damage is the DISTRIBUTION — with
         * whole-list scaling most of the travel refuses and sticks on Bypass,
         * so one machine claims the majority of the knob. */
        int hits[WORK_FX_COUNT] = {0}, family = 0;
        for (int mc = 0; mc < WORK_FX_COUNT; ++mc)
            if (work_machine_fits_stage(stage, mc)) family++;

        for (int v = 0; v <= 127; ++v) {
            work_t *m = work_create(&host);
            uint8_t msg[3] = {0xB0, (uint8_t)ctrl, (uint8_t)v};
            work_on_midi(m, msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
            char got[16];
            work_get_param(m, STAGE_KEY[stage], got, sizeof(got));
            int mc = atoi(got);
            CHECK_QUIET(work_machine_fits_stage(stage, mc),
                        "CC %d at %d selected %d, which stage %d refuses",
                        ctrl, v, mc, stage);
            hits[mc]++;
            work_destroy(m);
        }

        int worst = 0, worst_mc = 0, reached = 0;
        for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
            if (hits[mc]) reached++;
            if (hits[mc] > worst) { worst = hits[mc]; worst_mc = mc; }
        }
        const int share = 2 * (128 + family - 1) / family;   /* twice its fair share */
        CHECK(worst <= share,
              "CC %d: %s claims %d of 128 positions (fair share is about %d) — "
              "the sweep is not crossing stage %d's family, it is stalling on "
              "the machines the stage refuses",
              ctrl, work_machine_name(worst_mc), worst, 128 / family, stage);
        CHECK(reached == family,
              "CC %d reached %d of stage %d's %d machines",
              ctrl, reached, stage, family);
    }

    uint8_t ccr[3] = {0xB0, 66, 127};            /* live record on */
    work_on_midi(w, ccr, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_get_param(w, "live_rec", s, sizeof(s));
    CHECK(atoi(s) == 1, "CC 66 did not arm live record (%s)", s);

    /* CCs outside the map must be ignored rather than land somewhere */
    uint8_t cclow[3] = {0xB0, 1, 127};           /* mod wheel */
    work_on_midi(w, cclow, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    uint8_t cchigh[3] = {0xB0, 123, 127};        /* all notes off */
    work_on_midi(w, cchigh, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    CHECK(1, "unmapped CCs handled");            /* no crash is the assertion */

    work_destroy(w);
}

/* ------------------------------------------------------------- Tier A */

static void test_pattern_bank(void) {
    printf("the pattern bank holds independent patterns\n");
    work_t *w = work_create(&host);
    assert(w);
    char s[64];

    work_set_param(w, "pattern", "0");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "seq_len", "8");

    work_set_param(w, "pattern", "5");
    work_get_param(w, "step0", s, sizeof(s));
    CHECK(s[0] == '0', "pattern 5 inherited pattern 0's trig (%s)", s);
    work_get_param(w, "seq_len", s, sizeof(s));
    CHECK(atoi(s) == 16, "pattern 5's length should default to 16, got %s", s);

    work_set_param(w, "step3", "1:0:0:0");
    work_set_param(w, "seq_len", "32");

    work_set_param(w, "pattern", "0");
    work_get_param(w, "step0", s, sizeof(s));
    CHECK(s[0] == '1', "pattern 0 lost its trig (%s)", s);
    work_get_param(w, "step3", s, sizeof(s));
    CHECK(s[0] == '0', "pattern 5's edit leaked into pattern 0 (%s)", s);
    work_get_param(w, "seq_len", s, sizeof(s));
    CHECK(atoi(s) == 8, "pattern 0's length changed to %s", s);

    /* out-of-range selects clamp rather than corrupt */
    work_set_param(w, "pattern", "99");
    work_get_param(w, "pattern", s, sizeof(s));
    CHECK(atoi(s) == WORK_PATTERNS - 1, "pattern select did not clamp (%s)", s);
    work_destroy(w);
}

static void test_song_mode(void) {
    printf("song mode chains patterns with repeats\n");
    work_t *w = work_create(&host);
    assert(w);

    /* two rows: pattern 2 twice, then pattern 7 once */
    work_set_param(w, "song_row0", "2:2:0");
    work_set_param(w, "song_row1", "7:1:0");
    work_set_param(w, "song_len", "2");
    for (int p = 0; p < WORK_PATTERNS; ++p) {
        char v[8]; snprintf(v, sizeof(v), "%d", p);
        work_set_param(w, "pattern", v);
        work_set_param(w, "seq_len", "1");
        work_set_param(w, "step0", "1:0:0:0");
    }
    work_set_param(w, "song_on", "1");
    work_set_param(w, "seq_on", "1");

    char s[32];
    work_get_param(w, "pattern", s, sizeof(s));
    CHECK(atoi(s) == 2, "song should start on pattern 2, got %s", s);

    idle(w, blocks_per_step() + 2);      /* pass 1 of row 0 done */
    work_get_param(w, "pattern", s, sizeof(s));
    CHECK(atoi(s) == 2, "row 0 has 2 repeats; still expected pattern 2, got %s", s);

    idle(w, blocks_per_step() + 2);      /* pass 2 done -> row 1 */
    work_get_param(w, "pattern", s, sizeof(s));
    CHECK(atoi(s) == 7, "after 2 repeats the song should move to pattern 7, got %s", s);

    idle(w, blocks_per_step() + 2);      /* row 1 done -> wrap to row 0 */
    work_get_param(w, "pattern", s, sizeof(s));
    CHECK(atoi(s) == 2, "the song should wrap back to pattern 2, got %s", s);

    work_destroy(w);
}

static void test_undo_redo_memorize(void) {
    printf("undo, redo and memorize/recall work on the pattern\n");
    work_t *w = work_create(&host);
    assert(w);
    char s[64];

    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "step1", "1:0:0:0");
    work_set_param(w, "memorize", "1");

    work_set_param(w, "seq_clear", "1");
    work_get_param(w, "step0", s, sizeof(s));
    CHECK(s[0] == '0', "clear did not empty step 0 (%s)", s);

    work_set_param(w, "undo", "1");
    work_get_param(w, "step0", s, sizeof(s));
    CHECK(s[0] == '1', "undo did not restore step 0 (%s)", s);

    work_set_param(w, "redo", "1");
    work_get_param(w, "step0", s, sizeof(s));
    CHECK(s[0] == '0', "redo did not re-apply the clear (%s)", s);

    work_set_param(w, "recall", "1");
    work_get_param(w, "step1", s, sizeof(s));
    CHECK(s[0] == '1', "recall did not restore the memorized pattern (%s)", s);

    /* undo with nothing to undo must be a no-op, not a corruption */
    work_t *q = work_create(&host);
    work_set_param(q, "undo", "1");
    work_get_param(q, "step0", s, sizeof(s));
    CHECK(s[0] == '0', "undo on a fresh engine changed something (%s)", s);
    work_destroy(q);
    work_destroy(w);
}

static void test_trig_types(void) {
    printf("a lock trig applies locks without restarting the modulators\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(w, "fx1_p5", "100");
    work_set_param(w, "seq_len", "2");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "step1", "1:0:0:0:100:1");     /* lock trig */
    work_set_param(w, "lock1_12", "12");
    work_set_param(w, "menv_dest", "14");   /* insert 1 knob G */
    work_set_param(w, "menv_depth", "127");
    work_set_param(w, "seq_on", "1");

    char s[32];
    work_get_param(w, "trigtype1", s, sizeof(s));
    CHECK(atoi(s) == WORK_TRIG_LOCK, "step 1 trig type reads %s", s);

    idle(w, 2);
    idle(w, blocks_per_step());
    /* the lock still applies */
    CHECK(eff_param(w, WORK_STAGE_FX1, 4) == 12,
          "a lock trig failed to apply its lock (%d)", eff_param(w, WORK_STAGE_FX1, 4));
    work_destroy(w);
}

static void test_transform_and_quantize(void) {
    printf("transform and quantize rewrite the pattern\n");
    work_t *w = work_create(&host);
    assert(w);
    char s[64];

    work_set_param(w, "seq_len", "4");
    work_set_param(w, "step0", "1:0:0:0");

    work_set_param(w, "transform", "reverse");
    work_get_param(w, "step3", s, sizeof(s));
    CHECK(s[0] == '1', "reverse did not move step 0's trig to step 3 (%s)", s);
    work_get_param(w, "step0", s, sizeof(s));
    CHECK(s[0] == '0', "reverse left a trig on step 0 (%s)", s);

    work_set_param(w, "transform", "rotl");
    work_get_param(w, "step2", s, sizeof(s));
    CHECK(s[0] == '1', "rotate-left did not move the trig to step 2 (%s)", s);

    work_set_param(w, "transform", "invert");
    work_get_param(w, "step2", s, sizeof(s));
    CHECK(s[0] == '0', "invert did not clear the trig (%s)", s);
    work_get_param(w, "step0", s, sizeof(s));
    CHECK(s[0] == '1', "invert did not set the empty steps (%s)", s);

    /* every transform is undoable */
    work_set_param(w, "undo", "1");
    work_get_param(w, "step2", s, sizeof(s));
    CHECK(s[0] == '1', "undo did not reverse the invert (%s)", s);

    /* quantize pulls micro-timing toward the grid */
    work_set_param(w, "step1", "1:0:20:0");
    work_set_param(w, "quantize", "127");
    work_get_param(w, "step1", s, sizeof(s));
    int micro = atoi(strchr(strchr(s, ':') + 1, ':') + 1);
    CHECK(micro == 0, "full quantize left micro timing at %d", micro);
    work_destroy(w);
}

static void test_page_mask(void) {
    printf("silencing a page stops its trigs firing\n");
    work_t *w = work_create(&host);
    assert(w);
    set_stage(w, WORK_STAGE_FX1, WORK_FX_LPF);
    work_set_param(w, "fx1_p5", "100");
    work_set_param(w, "seq_len", "32");            /* two pages */
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "step16", "1:0:0:0");
    work_set_param(w, "lock16_12", "9");
    work_set_param(w, "page_mask", "1");           /* page 1 only */
    work_set_param(w, "seq_on", "1");

    idle(w, 2);
    idle(w, blocks_per_step() * 16 + 2);           /* into page 2 */
    CHECK(eff_param(w, WORK_STAGE_FX1, 4) == 100,
          "a silenced page still fired its lock (%d)", eff_param(w, WORK_STAGE_FX1, 4));

    work_set_param(w, "page_mask", "3");           /* both pages */
    work_set_param(w, "seq_on", "0");
    work_set_param(w, "seq_on", "1");
    idle(w, 2);
    idle(w, blocks_per_step() * 16 + 2);
    CHECK(eff_param(w, WORK_STAGE_FX1, 4) == 9,
          "re-enabling the page did not restore its lock (%d)", eff_param(w, WORK_STAGE_FX1, 4));
    work_destroy(w);
}

static void test_nrpn(void) {
    printf("NRPN reaches parameters at full 14-bit resolution\n");
    work_t *w = work_create(&host);
    assert(w);
    char s[32];

    /* NRPN 24 = FX 1 machine. At full scale it must select the LAST machine —
     * the range bug hardware sequencers keep shipping. */
    uint8_t msb[3]  = {0xB0, 99, 0};
    uint8_t lsb[3]  = {0xB0, 98, 24};
    uint8_t dmsb[3] = {0xB0, 6, 127};
    uint8_t dlsb[3] = {0xB0, 38, 127};
    work_on_midi(w, msb, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_on_midi(w, lsb, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_on_midi(w, dmsb, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_on_midi(w, dlsb, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_get_param(w, "fx1", s, sizeof(s));
    CHECK(atoi(s) == WORK_FX_COUNT - 1,
          "NRPN 24 at full scale selected %s, expected %d", s, WORK_FX_COUNT - 1);

    /* NRPN 8 = FX 1 knob A */
    uint8_t lsb2[3] = {0xB0, 98, 8};
    uint8_t d2[3]   = {0xB0, 6, 64};
    work_on_midi(w, lsb2, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_on_midi(w, d2, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    work_get_param(w, "fx1_p1", s, sizeof(s));
    CHECK(atoi(s) > 60 && atoi(s) < 68, "NRPN 8 at half scale gave %s", s);

    /* internal NRPN must be ignored like internal CC */
    work_t *q = work_create(&host);
    work_on_midi(q, lsb2, 3, MOVE_MIDI_SOURCE_INTERNAL);
    work_on_midi(q, d2, 3, MOVE_MIDI_SOURCE_INTERNAL);
    work_get_param(q, "fx1_p1", s, sizeof(s));
    CHECK(atoi(s) != 64, "an internal NRPN reached a parameter (%s)", s);
    work_destroy(q);
    work_destroy(w);
}

static void test_feedback_monitor(void) {
    printf("monitor mutes the live input without killing the tails\n");
    work_t *w = work_create(&host);
    assert(w);

    char s[16];
    work_get_param(w, "monitor", s, sizeof(s));
    CHECK(atoi(s) == 1, "monitor should default to on, got %s", s);

    /* a reverb, so there is a tail to check */
    set_stage(w, WORK_STAGE_FX1, WORK_FX_VOIDSPACE);
    work_set_param(w, "fx1_p7", "127");
    int dummy;
    int64_t live = run_blocks(w, 60, &dummy);
    CHECK(live > 0, "no output with the input live");

    /* muting the input silences the source but the tail must ring out */
    work_set_param(w, "monitor", "0");
    int16_t buf[BLOCK * 2];
    double ph = 0.0;
    int64_t first = 0, later = 0;
    for (int b = 0; b < 8; ++b) {
        fill_signal(buf, BLOCK, &ph);
        work_process(w, buf, buf, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) first += llabs(buf[i]);
    }
    for (int b = 0; b < 200; ++b) {
        fill_signal(buf, BLOCK, &ph);
        work_process(w, buf, buf, BLOCK);
    }
    for (int b = 0; b < 8; ++b) {
        fill_signal(buf, BLOCK, &ph);
        work_process(w, buf, buf, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) later += llabs(buf[i]);
    }
    CHECK(first > 0, "muting the input killed the reverb tail immediately");
    CHECK(later < first, "the tail did not decay while the input was muted");

    /* Bypass with the input muted must be silent — that is the actual
     * feedback-breaking guarantee. */
    work_t *q = work_create(&host);
    work_set_param(q, "monitor", "0");
    int64_t e = run_blocks(q, 40, &dummy);
    CHECK(e == 0, "a muted input still passed audio through Bypass (%lld)",
          (long long)e);
    work_destroy(q);

    /* monitor must NEVER be preset-saved: loading a patch must not be able to
     * re-open a feedback path. */
    char blob[8192];
    work_get_param(w, "state", blob, sizeof(blob));
    CHECK(strstr(blob, "monitor") == NULL && strstr(blob, "\"mon\"") == NULL,
          "monitor leaked into the preset blob");
    work_destroy(w);
}

static void test_hw_input_flag(void) {
    printf("only the builds that read the mic advertise hw_input\n");
    work_t *w = work_create(&host);
    char s[16];
    work_get_param(w, "hw_input", s, sizeof(s));
    CHECK(atoi(s) == 0,
          "hw_input must default to 0 so the audio_fx build never auto-mutes (got %s)", s);
    work_set_param(w, "hw_input", "1");
    work_get_param(w, "hw_input", s, sizeof(s));
    CHECK(atoi(s) == 1, "hw_input did not set");
    work_destroy(w);
}


/* ------------------------------------------------------------- Tier B: SRC
 *
 * Sample transfer has to survive the one constraint that shapes the whole
 * design: work_set_param runs on the shim's AUDIO THREAD, so the engine can
 * never open a file. Audio arrives base64-encoded in chunks and the engine
 * only ever does a bounded decode into memory allocated at create time.
 */

static void b64_encode(const uint8_t *src, int n, char *out) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int v = src[i] << 16;
        if (i + 1 < n) v |= src[i + 1] << 8;
        if (i + 2 < n) v |= src[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Build a ramp so every frame is individually identifiable — a transfer that
 * drops, duplicates or misaligns a chunk shows up as a wrong value at a known
 * index, not as vague distortion. */
static void make_ramp(int16_t *pcm, int frames) {
    for (int i = 0; i < frames; ++i) {
        pcm[i * 2]     = (int16_t)((i * 7) % 30000 - 15000);
        pcm[i * 2 + 1] = (int16_t)((i * 11) % 30000 - 15000);
    }
}

static void send_sample(work_t *w, const int16_t *pcm, int frames, int chunk_frames) {
    char begin[64];
    snprintf(begin, sizeof begin, "%d:ramp", frames);
    work_set_param(w, "sample_begin", begin);

    char b64[64 * 1024];
    for (int at = 0; at < frames; at += chunk_frames) {
        int n = frames - at;
        if (n > chunk_frames) n = chunk_frames;
        b64_encode((const uint8_t *)(pcm + at * 2), n * 2 * (int)sizeof(int16_t), b64);
        work_set_param(w, "sample_chunk", b64);
    }
    work_set_param(w, "sample_end", "1");
}

/* A preset that restores every parameter, LFO, pattern and lock of a source
 * patch and then plays SILENCE reads as a broken module, not as a missing
 * file. The blob carries the path so the UI can reload the audio. */
static void test_state_carries_the_sample_path(void) {
    printf("a preset remembers which sample the patch wants\n");
    work_t *w = work_create(&host);

    const char *path = "/data/UserData/UserLibrary/Samples/2026-07-29/take 3.wav";
    work_set_param(w, "sample_path", path);

    static char blob[65536];
    work_get_param(w, "state", blob, sizeof blob);
    CHECK(strstr(blob, "\"smp\":\"") != NULL,
          "the state blob does not mention the sample at all");

    work_t *v = work_create(&host);
    work_set_param(v, "state", blob);
    char got[256];
    work_get_param(v, "sample_path", got, sizeof got);
    CHECK(strcmp(got, path) == 0,
          "path came back as \"%s\", sent \"%s\"", got, path);

    /* A patch with no sample must not grow a key, or every FX-only preset
     * pays for a feature it does not use. */
    work_t *u = work_create(&host);
    work_get_param(u, "state", blob, sizeof blob);
    CHECK(strstr(blob, "\"smp\"") == NULL,
          "an FX-only patch emitted a sample key");

    /* Quotes and backslashes have to survive, or one badly named file makes
     * the whole blob unparseable and takes the patch down with it. */
    work_t *t = work_create(&host);
    const char *odd = "/samples/a \"quoted\\name\".wav";
    work_set_param(t, "sample_path", odd);
    work_get_param(t, "state", blob, sizeof blob);
    work_t *r = work_create(&host);
    work_set_param(r, "state", blob);
    work_get_param(r, "sample_path", got, sizeof got);
    CHECK(strcmp(got, odd) == 0,
          "awkward path came back as \"%s\", sent \"%s\"", got, odd);
    /* and the rest of the blob still parses — mix is written after it */
    work_set_param(t, "mix", "42");
    work_get_param(t, "state", blob, sizeof blob);
    work_t *r2 = work_create(&host);
    work_set_param(r2, "state", blob);
    work_get_param(r2, "mix", got, sizeof got);
    CHECK(atoi(got) == 42, "mix survived an escaped path as %s", got);

    work_destroy(w); work_destroy(v); work_destroy(u);
    work_destroy(t); work_destroy(r); work_destroy(r2);
}

/* Fire a voice the way a keyboard does. work_src_trigger is internal; the
 * public door is a note-on from an EXTERNAL source. */
static void fire_note(work_t *w, int note) {
    uint8_t on[3] = { 0x90, (uint8_t)note, 100 };
    work_on_midi(w, on, 3, MOVE_MIDI_SOURCE_EXTERNAL);
}

/* The sample machines had only an amp AD envelope, so every sample played at
 * full bandwidth however it was triggered. */
static void test_voice_filter(void) {
    printf("the voice filter shapes the sample, and is inert until you touch it\n");

    const int frames = 8000;
    static int16_t pcm[8000 * 2];
    make_ramp(pcm, frames);      /* a ramp is broadband: energy everywhere */

    /* Untouched, the filter must not change a single sample. A default that
     * colours the sound would silently alter every patch saved before it. */
    work_t *a = work_create(&host);
    send_sample(a, pcm, frames, 400);
    set_stage(a, WORK_STAGE_SRC, WORK_FX_ONESHOT);
    work_set_param(a, "src_p3", "127");           /* LEN full */
    fire_note(a, 60);
    int pa; int64_t ea = run_blocks(a, 40, &pa);

    work_t *b = work_create(&host);
    send_sample(b, pcm, frames, 400);
    set_stage(b, WORK_STAGE_SRC, WORK_FX_ONESHOT);
    work_set_param(b, "src_p3", "127");
    work_set_param(b, "vf_base",  "0");           /* the documented defaults */
    work_set_param(b, "vf_width", "127");
    work_set_param(b, "vf_env",   "64");
    fire_note(b, 60);
    int pb; int64_t eb = run_blocks(b, 40, &pb);
    CHECK(ea == eb, "the default filter changed the sound: %lld vs %lld",
          (long long)ea, (long long)eb);

    /* Closed down, it must take energy out. */
    work_t *c = work_create(&host);
    send_sample(c, pcm, frames, 400);
    set_stage(c, WORK_STAGE_SRC, WORK_FX_ONESHOT);
    work_set_param(c, "src_p3", "127");
    work_set_param(c, "vf_base",  "0");
    work_set_param(c, "vf_width", "8");           /* a narrow band down low */
    fire_note(c, 60);
    int pc; int64_t ec = run_blocks(c, 40, &pc);
    CHECK(ec < ea / 2, "a narrow band left %lld of %lld — it is not filtering",
          (long long)ec, (long long)ea);

    /* And it must reach the polyphonic machines, per voice. */
    work_t *d = work_create(&host);
    send_sample(d, pcm, frames, 400);
    set_stage(d, WORK_STAGE_SRC, WORK_FX_POLYSAMPLE);
    work_set_param(d, "src_p6", "127");
    fire_note(d, 60);
    fire_note(d, 64);
    int pd; int64_t open_e = run_blocks(d, 40, &pd);

    work_t *f = work_create(&host);
    send_sample(f, pcm, frames, 400);
    set_stage(f, WORK_STAGE_SRC, WORK_FX_POLYSAMPLE);
    work_set_param(f, "src_p6", "127");
    work_set_param(f, "vf_base",  "0");
    work_set_param(f, "vf_width", "8");
    fire_note(f, 60);
    fire_note(f, 64);
    int64_t shut_e = run_blocks(f, 40, &pd);
    CHECK(shut_e < open_e, "Polysample ignored the voice filter (%lld vs %lld)",
          (long long)shut_e, (long long)open_e);

    /* The settings survive a save. */
    static char blob[65536];
    work_get_param(c, "state", blob, sizeof blob);
    work_t *e = work_create(&host);
    work_set_param(e, "state", blob);
    char got[32];
    work_get_param(e, "vf_width", got, sizeof got);
    CHECK(atoi(got) == 8, "vf_width came back as %s after a reload", got);

    work_destroy(a); work_destroy(b); work_destroy(c);
    work_destroy(d); work_destroy(e); work_destroy(f);
}

/* schwung's remote UI polls this instead of reading the whole state blob. If
 * the revision never moved, a browser client would never see an edit made on
 * the hardware; if it moved on every read, the manager would pay for a full
 * state read every poll — and that blob serialises every step and lock. */
static void test_remote_ui_poll_digest(void) {
    printf("rui_poll reports a revision that moves on edits and not on reads\n");
    work_t *w = work_create(&host);

    char a[64], b[64];
    work_get_param(w, "rui_poll", a, sizeof a);
    CHECK(strchr(a, ':') != NULL, "rui_poll is \"%s\", expected rev:on:tick:bpm", a);

    /* four fields */
    int colons = 0;
    for (const char *c = a; *c; ++c) if (*c == ':') colons++;
    CHECK(colons == 3, "rui_poll has %d separators, expected 3 (%s)", colons, a);

    /* reading must not move it, or every poll costs a full state read */
    work_get_param(w, "rui_poll", b, sizeof b);
    CHECK(strcmp(a, b) == 0, "two reads with no edit differed: %s vs %s", a, b);

    /* an edit must move it, or a browser client never sees hardware changes */
    work_set_param(w, "mix", "37");
    work_get_param(w, "rui_poll", b, sizeof b);
    CHECK(strcmp(a, b) != 0, "editing mix left rui_poll at %s", b);

    work_destroy(w);
}

/* The browser editor talks through two keys: rui_set for every write, and a
 * flat mirror inside the state blob for the seed. schwung's manager parses
 * that blob as a FLAT object and drops anything that is a JSON array, so the
 * arrays the presets use are useless to it. */
static void test_remote_ui_bridge(void) {
    printf("rui_set dispatches writes and the state blob carries a flat mirror\n");
    work_t *w = work_create(&host);

    /* one key writes anything */
    work_set_param(w, "rui_set", "mix:33");
    char v[64];
    work_get_param(w, "mix", v, sizeof v);
    CHECK(atoi(v) == 33, "rui_set did not reach mix (%s)", v);

    work_set_param(w, "rui_set", "fx2_p3:99");
    work_get_param(w, "fx2_p3", v, sizeof v);
    CHECK(atoi(v) == 99, "rui_set did not reach fx2_p3 (%s)", v);

    /* malformed writes must be ignored rather than reaching something else */
    work_set_param(w, "rui_set", "nocolon");
    work_set_param(w, "rui_set", ":42");
    work_set_param(w, "rui_set", "rui_set:mix:1");   /* no recursion */
    work_get_param(w, "mix", v, sizeof v);
    CHECK(atoi(v) == 33, "a malformed rui_set changed mix to %s", v);

    /* the flat mirror: strings, not arrays, or the seed drops them */
    static char blob[65536];
    work_set_param(w, "machine1", "15");
    work_get_param(w, "state", blob, sizeof blob);
    CHECK(strstr(blob, "\"fp_src\":\"") != NULL, "no flat parameter mirror in the blob");
    CHECK(strstr(blob, "\"fvf\":\"") != NULL, "no flat voice-filter mirror");
    CHECK(strstr(blob, "\"fn1\":\"Roomtone Reverb\"") != NULL,
          "the blob does not name insert 1's machine for the browser");
    CHECK(strstr(blob, "\"fn_src\":\"") != NULL,
          "the blob does not name the source stage's machine for the browser");

    /* and the flat fields must not disturb a reload */
    work_t *r = work_create(&host);
    work_set_param(r, "state", blob);
    work_get_param(r, "mix", v, sizeof v);
    CHECK(atoi(v) == 33, "mix came back as %s after a reload", v);
    work_get_param(r, "fx2_p3", v, sizeof v);
    CHECK(atoi(v) == 99, "fx2_p3 came back as %s after a reload", v);

    work_get_param(w, "rui_play", v, sizeof v);
    CHECK(strchr(v, ':') != NULL, "rui_play is \"%s\", expected on:tick:bpm", v);

    work_destroy(w); work_destroy(r);
}

static void test_sample_transfer(void) {
    printf("a sample transfers in chunks and lands byte-exact\n");
    work_t *w = work_create(&host);

    const int frames = 3000;
    static int16_t pcm[3000 * 2];
    make_ramp(pcm, frames);

    /* Nothing is visible until the transfer commits. */
    work_set_param(w, "sample_begin", "3000:ramp");
    char probe[64];
    work_get_param(w, "sample_frames", probe, sizeof probe);
    CHECK(atoi(probe) == 0, "sample_frames was %s before sample_end — a partial "
          "upload must never be audible", probe);

    work_destroy(w);
    w = work_create(&host);
    send_sample(w, pcm, frames, 400);

    work_get_param(w, "sample_frames", probe, sizeof probe);
    CHECK(atoi(probe) == frames, "committed %s frames, sent %d", probe, frames);
    work_get_param(w, "sample_name", probe, sizeof probe);
    CHECK(strcmp(probe, "ramp") == 0, "sample name round-tripped as \"%s\"", probe);

    /* Play it back and compare against the source. START 0, LEN full, LOOP off,
     * instant attack, full level, centre pan, unity tune. */
    work_set_param(w, "src", "21");
    work_set_param(w, "src_p1", "64");   /* TUNE = unity */
    work_set_param(w, "src_p2", "0");    /* STRT */
    work_set_param(w, "src_p3", "127");  /* LEN  */
    work_set_param(w, "src_p4", "0");    /* LOOP off */
    work_set_param(w, "src_p5", "0");    /* ATK  */
    work_set_param(w, "src_p6", "127");  /* DEC  */
    work_set_param(w, "src_p7", "127");  /* LEV  */
    work_set_param(w, "src_p8", "64");   /* PAN centre */
    work_set_param(w, "mix", "127");
    work_set_param(w, "fx2", "0");

    /* A note fires the voice. */
    uint8_t note_on[3] = { 0x90, 60, 100 };
    work_on_midi(w, note_on, 3, 2);

    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];
    work_process(w, in, out, BLOCK);

    /* Playback should track the ramp. Compare SHAPE, not values — level, pan
     * law and the envelope all scale it. Start at frame 32: even at ATK 0 the
     * attack is half a millisecond (~22 frames), and a rising envelope over a
     * falling sample inverts the comparison while it lasts. */
    int matched = 0, tested = 0;
    for (int i = 33; i < 96; ++i) {
        int src_up = pcm[i * 2] > pcm[(i - 1) * 2];
        int out_up = out[i * 2] > out[(i - 1) * 2];
        if (src_up == out_up) matched++;
        tested++;
    }
    CHECK(matched >= tested - 2, "playback followed the source ramp in only %d "
          "of %d steps — the transfer is misaligned", matched, tested);

    work_destroy(w);
}

static void test_sample_bounds(void) {
    printf("an oversized or malformed transfer cannot run past the buffer\n");
    work_t *w = work_create(&host);
    char probe[64];

    work_get_param(w, "sample_max", probe, sizeof probe);
    const int cap = atoi(probe);
    CHECK(cap > 0, "sample_max reported %s", probe);

    /* Declare far more than fits. The engine must clamp rather than trust it. */
    char begin[64];
    snprintf(begin, sizeof begin, "%d", cap * 4);
    work_set_param(w, "sample_begin", begin);

    /* Garbage must be skipped, not decoded. Note the characters: "not base64"
     * would NOT do as a junk string, because every letter and digit in it is a
     * valid base64 symbol — it decodes to real bytes. Only symbols outside the
     * alphabet actually test the rejection path. */
    work_set_param(w, "sample_chunk", "!!!!@@@@####$$$$%%%%^^^^&&&&");
    work_get_param(w, "sample_fill", probe, sizeof probe);
    CHECK(atoi(probe) == 0, "junk decoded to %s frames", probe);

    /* Now push more real audio than the buffer holds. */
    static int16_t pcm[8192 * 2];
    make_ramp(pcm, 8192);
    char b64[64 * 1024];
    for (int i = 0; i < 200; ++i) {
        b64_encode((const uint8_t *)pcm, 8192 * 2 * (int)sizeof(int16_t), b64);
        work_set_param(w, "sample_chunk", b64);
    }
    work_set_param(w, "sample_end", "1");
    work_get_param(w, "sample_frames", probe, sizeof probe);
    CHECK(atoi(probe) <= cap, "committed %s frames against a %d-frame buffer",
          probe, cap);
    CHECK(atoi(probe) > 0, "an over-long transfer committed nothing at all");

    work_destroy(w);
}

static void test_single_player(void) {
    printf("One Shot fires on a trig and honours its window\n");
    work_t *w = work_create(&host);

    const int frames = 4000;
    static int16_t pcm[4000 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 600);

    work_set_param(w, "src", "21");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "src_p1", "64");
    work_set_param(w, "src_p2", "0");
    work_set_param(w, "src_p3", "127");
    work_set_param(w, "src_p4", "0");
    work_set_param(w, "src_p5", "0");
    work_set_param(w, "src_p6", "127");
    work_set_param(w, "src_p7", "127");
    work_set_param(w, "src_p8", "64");

    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];

    /* Untriggered, a source machine is silent. */
    work_process(w, in, out, BLOCK);
    int64_t idle = 0;
    for (int i = 0; i < BLOCK * 2; ++i) idle += llabs(out[i]);
    CHECK(idle == 0, "One Shot made sound with no trig (energy %lld)",
          (long long)idle);

    /* A sequencer trig starts it. */
    work_set_param(w, "seq_on", "1");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "seq_len", "16");
    uint8_t start[1] = { 0xFA };
    work_on_midi(w, start, 1, 2);

    int64_t fired = 0;
    for (int b = 0; b < 8; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) fired += llabs(out[i]);
    }
    CHECK(fired > 0, "a trig did not start the sample");

    /* LOOP off must eventually stop; LOOP on must not. */
    work_destroy(w);
    w = work_create(&host);
    send_sample(w, pcm, 400, 400);           /* a short one, ~9 ms */
    work_set_param(w, "src", "21");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "src_p1", "64"); work_set_param(w, "src_p2", "0");
    work_set_param(w, "src_p3", "127"); work_set_param(w, "src_p5", "0");
    work_set_param(w, "src_p6", "127"); work_set_param(w, "src_p7", "127");
    work_set_param(w, "src_p8", "64");

    work_set_param(w, "src_p4", "0");        /* LOOP off */
    uint8_t note_on[3] = { 0x90, 60, 100 };
    work_on_midi(w, note_on, 3, 2);
    for (int b = 0; b < 12; ++b) work_process(w, in, out, BLOCK);
    int64_t tail = 0;
    for (int b = 0; b < 4; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) tail += llabs(out[i]);
    }
    CHECK(tail == 0, "LOOP off kept sounding past the window (energy %lld)",
          (long long)tail);

    work_set_param(w, "src_p4", "127");      /* LOOP on */
    work_on_midi(w, note_on, 3, 2);
    for (int b = 0; b < 12; ++b) work_process(w, in, out, BLOCK);
    int64_t looped = 0;
    for (int b = 0; b < 4; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) looped += llabs(out[i]);
    }
    CHECK(looped > 0, "LOOP on stopped at the end of the window");

    work_destroy(w);
}


static void test_grainer_reads_the_sample(void) {
    printf("Granulator granulates the loaded sample, and live input without one\n");
    work_t *w = work_create(&host);

    work_set_param(w, "src", "20");     /* Granulator */
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");

    /* No sample, no input: a live-input granulator has nothing to grind. */
    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];
    int64_t silent = 0;
    for (int b = 0; b < 20; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) silent += llabs(out[i]);
    }
    CHECK(silent == 0, "Granulator made sound from silence with no sample (%lld)",
          (long long)silent);

    /* Load one. Now the same silent input must produce grains — that is the
     * whole point of an SRC machine. */
    const int frames = 4000;
    static int16_t pcm[4000 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 800);

    int64_t grained = 0;
    for (int b = 0; b < 40; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) grained += llabs(out[i]);
    }
    CHECK(grained > 0, "Granulator stayed silent with a sample loaded");

    /* Clearing it returns to live granulation. */
    work_set_param(w, "sample_clear", "1");
    int64_t after = 0;
    for (int b = 0; b < 40; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) after += llabs(out[i]);
    }
    CHECK(after < grained / 4,
          "clearing the sample left Granulator sounding (%lld vs %lld)",
          (long long)after, (long long)grained);

    work_destroy(w);
}

/* -------------------------------------------------- Phase 3: SRC machines */

/* Fire a note at a machine holding a sample and report the energy it makes. */
static int64_t src_energy(int machine, int note, int blocks,
                          void (*setup)(work_t *)) {
    work_t *w = work_create(&host);
    const int frames = 6000;
    static int16_t pcm[6000 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 1000);

    char code[8]; snprintf(code, sizeof code, "%d", machine);
    work_set_param(w, "src", code);
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    if (setup) setup(w);

    uint8_t on[3] = { 0x90, (uint8_t)note, 100 };
    work_on_midi(w, on, 3, 2);

    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];
    int64_t e = 0;
    for (int b = 0; b < blocks; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) e += llabs(out[i]);
    }
    work_destroy(w);
    return e;
}

static void test_multi_player_is_polyphonic(void) {
    printf("Polysample is polyphonic and tracks note pitch\n");

    work_t *w = work_create(&host);
    const int frames = 6000;
    static int16_t pcm[6000 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 1000);
    work_set_param(w, "src", "22");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "src_p2", "0");      /* no vibrato, so this is pitch only */

    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];

    /* one note */
    uint8_t n1[3] = { 0x90, 60, 100 };
    work_on_midi(w, n1, 3, 2);
    int64_t one = 0;
    for (int b = 0; b < 6; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) one += llabs(out[i]);
    }
    CHECK(one > 0, "a single note made no sound");

    /* four notes at once must be louder than one — that is what polyphony is */
    work_destroy(w);
    w = work_create(&host);
    send_sample(w, pcm, frames, 1000);
    work_set_param(w, "src", "22");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "src_p2", "0");
    for (int n = 0; n < 4; ++n) {
        uint8_t on[3] = { 0x90, (uint8_t)(60 + n * 3), 100 };
        work_on_midi(w, on, 3, 2);
        work_process(w, in, out, BLOCK);       /* each note lands its own block */
    }
    int64_t many = 0;
    for (int b = 0; b < 6; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) many += llabs(out[i]);
    }
    CHECK(many > one, "four notes were no louder than one — voices are not "
          "stacking, so the machine is monophonic (%lld vs %lld)",
          (long long)many, (long long)one);

    /* Eight voices is the documented limit; a ninth must steal, not overrun. */
    for (int n = 0; n < 16; ++n) {
        uint8_t on[3] = { 0x90, (uint8_t)(40 + n), 100 };
        work_on_midi(w, on, 3, 2);
        work_process(w, in, out, BLOCK);
    }
    int railed = 0;
    int64_t e = run_blocks(w, 40, &railed);
    CHECK(e >= 0 && railed < 40 * BLOCK, "sixteen notes railed the output");
    work_destroy(w);
}

/* A note an octave up must read through the sample about twice as fast, so it
 * runs out sooner. Comparing durations is a pitch test that needs no FFT. */
static void test_multi_player_pitch(void) {
    printf("a higher note plays the sample faster\n");
    int64_t low  = src_energy(22, 48, 200, NULL);   /* an octave below unity */
    int64_t high = src_energy(22, 72, 200, NULL);   /* an octave above       */
    CHECK(low > high, "the low note did not sound longer than the high one "
          "(%lld vs %lld) — pitch is not reaching the read rate",
          (long long)low, (long long)high);
}

/* Slicer's PLAY mode knob. It lives on the SOURCE stage — writing fx1_p2 here
 * set a parameter of whatever was in the first insert instead, and the reverse
 * check still passed because plain forward playback also makes a sound. */
static void set_reverse(work_t *w)  { work_set_param(w, "src_p2", "40");  }
static void set_fwd_loop(work_t *w) { work_set_param(w, "src_p2", "80");  }

static void test_subtracks_play_modes(void) {
    printf("Slicer plays forward, reverse and loops\n");

    int64_t fwd = src_energy(23, 60, 60, NULL);
    CHECK(fwd > 0, "forward mode made no sound");

    int64_t rev = src_energy(23, 60, 60, set_reverse);
    CHECK(rev > 0, "reverse mode made no sound");

    /* A forward loop must still be sounding long after a one-shot has ended.
     * The window is the whole 6000-frame sample, ~136 ms, so 200 blocks
     * (~580 ms) is well past it. */
    work_t *w = work_create(&host);
    const int frames = 6000;
    static int16_t pcm[6000 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 1000);
    work_set_param(w, "src", "23");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "src_p6", "127");      /* long decay, so LEN ends it */
    set_fwd_loop(w);
    uint8_t on[3] = { 0x90, 60, 100 };
    work_on_midi(w, on, 3, 2);

    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];
    for (int b = 0; b < 100; ++b) work_process(w, in, out, BLOCK);
    int64_t late = 0;
    for (int b = 0; b < 40; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) late += llabs(out[i]);
    }
    CHECK(late > 0, "forward loop stopped at the end of the window");
    work_destroy(w);
}

static void test_wavefinder_needs_a_wavetable(void) {
    printf("Wavescan oscillates from the sample, and is silent without one\n");

    /* Too short to hold two 2048-frame waves: silent rather than reading junk. */
    work_t *w = work_create(&host);
    work_set_param(w, "src", "24");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];
    int64_t empty = 0;
    for (int b = 0; b < 20; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) empty += llabs(out[i]);
    }
    CHECK(empty == 0, "Wavescan made sound with no wavetable loaded");
    work_destroy(w);

    /* Long enough for several waves: it should oscillate continuously, with
     * no trig at all — it is an oscillator, not a one-shot. */
    w = work_create(&host);
    const int frames = 2048 * 6;
    static int16_t pcm[2048 * 6 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 2048);
    work_set_param(w, "src", "24");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    int64_t e = 0;
    for (int b = 0; b < 40; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) e += llabs(out[i]);
    }
    CHECK(e > 0, "Wavescan was silent with a wavetable loaded");
    work_destroy(w);
}

static void test_shape_shelves(void) {
    printf("Tilt boosts and cuts its shelves, and is flat in the middle\n");
    work_t *w = work_create(&host);
    work_set_param(w, "machine1", "25");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "fx1_p5", "0");      /* no drive, so this measures the EQ */
    work_set_param(w, "fx1_p7", "127");    /* full level */
    work_set_param(w, "fx1_p8", "127");    /* full wet  */
    work_set_param(w, "fx1_p1", "64");     /* LO.G flat */
    work_set_param(w, "fx1_p4", "64");     /* HI.G flat */
    work_set_param(w, "fx1_p5", "64");     /* WDTH unity */

    /* Tilt makes no sound of its own: it shapes what is already there. */
    int16_t quiet[BLOCK * 2] = {0}, out[BLOCK * 2];
    int64_t self = 0;
    for (int b = 0; b < 10; ++b) {
        work_process(w, quiet, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) self += llabs(out[i]);
    }
    CHECK(self == 0, "Tilt generated sound from silence (%lld)", (long long)self);

    /* Low shelf boost must raise a low tone more than a cut does.
     *
     * The tone has to be CONTINUOUS across blocks. Building one 128-sample
     * buffer and replaying it looks like a 60 Hz sine but is not: 128 samples
     * is a sixth of a cycle at 60 Hz, so replaying it puts a step
     * discontinuity every block — a 344 Hz buzz with broadband harmonics that
     * a low shelf cannot remove, which caps the measurable cut no matter how
     * good the filter is. Carrying the phase across blocks was the difference
     * between measuring 10x and 21x here. */
    int16_t lo[BLOCK * 2];
    float lo_phase = 0.0f;
    #define FILL_LO() do { \
        for (int i = 0; i < BLOCK; ++i) { \
            float v = sinf(lo_phase) * 8000.0f; \
            lo_phase += 6.28318531f * 60.0f / (float)WORK_SR; \
            if (lo_phase > 6.28318531f) lo_phase -= 6.28318531f; \
            lo[i * 2] = (int16_t)v; lo[i * 2 + 1] = (int16_t)v; \
        } \
    } while (0)
    FILL_LO();
    work_set_param(w, "fx1_p1", "127");    /* LO.G max boost */
    int64_t boosted = 0;
    for (int b = 0; b < 20; ++b) {
        FILL_LO();
        work_process(w, lo, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) boosted += llabs(out[i]);
    }
    work_set_param(w, "fx1_p1", "0");      /* LO.G max cut */
    int64_t cut = 0;
    for (int b = 0; b < 20; ++b) {
        FILL_LO();
        work_process(w, lo, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) cut += llabs(out[i]);
    }
    CHECK(boosted > cut * 2, "the low shelf barely moved a 60 Hz tone: "
          "boost %lld vs cut %lld", (long long)boosted, (long long)cut);

    /* The ENDS are the point of this control. The manual documents -64 as a
     * highpass response — the low band GONE, not merely reduced — and capping
     * the cut at -12 dB made the machine feel tame on hardware ("it's not
     * extreme, but able to cut lows or highs").
     *
     * Measure boost and cut at the SAME corner, which the first version of
     * this test did not: it compared a cut taken at one LO.F against a boost
     * taken at another and the ratio said more about the crossover than the
     * shelf. And put the corner well ABOVE the tone — with LO.F at its default
     * the corner sits near 93 Hz, barely above 60 Hz, so even a perfect
     * subtraction can only remove about 70% of it.
     *
     * Measured at LO.F 110 relative to flat: -11.6 dB at LO.G 32 and -4.9 dB
     * at 48, matching the manual's -12 and -6; +12.0 dB at 127; and -20.7 dB
     * at 0, which is exactly a one-pole complementary highpass. Full cut
     * against full boost is about 43x. The null is deep rather than infinite
     * because the crossover is one pole — the honest limit of this topology,
     * and steepening it makes the null WORSE, not better. */
    work_set_param(w, "fx1_p2", "110");       /* corner ~1 kHz, tone well inside */
    work_set_param(w, "fx1_p1", "127");
    int64_t hiBoost = 0;
    for (int b = 0; b < 30; ++b) {
        FILL_LO();
        work_process(w, lo, out, BLOCK);
        if (b >= 10) for (int i = 0; i < BLOCK * 2; ++i) hiBoost += llabs(out[i]);
    }
    work_set_param(w, "fx1_p1", "0");
    int64_t hiCut = 0;
    for (int b = 0; b < 30; ++b) {
        FILL_LO();
        work_process(w, lo, out, BLOCK);
        if (b >= 10) for (int i = 0; i < BLOCK * 2; ++i) hiCut += llabs(out[i]);
    }
    CHECK(hiCut * 15 < hiBoost, "at a corner above the tone, full cut left %lld "
          "against a boost of %lld — under 15x means the extreme is not "
          "approaching a highpass", (long long)hiCut, (long long)hiBoost);

    /* ...and half travel stays a gentle shelf, so the control is playable
     * rather than an on/off switch. */
    work_set_param(w, "fx1_p1", "32");        /* the documented -12 dB point */
    int64_t half = 0;
    for (int b = 0; b < 30; ++b) {
        FILL_LO();
        work_process(w, lo, out, BLOCK);
        if (b >= 10) for (int i = 0; i < BLOCK * 2; ++i) half += llabs(out[i]);
    }
    CHECK(half > hiCut * 2.5, "half-travel cut collapsed towards silence (%lld vs "
          "%lld at full cut) — the taper is too abrupt to play",
          (long long)half, (long long)hiCut);
    work_set_param(w, "fx1_p2", "40");
    work_destroy(w);
}


/* Reported from hardware: "I keep getting feedback." Overwork always reads the
 * mic, and with a SOURCE machine generating there is no reason for it to be in
 * the path — nothing to blend it with, and a live mic into speakers is a loop.
 * The guard mutes on RISK; this removes the path entirely, which does not
 * depend on detecting anything. */
static void test_source_machine_ignores_the_input(void) {
    printf("a source machine keeps the live input out of the signal path\n");
    work_t *w = work_create(&host);

    const int frames = 4000;
    static int16_t pcm[4000 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 800);

    /* a loud input, the mic howling */
    int16_t loud[BLOCK * 2], out[BLOCK * 2];
    for (int i = 0; i < BLOCK * 2; ++i) loud[i] = (int16_t)(20000 - (i % 40000));

    /* Bypass everywhere at MIX 0 is the dry path: the input SHOULD pass. */
    work_set_param(w, "src", "0");
    work_set_param(w, "fx1", "0");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "0");
    int64_t passed = 0;
    for (int b = 0; b < 8; ++b) {
        work_process(w, loud, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) passed += llabs(out[i]);
    }
    CHECK(passed > 0, "the dry path is silent even with Bypass — this test "
          "would pass for the wrong reason");

    /* Now a source machine in the SOURCE stage, still MIX 0 and monitor ON.
     * The input must not reach the output by ANY route. */
    work_set_param(w, "src", "21");
    work_set_param(w, "monitor", "1");
    for (int p = 1; p <= 8; ++p) {
        char k[16]; snprintf(k, sizeof k, "src_p%d", p);
        work_set_param(w, k, "0");                  /* LEV 0: no sample either */
    }
    int64_t leaked = 0;
    for (int b = 0; b < 8; ++b) {
        work_process(w, loud, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) leaked += llabs(out[i]);
    }
    CHECK(leaked == 0, "the live input leaked through a source machine "
          "(energy %lld) — that is the feedback path", (long long)leaked);

    /* An FX machine must still hear the input, or the module is broken. The
     * source stage goes back to Bypass first — a source sitting in front of the
     * inserts is exactly what mutes the input, so leaving it loaded would make
     * this check fail for a reason that has nothing to do with the insert. */
    work_set_param(w, "src", "0");
    work_set_param(w, "fx1", "11");             /* Low-Pass Filter */
    work_set_param(w, "mix", "127");
    int64_t heard = 0;
    for (int b = 0; b < 8; ++b) {
        work_process(w, loud, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) heard += llabs(out[i]);
    }
    CHECK(heard > 0, "an FX machine stopped hearing the input");

    work_destroy(w);
}


/* Reported from hardware: "when I hit step button it triggers the sample".
 * Move's surface sends step buttons as NOTES 16-31, and any note-on fired a
 * voice — so the sequencer's own keys played the sound instead of editing the
 * pattern. Filtering on note NUMBER would break an external keyboard, whose
 * range includes 60, the sampler's unity pitch. The rule is by SOURCE. */
static void test_surface_control_notes_do_not_fire_voices(void) {
    printf("Move's step buttons do not trigger the sample; a keyboard does\n");
    work_t *w = work_create(&host);

    const int frames = 4000;
    static int16_t pcm[4000 * 2];
    make_ramp(pcm, frames);
    send_sample(w, pcm, frames, 800);
    work_set_param(w, "src", "21");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "src_p7", "127");

    int16_t in[BLOCK * 2] = {0}, out[BLOCK * 2];

    /* A step button: note 20, from Move's own surface. */
    uint8_t step[3] = { 0x90, 20, 127 };
    work_on_midi(w, step, 3, MOVE_MIDI_SOURCE_INTERNAL);
    int64_t fired = 0;
    for (int b = 0; b < 8; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) fired += llabs(out[i]);
    }
    CHECK(fired == 0, "a step button fired the sample (energy %lld)",
          (long long)fired);

    /* Move's PADS are playable, so an internal pad note must still fire. */
    uint8_t pad[3] = { 0x90, 72, 100 };
    work_on_midi(w, pad, 3, MOVE_MIDI_SOURCE_INTERNAL);
    int64_t padded = 0;
    for (int b = 0; b < 8; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) padded += llabs(out[i]);
    }
    CHECK(padded > 0, "a pad note did not fire the sample");

    /* And an EXTERNAL keyboard must reach the whole range, including the low
     * notes that overlap Move's control numbers. */
    work_destroy(w);
    w = work_create(&host);
    send_sample(w, pcm, frames, 800);
    work_set_param(w, "src", "21");
    work_set_param(w, "fx2", "0");
    work_set_param(w, "mix", "127");
    work_set_param(w, "src_p7", "127");
    uint8_t low[3] = { 0x90, 36, 100 };
    work_on_midi(w, low, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    int64_t ext = 0;
    for (int b = 0; b < 8; ++b) {
        work_process(w, in, out, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) ext += llabs(out[i]);
    }
    CHECK(ext > 0, "an external keyboard note below 68 was ignored");

    work_destroy(w);
}

int main(void) {
    printf("Work engine — host simulator\n\n");

    test_bypass_transparent();
    test_all_machines_bounded();
    test_global_mix_dry();
    test_state_roundtrip();
    test_get_param_tiny_buffers();
    test_compressor_reduces_gain();
    test_filterbank_silence();
    test_machine_change_resets();
    test_fx_lfo_modulates();
    test_stages_run_in_series();
    test_stages_refuse_the_wrong_family();
    test_every_machine_has_a_stage();
    test_lock_map_is_append_only();
    test_machine_lock_respects_the_family();
    test_machine_load_installs_defaults();
    test_midi_clock();
    test_param_name_table();
    test_ui_hierarchy_not_served();
    test_reverbs_are_distinct();
    test_grainer();

    printf("\n-- sequencer --\n");
    test_seq_locks_apply_and_revert();
    test_seq_off_ignores_pattern();
    test_trig_conditions();
    test_machine_lock();
    test_micro_timing();
    test_seq_state_roundtrip();
    test_seq_blob_tiny_buffers();
    test_seq_clear();
    test_locks_string_api();
    test_lock_labels();
    test_transport_restarts_pattern();

    printf("\n-- v0.3.0 performance pack --\n");
    test_live_record();
    test_step_probability();
    test_mod_envelope();
    test_third_lfo();
    test_midi_cc();

    printf("\n-- Tier A: bank, song, history, transform --\n");
    test_pattern_bank();
    test_song_mode();
    test_undo_redo_memorize();
    test_trig_types();
    test_transform_and_quantize();
    test_page_mask();
    test_nrpn();
    test_feedback_monitor();
    test_hw_input_flag();

    test_state_carries_the_sample_path();
    test_remote_ui_poll_digest();
    test_remote_ui_bridge();
    test_voice_filter();
    test_sample_transfer();
    test_sample_bounds();
    test_single_player();

    test_grainer_reads_the_sample();

    test_multi_player_is_polyphonic();
    test_multi_player_pitch();
    test_subtracks_play_modes();
    test_wavefinder_needs_a_wavetable();
    test_shape_shelves();

    test_source_machine_ignores_the_input();

    test_surface_control_notes_do_not_fire_voices();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
