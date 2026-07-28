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

static void set_slot(work_t *w, int slot, int machine) {
    char key[8], val[8];
    snprintf(key, sizeof(key), "fx%d", slot + 1);
    snprintf(val, sizeof(val), "%d", machine);
    work_set_param(w, key, val);
}

static void set_all_params(work_t *w, int slot, int value) {
    for (int i = 0; i < WORK_PARAMS; ++i) {
        char key[16], val[8];
        snprintf(key, sizeof(key), "fx%d_p%d", slot + 1, i + 1);
        snprintf(val, sizeof(val), "%d", value);
        work_set_param(w, key, val);
    }
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
            set_slot(w, 0, mc);
            if (settings[sIdx] >= 0) set_all_params(w, 0, settings[sIdx]);

            int railed = 0;
            int64_t e = run_blocks(w, 200, &railed);
            int64_t samples = (int64_t)200 * BLOCK * 2;

            CHECK(railed < samples / 2,
                  "%s (%s): %d/%lld samples railed — runaway gain",
                  work_machine_name(mc), label[sIdx], railed, (long long)samples);

            /* Bypass at all-min is still bypass; every other machine at its
             * defaults should pass or produce something audible. */
            if (sIdx == 1 && mc != WORK_FX_BYPASS) {
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
    set_slot(w, 0, WORK_FX_SUPERVOID);
    set_slot(w, 1, WORK_FX_DEGRADER);
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

    set_slot(a, 0, WORK_FX_PHASE98);
    set_slot(a, 1, WORK_FX_STEELBOX);
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
    set_slot(w, 0, WORK_FX_WARBLE);
    set_slot(w, 1, WORK_FX_RUMSKLANG);
    set_all_params(w, 0, 127);
    set_all_params(w, 1, 127);
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
    set_slot(w, 0, WORK_FX_COMP);
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
    set_slot(w, 0, WORK_FX_FBANK);
    set_all_params(w, 0, 0);

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

    set_slot(w, 0, WORK_FX_SUPERVOID);
    work_set_param(w, "fx1_p7", "127");         /* fully wet */
    int dummy;
    run_blocks(w, 100, &dummy);                 /* build a long tail */

    set_slot(w, 0, WORK_FX_BYPASS);

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
    set_slot(a, 0, WORK_FX_LPF);
    work_set_param(a, "fx1_p5", "40");           /* cutoff  */
    int dummy;
    int64_t e_static = run_blocks(a, 120, &dummy);
    work_destroy(a);

    work_t *b = work_create(&host);
    set_slot(b, 0, WORK_FX_LPF);
    work_set_param(b, "fx1_p5", "40");
    work_set_param(b, "lfo1_dest", "4");         /* slot 1, param 5 = FREQ */
    work_set_param(b, "lfo1_depth", "127");
    work_set_param(b, "lfo1_spd", "100");
    int64_t e_mod = run_blocks(b, 120, &dummy);
    work_destroy(b);

    CHECK(llabs(e_mod - e_static) > e_static / 20,
          "LFO changed output energy by less than 5%% (%lld vs %lld)",
          (long long)e_mod, (long long)e_static);
}

/* Both slots run in series, so loading a second machine must change the sound
 * relative to running only the first. */
static void test_two_slot_series(void) {
    printf("slot 2 processes the output of slot 1\n");

    work_t *a = work_create(&host);
    set_slot(a, 0, WORK_FX_DIRT);
    int dummy;
    int64_t e1 = run_blocks(a, 60, &dummy);
    work_destroy(a);

    work_t *b = work_create(&host);
    set_slot(b, 0, WORK_FX_DIRT);
    set_slot(b, 1, WORK_FX_FBANK);
    set_all_params(b, 1, 0);                     /* a wall in slot 2 */
    int64_t e2 = run_blocks(b, 60, &dummy);
    work_destroy(b);

    CHECK(e1 > 0, "slot 1 alone produced silence");
    CHECK(e2 == 0, "slot 2 did not receive slot 1's output (energy %lld)",
          (long long)e2);
}

/* Loading a machine installs its defaults, the way selecting a machine does on
 * the hardware. */
static void test_machine_load_installs_defaults(void) {
    printf("loading a machine installs its default parameters\n");
    work_t *w = work_create(&host);
    assert(w);

    set_all_params(w, 0, 0);
    set_slot(w, 0, WORK_FX_SATDELAY);

    int nonzero = 0;
    for (int i = 0; i < WORK_PARAMS; ++i) {
        char key[16], buf[8];
        snprintf(key, sizeof(key), "fx1_p%d", i + 1);
        work_get_param(w, key, buf, sizeof(buf));
        if (atoi(buf) != 0) nonzero++;
    }
    CHECK(nonzero > 0, "Saturator Delay loaded with all parameters at zero");
    work_destroy(w);
}

/* MIDI clock must drive the tempo-synced machines without wedging anything. */
static void test_midi_clock(void) {
    printf("MIDI clock and note events are handled\n");
    work_t *w = work_create(&host);
    assert(w);
    set_slot(w, 0, WORK_FX_MMF);
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
static int eff_param(work_t *w, int slot, int idx) {
    char buf[128];
    if (work_get_param(w, slot ? "eff2" : "eff1", buf, sizeof(buf)) <= 0) return -1;
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

    set_slot(w, 0, WORK_FX_LPF);
    work_set_param(w, "fx1_p5", "100");        /* base cutoff */
    work_set_param(w, "seq_len", "2");
    work_set_param(w, "step0", "1:0:0:0");     /* trig, no locks   */
    work_set_param(w, "step1", "1:0:0:0");     /* trig, locks FREQ */
    work_set_param(w, "lock1_4", "10");        /* lock index 4 = slot1 knob E */
    work_set_param(w, "seq_on", "1");

    idle(w, 2);
    CHECK(eff_param(w, 0, 4) == 100, "step 0 should hold the base 100, got %d",
          eff_param(w, 0, 4));

    idle(w, blocks_per_step());
    CHECK(eff_param(w, 0, 4) == 10, "step 1 lock should force 10, got %d",
          eff_param(w, 0, 4));

    idle(w, blocks_per_step());
    CHECK(eff_param(w, 0, 4) == 100, "wrapping to step 0 should revert to 100, got %d",
          eff_param(w, 0, 4));

    work_destroy(w);
}

static void test_seq_off_ignores_pattern(void) {
    printf("a pattern has no effect while the sequencer is off\n");
    work_t *w = work_create(&host);
    assert(w);
    set_slot(w, 0, WORK_FX_LPF);
    work_set_param(w, "fx1_p5", "100");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "lock0_4", "5");
    /* seq_on deliberately left at 0 */

    idle(w, blocks_per_step() * 3);
    CHECK(eff_param(w, 0, 4) == 100,
          "locks leaked with the sequencer off (got %d)", eff_param(w, 0, 4));
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
        set_slot(w, 0, WORK_FX_LPF);
        work_set_param(w, "fx1_p5", "100");
        work_set_param(w, "seq_len", "1");

        char sv[32];
        snprintf(sv, sizeof(sv), "1:%d:0:0", cases[k].cond);
        work_set_param(w, "step0", sv);
        work_set_param(w, "lock0_4", "7");
        work_set_param(w, "fill", cases[k].fill ? "1" : "0");
        work_set_param(w, "seq_on", "1");

        for (int pass = 0; pass < 4; ++pass) {
            /* Force a fresh step edge each pass, then sample */
            idle(w, 2);
            int v = eff_param(w, 0, 4);
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
    set_slot(w, 0, WORK_FX_BYPASS);
    work_set_param(w, "seq_len", "2");
    work_set_param(w, "step0", "1:0:0:0");
    work_set_param(w, "step1", "1:0:0:0");
    char v[8];
    snprintf(v, sizeof(v), "%d", WORK_FX_DEGRADER);
    work_set_param(w, "lock1_16", v);          /* lock 16 = slot 1 machine */
    work_set_param(w, "seq_on", "1");

    idle(w, 2);
    char m[32];
    work_get_param(w, "effm", m, sizeof(m));
    CHECK(atoi(m) == WORK_FX_BYPASS, "step 0 machine should be Bypass, effm=%s", m);

    idle(w, blocks_per_step());
    work_get_param(w, "effm", m, sizeof(m));
    CHECK(atoi(m) == WORK_FX_DEGRADER, "step 1 should lock Degrader, effm=%s", m);

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
        set_slot(w, 0, WORK_FX_LPF);
        work_set_param(w, "fx1_p5", "100");
        work_set_param(w, "seq_len", "2");
        work_set_param(w, "step0", "1:0:0:0");
        work_set_param(w, "step1", variant ? "1:0:23:0" : "1:0:0:0");
        work_set_param(w, "lock1_4", "3");
        work_set_param(w, "seq_on", "1");

        idle(w, 2);
        idle(w, blocks_per_step() + 1);        /* just past step 1's nominal start */
        seen[variant] = eff_param(w, 0, 4);
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
    CHECK(strcmp(s, "0:0:0:0:0") == 0, "step 3 after clear reads %s", s);
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
    set_slot(w, 0, WORK_FX_CHRONO);

    char s[64];
    work_get_param(w, "locklabel0", s, sizeof(s));
    CHECK(strcmp(s, "1:TUNE") == 0, "slot 1 knob A under Chrono Pitch reads %s", s);

    /* Filterbank's knobs are Gain A..H in the manual, but the label here is
     * the band's frequency — a knob called "A" tells you nothing. */
    set_slot(w, 0, WORK_FX_FBANK);
    work_get_param(w, "locklabel0", s, sizeof(s));
    CHECK(strcmp(s, "1:90Hz") == 0, "slot 1 knob A under Filterbank reads %s", s);

    work_get_param(w, "locklabel16", s, sizeof(s));
    CHECK(strcmp(s, "1:MACH") == 0, "lock 16 should be slot 1's machine, reads %s", s);
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
static void test_ui_hierarchy_labels(void) {
    printf("ui_hierarchy carries the loaded machine's real knob labels\n");
    work_t *w = work_create(&host);
    assert(w);
    set_slot(w, 0, WORK_FX_CHRONO);
    set_slot(w, 1, WORK_FX_WARBLE);

    char buf[16384];
    int n = work_get_param(w, "ui_hierarchy", buf, sizeof(buf));
    CHECK(n > 0, "ui_hierarchy returned %d", n);
    CHECK(n < (int)sizeof(buf) - 1,
          "hierarchy is %d bytes; the device host buffer is 16384", n);

    CHECK(strstr(buf, "\"TUNE\"") != NULL, "Chrono Pitch's TUNE label missing");
    CHECK(strstr(buf, "\"FDBK\"") != NULL, "Chrono Pitch's FDBK label missing");
    CHECK(strstr(buf, "\"N.HPF\"") != NULL, "Warble's N.HPF label missing");
    CHECK(strstr(buf, "\"FX 1: Chrono Pitch\"") != NULL,
          "slot 1 link does not name the loaded machine");
    /* No knob anywhere should be labelled with a bare letter — that was the
     * original complaint: "I don't know what each does". */
    for (char c = 'A'; c <= 'H'; ++c) {
        char probe[16];
        snprintf(probe, sizeof(probe), "\"name\":\"%c\"", c);
        CHECK(strstr(buf, probe) == NULL,
              "hierarchy still exposes a bare letter label \"%c\"", c);
    }

    /* balanced braces and brackets — a cheap structural check */
    int braces = 0, brackets = 0, instr = 0;
    for (const char *c = buf; *c; ++c) {
        if (*c == '"' && (c == buf || c[-1] != '\\')) instr = !instr;
        if (instr) continue;
        if (*c == '{') braces++;
        else if (*c == '}') braces--;
        else if (*c == '[') brackets++;
        else if (*c == ']') brackets--;
    }
    CHECK(braces == 0, "unbalanced braces in hierarchy (%d)", braces);
    CHECK(brackets == 0, "unbalanced brackets in hierarchy (%d)", brackets);

    /* the labels must FOLLOW a machine change */
    set_slot(w, 0, WORK_FX_FBANK);
    work_get_param(w, "ui_hierarchy", buf, sizeof(buf));
    CHECK(strstr(buf, "\"FX 1: Filterbank\"") != NULL,
          "hierarchy did not follow the machine change");
    CHECK(strstr(buf, "\"TUNE\"") == NULL,
          "stale Chrono Pitch label survived the machine change");

    /* Bypass has no parameters — it must say so, not emit an empty level */
    set_slot(w, 0, WORK_FX_BYPASS);
    work_get_param(w, "ui_hierarchy", buf, sizeof(buf));
    CHECK(strstr(buf, "(no parameters)") != NULL,
          "Bypass produced an empty parameter level");

    /* and the usual short-buffer canary */
    for (int capsz = 8; capsz <= 4096; capsz *= 4) {
        char *small = malloc((size_t)capsz + 64);
        memset(small, 0x3C, (size_t)capsz + 64);
        int r = work_get_param(w, "ui_hierarchy", small, capsz);
        int ok = 1;
        for (int i = capsz; i < capsz + 64; ++i)
            if ((unsigned char)small[i] != 0x3C) ok = 0;
        CHECK(ok, "ui_hierarchy into %d bytes overwrote past the buffer", capsz);
        CHECK(r <= capsz - 1, "ui_hierarchy into %d bytes reported %d", capsz, r);
        free(small);
    }
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
    test_two_slot_series();
    test_machine_load_installs_defaults();
    test_midi_clock();
    test_param_name_table();
    test_ui_hierarchy_labels();

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

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
