/*
 * Work — clean-room FX engine inspired by the Elektron Tonverk's FX machines.
 * See work_core.h for the architecture and the clean-room statement.
 *
 * Layout of this file:
 *   1. small helpers (clamping, parameter mapping, PRNG)
 *   2. DSP primitives (SVF, one-poles, allpass, delay reads, Hilbert)
 *   3. per-slot state
 *   4. the twenty machines, in work_fx_t order
 *   5. slot dispatch, FX LFOs, parameter I/O
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp — machine names accepted by name or index */

#include "work_core.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ 1. helpers */

static float fclampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int iclamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Parameter 0..127 -> 0..1 */
static float p01(uint8_t v) { return (float)v / 127.0f; }

/* Parameter 0..127 -> -1..+1 with an exact zero at 64 (Elektron bipolar) */
static float pbi(uint8_t v) {
    return v >= 64 ? (float)(v - 64) / 63.0f : (float)(v - 64) / 64.0f;
}

/* Exponential map, for anything measured in Hz */
static float pexp(uint8_t v, float lo, float hi) {
    return lo * powf(hi / lo, p01(v));
}

/* Deterministic PRNG. Seeded per slot so the test suite is reproducible;
 * dropouts and freezes must not depend on wall-clock state. */
static uint32_t rnd_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (*s = x);
}
static float rnd_bi(uint32_t *s) {
    return (float)((int32_t)(rnd_next(s) >> 8)) * (1.0f / 8388608.0f) - 1.0f;
}
static float rnd_01(uint32_t *s) {
    return (float)(rnd_next(s) >> 8) * (1.0f / 16777216.0f);
}

/* Guard against a denormal/NaN escaping into the host's int16 conversion.
 * Every machine's output goes through this before the mix stage. */
static float sane(float v) {
    if (!isfinite(v)) return 0.0f;
    return fclampf(v, -8.0f, 8.0f);
}

/* snprintf into a bounded cursor. The engine's get_param("state") builds a
 * blob by repeated appends; snprintf returns the length it WOULD have
 * written, so accumulating it unclamped walks the cursor past the end and the
 * next append writes out of bounds through buf+n with an underflowed size.
 * That exact bug crashed Smack on device (v0.8.2). Never append without this. */
static int nclamp(int n, int cap) { return n < 0 ? 0 : (n > cap ? cap : n); }

/* ------------------------------------------------------- 2. DSP primitives */

/* Topology-preserving-transform state variable filter (Zavalishin/Cytomic).
 * Coefficients are refreshed once per block; audio-rate cutoff modulation at
 * LFO speeds does not need per-sample tan(). */
typedef struct { float ic1, ic2; } svf_t;
typedef struct { float g, k, a1, a2, a3; } svf_co_t;

static void svf_coeffs(svf_co_t *c, float fc, float q) {
    fc = fclampf(fc, 10.0f, (float)WORK_SR * 0.45f);
    c->g  = tanf((float)M_PI * fc / (float)WORK_SR);
    c->k  = 1.0f / fclampf(q, 0.5f, 20.0f);
    c->a1 = 1.0f / (1.0f + c->g * (c->g + c->k));
    c->a2 = c->g * c->a1;
    c->a3 = c->g * c->a2;
}

/* Runs one sample, filling whichever outputs the caller asked for. */
static void svf_run(svf_t *s, const svf_co_t *c, float in,
                    float *lp, float *bp, float *hp) {
    float v3 = in - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    if (lp) *lp = v2;
    if (bp) *bp = v1;
    if (hp) *hp = in - c->k * v1 - v2;
}

static float svf_lp(svf_t *s, const svf_co_t *c, float in) {
    float o; svf_run(s, c, in, &o, NULL, NULL); return o;
}
static float svf_hp(svf_t *s, const svf_co_t *c, float in) {
    float o; svf_run(s, c, in, NULL, NULL, &o); return o;
}
static float svf_bp(svf_t *s, const svf_co_t *c, float in) {
    float o; svf_run(s, c, in, NULL, &o, NULL); return o;
}

/* One-pole low-pass, for damping and smoothing where a full SVF is overkill */
typedef struct { float z; } op_t;
static float op_lp(op_t *s, float in, float a) {
    s->z += a * (in - s->z);
    return s->z;
}
/* Coefficient for a one-pole at fc Hz */
static float op_a(float fc) {
    return fclampf(1.0f - expf(-2.0f * (float)M_PI * fc / (float)WORK_SR), 0.0f, 1.0f);
}

/* DC blocker — the wavefolder and rectifier both generate offset */
typedef struct { float x1, y1; } dc_t;
static float dc_run(dc_t *s, float x) {
    float y = x - s->x1 + 0.9975f * s->y1;
    s->x1 = x; s->y1 = y;
    return y;
}

/* Soft saturation. Cheap tanh approximation, monotonic and exactly odd. */
static float softclip(float x) {
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Interpolated read from a stereo-interleaved delay line. `back` is how many
 * frames behind the write cursor to read, and may be fractional. */
static float dl_read(const float *dl, int len, int w, float back, int ch) {
    if (back < 1.0f) back = 1.0f;
    if (back > (float)(len - 2)) back = (float)(len - 2);
    float fpos = (float)w - back;
    while (fpos < 0.0f) fpos += (float)len;
    int   i0 = (int)fpos;
    float fr = fpos - (float)i0;
    int   i1 = i0 + 1; if (i1 >= len) i1 -= len;
    return dl[i0 * 2 + ch] * (1.0f - fr) + dl[i1 * 2 + ch] * fr;
}

/* Second-order allpass in z^-2, the building block of the Hilbert network */
typedef struct { float x1, x2, y1, y2; } ap2_t;
static float ap2_run(ap2_t *s, float x, float a2) {
    float y = a2 * (x + s->y2) - s->x2;
    s->x2 = s->x1; s->x1 = x;
    s->y2 = s->y1; s->y1 = y;
    return y;
}

/* Two-path IIR Hilbert network (Niemitalo). The paths differ by ~90 degrees
 * across the audio band; path B is delayed one sample to complete the pair. */
static const float HILB_A[4] = {0.6923877778f, 0.9360654323f, 0.9882295227f, 0.9987488453f};
static const float HILB_B[4] = {0.4021921162f, 0.8561710882f, 0.9722909546f, 0.9952884791f};

typedef struct { ap2_t a[4], b[4]; float bz; } hilbert_t;
static void hilbert_run(hilbert_t *h, float x, float *i_out, float *q_out) {
    float a = x, b = x;
    for (int k = 0; k < 4; ++k) a = ap2_run(&h->a[k], a, HILB_A[k] * HILB_A[k]);
    for (int k = 0; k < 4; ++k) b = ap2_run(&h->b[k], b, HILB_B[k] * HILB_B[k]);
    *i_out = a;
    *q_out = h->bz;
    h->bz  = b;
}

/* First-order allpass, for the phaser */
typedef struct { float z; } ap1_t;
static float ap1_run(ap1_t *s, float x, float a) {
    float y = -a * x + s->z;
    s->z = x + a * y;
    return y;
}

/* ------------------------------------------------------------ 3. slot state */

/* Freeverb-derived comb/allpass tunings at 44.1 kHz, plus the stereo spread
 * that decorrelates the right channel. */
static const int COMB_TUNE[WORK_TANK_COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int AP_TUNE[WORK_TANK_APS]     = {556, 441, 341, 225};
#define TANK_SPREAD 23
#define WORK_AP_LEN 1024

typedef struct {
    /* generic delay line, stereo interleaved: delays, combs, flanger,
     * chorus, warble, chrono-pitch grains */
    float *dl;
    int    dw;

    /* reverb predelay + early reflections */
    float *pre;
    int    pw;

    /* reverb tank */
    float *comb;                 /* [WORK_TANK_COMBS][2][WORK_TANK_LEN] */
    float *ap;                   /* [WORK_TANK_APS][2][WORK_AP_LEN]     */
    int    comb_w[WORK_TANK_COMBS][2];
    int    ap_w[WORK_TANK_APS][2];
    op_t   comb_damp[WORK_TANK_COMBS][2];

    /* filters, one set per channel where the machine is stereo */
    svf_t  f1[2], f2[2], f3[2], f4[2];
    svf_t  bank[8][2];
    op_t   o1[2], o2[2], o3[2];
    dc_t   dcb[2];
    ap1_t  phase[6][2];
    hilbert_t hilb[2];

    /* modulation + envelopes */
    float  lfo_ph, lfo_ph2;
    float  env, env_stage;       /* Multimode Filter ADSR                 */
    float  gr;                   /* compressor gain reduction, dB, for UI */
    float  comp_env;
    float  grain_ph;             /* Chrono Pitch grain cursor             */
    float  warp_ph;              /* Frequency Warper carrier phase        */
    float  drift;                /* Warble random walk                    */
    float  hold[2];              /* Degrader sample-and-hold              */
    float  hold_ph;
    int    drop_ctr, frez_ctr, frez_len;
    float  frez_pos;
    uint32_t rng;

    int    last_machine;         /* reset state when the machine changes  */
} work_slot_t;

struct work {
    const host_api_v1_t *host;
    work_slot_cfg_t      cfg[WORK_SLOTS];
    work_lfo_cfg_t       lfo[WORK_LFOS];
    work_slot_t          slot[WORK_SLOTS];
    uint8_t              mix;          /* global wet/dry, 0..127 */

    /* Effective values, recomputed per block as
     *   base (cfg) -> parameter locks from the current step -> FX LFOs
     * which is Elektron's order: a lock sets the value, the LFO moves around
     * whatever the lock set. */
    uint8_t              eff[WORK_SLOTS][WORK_PARAMS];
    uint8_t              eff_machine[WORK_SLOTS];
    uint8_t              eff_mix;
    float                lfo_ph[WORK_LFOS];

    /* sequencer */
    work_step_t          step[WORK_STEPS];
    uint8_t              seq_on;
    uint8_t              seq_len;
    uint8_t              fill;
    double               seq_frame;     /* frames since the pattern restarted */
    int                  seq_pos;       /* step whose locks are currently held */
    int                  last_step;     /* for edge detection                  */
    int                  pass;          /* pattern repetitions, for A:B and 1ST*/
    int                  pre_result;    /* last conditional outcome, for PRE   */
    uint32_t             cond_rng;      /* probability conditions              */
    uint8_t              held[WORK_LOCKABLE];   /* values latched by the trig  */
    uint32_t             held_mask;

    /* transport */
    float                bpm;
    int                  clock_ticks;
    int                  clock_running;
    int                  note_pending;  /* note-on seen since last block */
    uint32_t             rng;           /* LFO random wave; kept separate from
                                         * the slots' so an LFO cannot shift a
                                         * Degrader's dropout sequence */
};

/* ---------------------------------------------------------- machine names */

static const char *MACHINE_NAME[WORK_FX_COUNT] = {
    "Bypass", "Chrono Pitch", "Comb +/- Filter", "Compressor", "Daisy Delay",
    "Degrader", "Dirtshaper", "Filter Folder", "Filterbank", "Frequency Warper",
    "Infinite Flanger", "Low-Pass Filter", "Multimode Filter", "Panoramic Chorus",
    "Phase 98", "Rumsklang Reverb", "Saturator Delay", "Steel Box Reverb",
    "Supervoid Reverb", "Warble"
};

/* Knob labels A-H per machine, matching the Tonverk manual's abbreviations.
 * An empty string means the machine leaves that knob unused. */
static const char *PARAM_NAME[WORK_FX_COUNT][WORK_PARAMS] = {
/* Bypass    */ {"","","","","","","",""},
/* Chrono    */ {"TUNE","WIN","FDBK","DEP","HPF","LPF","SPD","MIX"},
/* Comb      */ {"SPD","DEP","SPH","DTUN","FREQ","FDBK","LPF","MIX"},
/* Comp      */ {"THR","ATK","REL","MUP","RAT","SCS","SCF","MIX"},
/* Daisy     */ {"DRV","TIME","FDBK","WIDH","MOD","SKEW","FILT","MIX"},
/* Degrader  */ {"BR","OVER","SRR","DROP","RATE","DEP","FREZ","F.TIM"},
/* Dirt      */ {"DRV","RECT","HPF","LPF","NOIS","N.FRQ","N.RES","MIX"},
/* Folder    */ {"ILEV","HP","FOLD","OLEV","FREQ","RESO","TYPE","DIST"},
/* Filterbank — the manual calls these Gain A..H, but the band each one
 * controls is the useful thing to know, so the label IS the frequency. */
/* Filterbank*/ {"90Hz","122Hz","225Hz","418Hz","777Hz","1k4","2k7","4k+"},
/* Warper    */ {"SPD","DEP","SPH","LAG","SHFT","SPRD","SBND","MIX"},
/* Flanger   */ {"SPD","DEP","TUNE","FDBK","LPF","","",""},
/* LPF       */ {"SPD","DEP","SPH","LAG","FREQ","RESO","SPRD",""},
/* MMF       */ {"ATK","DEC","SUS","REL","FREQ","RESO","TYPE","ENV"},
/* Chorus    */ {"DEP","SPD","HPF","WDTH","MIX","","",""},
/* Phase98   */ {"SPD","DEP","SHP","LAG","FREQ","FDBK","STG","MIX"},
/* Rumsklang */ {"PRE","EARLY","DAMP","SIZE","LOWC","HIGHC","",""},
/* SatDelay  */ {"TIME","X","WID","FDBK","HPF","LPF","MIX",""},
/* SteelBox  */ {"SIZE","FDBK","BRIT","PRE","WDTH","DIFF","LOWC","MIX"},
/* Supervoid */ {"PRE","DEC","FREQ","GAIN","HPF","LPF","MIX",""},
/* Warble    */ {"SPEED","DEPTH","BASE","WIDTH","N.LEV","N.HPF","STEREO","MIX"}
};

const char *work_machine_name(int code) {
    if (code < 0 || code >= WORK_FX_COUNT) return "?";
    return MACHINE_NAME[code];
}

const char *work_param_name(int machine, int idx) {
    if (machine < 0 || machine >= WORK_FX_COUNT) return "";
    if (idx < 0 || idx >= WORK_PARAMS) return "";
    return PARAM_NAME[machine][idx];
}

static const char *COND_NAME[WORK_COND_COUNT] = {
    "OFF",
    "FILL", "!FIL",
    "PRE",  "!PRE",
    "1ST",  "!1ST",
    "25%",  "50%",  "75%",
    "1:2",  "2:2",
    "1:3",  "2:3",  "3:3",
    "1:4",  "2:4",  "3:4",  "4:4"
};

const char *work_cond_name(int cond) {
    if (cond < 0 || cond >= WORK_COND_COUNT) return "?";
    return COND_NAME[cond];
}

/* "1:TUNE" for slot 1 knob A, "2:MACH" for slot 2's machine select, "MIX" for
 * the global dry/wet. The label follows whichever machine the slot holds. */
int work_lock_label(work_t *w, int index, char *buf, int buf_len) {
    if (!w || !buf || buf_len <= 1) return -1;
    int cap = buf_len - 1;

    if (index < 0 || index >= WORK_LOCKABLE)
        return nclamp(snprintf(buf, (size_t)buf_len, "?"), cap);
    if (index == 18)
        return nclamp(snprintf(buf, (size_t)buf_len, "MIX"), cap);
    if (index >= 16)
        return nclamp(snprintf(buf, (size_t)buf_len, "%d:MACH", index - 15), cap);

    int slot = index / WORK_PARAMS;
    int knob = index % WORK_PARAMS;
    const char *nm = PARAM_NAME[w->cfg[slot].machine][knob];
    if (!nm[0]) nm = "-";
    return nclamp(snprintf(buf, (size_t)buf_len, "%d:%s", slot + 1, nm), cap);
}

/* Default parameter values per machine. Chosen so that loading a machine and
 * touching nothing produces something musical rather than silence or a
 * screaming feedback path. */
static const uint8_t PARAM_DEFAULT[WORK_FX_COUNT][WORK_PARAMS] = {
/* Bypass    */ {0,0,0,0,0,0,0,0},
/* Chrono    */ {76,48,32,0,0,127,32,64},
/* Comb      */ {32,0,0,64,80,80,96,64},
/* Comp      */ {80,24,64,64,32,0,64,127},
/* Daisy     */ {32,32,48,64,16,64,64,48},
/* Degrader  */ {127,32,127,0,32,0,0,32},
/* Dirt      */ {48,0,64,110,0,64,32,80},
/* Folder    */ {64,16,40,64,90,32,8,24},
/* Filterbank*/ {64,64,64,64,64,64,64,64},
/* Warper    */ {24,0,0,64,64,64,0,64},
/* Flanger   */ {70,72,32,64,96,0,0,0},
/* LPF       */ {32,0,0,64,96,32,64,0},
/* MMF       */ {0,48,64,40,90,40,0,64},
/* Chorus    */ {48,40,16,80,64,0,0,0},
/* Phase98   */ {36,64,64,72,56,48,64,64},
/* Rumsklang */ {16,48,64,72,24,16,0,0},
/* SatDelay  */ {32,0,64,56,16,96,48,0},
/* SteelBox  */ {64,72,72,16,80,64,24,48},
/* Supervoid */ {16,72,72,64,16,110,48,0},
/* Warble    */ {40,40,48,72,16,64,64,64}
};

/* ------------------------------------------------- transport / tempo helpers */

static float work_bpm(const work_t *w) {
    float b = w->bpm;
    if (!(b > 20.0f && b < 400.0f)) b = 120.0f;
    return b;
}

/* Frames in one 16th-note step at the current tempo */
static float step_frames(const work_t *w) {
    return (float)WORK_SR * 15.0f / work_bpm(w);
}

/* --------------------------------------------------------- 4. the machines */

/* Every machine has the same shape: it reads L/R, writes L/R, and may use the
 * slot's buffers. `p` is the post-LFO effective parameter array. */
typedef struct {
    work_t      *w;
    work_slot_t *s;
    const uint8_t *p;
    int          frames;
} mctx_t;

/* --- helper shared by the three reverbs -------------------------------------
 * One Schroeder tank: 8 damped parallel combs into 4 series allpasses per
 * channel. `size` scales the comb tunings, `fb` sets decay, `damp` the
 * in-loop low-pass. The three reverb machines differ in what they feed it and
 * what they do around it, which is what actually gives them their character. */
static void tank_run(work_slot_t *s, float inl, float inr, float size,
                     float fb, float damp, float *outl, float *outr) {
    float acc[2] = {0.0f, 0.0f};
    float in[2]  = {inl, inr};

    for (int c = 0; c < WORK_TANK_COMBS; ++c) {
        for (int ch = 0; ch < 2; ++ch) {
            int len = (int)((float)(COMB_TUNE[c] + (ch ? TANK_SPREAD : 0)) * size);
            len = iclamp(len, 32, WORK_TANK_LEN - 1);
            float *line = s->comb + (c * 2 + ch) * WORK_TANK_LEN;
            int    wpos = s->comb_w[c][ch] % len;
            float  y    = line[wpos];
            float  damped = op_lp(&s->comb_damp[c][ch], y, damp);
            line[wpos] = in[ch] + damped * fb;
            s->comb_w[c][ch] = (wpos + 1) % len;
            acc[ch] += y;
        }
    }

    acc[0] *= 0.125f; acc[1] *= 0.125f;

    for (int a = 0; a < WORK_TANK_APS; ++a) {
        for (int ch = 0; ch < 2; ++ch) {
            int len = iclamp(AP_TUNE[a] + (ch ? TANK_SPREAD : 0), 32, WORK_AP_LEN - 1);
            float *line = s->ap + (a * 2 + ch) * WORK_AP_LEN;
            int    wpos = s->ap_w[a][ch] % len;
            float  buf  = line[wpos];
            float  y    = buf - acc[ch];
            line[wpos]  = acc[ch] + buf * 0.5f;
            s->ap_w[a][ch] = (wpos + 1) % len;
            acc[ch] = y;
        }
    }

    *outl = acc[0];
    *outr = acc[1];
}

/* Push a frame into the predelay line and read it back `frames` behind */
static void predelay(work_slot_t *s, float l, float r, float back,
                     float *ol, float *or_) {
    s->pre[s->pw * 2]     = l;
    s->pre[s->pw * 2 + 1] = r;
    back = fclampf(back, 1.0f, (float)(WORK_PRE_LEN - 2));
    *ol = dl_read(s->pre, WORK_PRE_LEN, s->pw, back, 0);
    *or_ = dl_read(s->pre, WORK_PRE_LEN, s->pw, back, 1);
    s->pw = (s->pw + 1) % WORK_PRE_LEN;
}

/* --- A.3.2 Chrono Pitch ---------------------------------------------------
 * Granular pitch shifter: the read head drifts against the write head at
 * (1 - rate), and two taps a half-window apart are crossfaded with a raised
 * sine so the wrap is inaudible. Feedback re-injects the shifted signal, so
 * held notes climb or fall in steps. */
static void m_chrono(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float semis = pbi(p[0]) * 24.0f;
    float lfo   = sinf(s->lfo_ph * 2.0f * (float)M_PI);
    semis += lfo * p01(p[3]) * 12.0f;
    s->lfo_ph += pexp(p[6], 0.02f, 12.0f) / (float)WORK_SR;
    if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;

    float win  = fclampf(p01(p[1]) * 0.19f + 0.01f, 0.005f, 0.25f) * (float)WORK_SR;
    float rate = powf(2.0f, semis / 12.0f);
    float fb   = p01(p[2]) * 0.92f;

    s->grain_ph += (1.0f - rate);
    while (s->grain_ph >= win)  s->grain_ph -= win;
    while (s->grain_ph <  0.0f) s->grain_ph += win;

    float a  = s->grain_ph;
    float b  = a + win * 0.5f; if (b >= win) b -= win;
    float ga = sinf((float)M_PI * a / win);
    float gb = sinf((float)M_PI * b / win);

    float wl = dl_read(s->dl, WORK_DLY_LEN, s->dw, a + 2.0f, 0) * ga
             + dl_read(s->dl, WORK_DLY_LEN, s->dw, b + 2.0f, 0) * gb;
    float wr = dl_read(s->dl, WORK_DLY_LEN, s->dw, a + 2.0f, 1) * ga
             + dl_read(s->dl, WORK_DLY_LEN, s->dw, b + 2.0f, 1) * gb;

    svf_co_t hc, lc;
    svf_coeffs(&hc, pexp(p[4], 20.0f, 4000.0f), 0.707f);
    svf_coeffs(&lc, pexp(p[5], 200.0f, 18000.0f), 0.707f);
    wl = svf_lp(&s->f2[0], &lc, svf_hp(&s->f1[0], &hc, wl));
    wr = svf_lp(&s->f2[1], &lc, svf_hp(&s->f1[1], &hc, wr));

    s->dl[s->dw * 2]     = *l + wl * fb;
    s->dl[s->dw * 2 + 1] = *r + wr * fb;
    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + wl * mix;
    *r = *r * (1.0f - mix) + wr * mix;
}

/* --- A.3.3 Comb +/- Filter ------------------------------------------------
 * FREQ is bipolar: it tunes the comb, and its sign selects positive feedback
 * (string-like) or negative (hollow, tube-like), per the manual. */
static void m_comb(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    /* Tempo-synced triangle LFO, measured in sequencer steps */
    float steps = pexp(p[0], 32.0f, 0.25f);
    float inc   = 1.0f / fmaxf(steps * step_frames(m->w), 1.0f);
    s->lfo_ph += inc;
    if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;
    float tri = 4.0f * fabsf(s->lfo_ph - 0.5f) - 1.0f;

    float bi   = pbi(p[4]);
    float hz   = 20.0f * powf(100.0f, fabsf(bi));
    float sign = bi < 0.0f ? -1.0f : 1.0f;
    float mod  = 1.0f + tri * p01(p[1]) * 0.5f;
    float det  = pbi(p[3]) * 0.03f;

    float dL = fclampf((float)WORK_SR / (hz * mod * (1.0f + det)), 2.0f, 8000.0f);
    float dR = fclampf((float)WORK_SR / (hz * mod * (1.0f - det)), 2.0f, 8000.0f);

    float fb = p01(p[5]) * 0.97f * sign;

    svf_co_t lc;
    svf_coeffs(&lc, pexp(p[6], 200.0f, 18000.0f), 0.707f);

    float tl = dl_read(s->dl, WORK_DLY_LEN, s->dw, dL, 0);
    float tr = dl_read(s->dl, WORK_DLY_LEN, s->dw, dR, 1);
    tl = svf_lp(&s->f1[0], &lc, tl);
    tr = svf_lp(&s->f1[1], &lc, tr);

    s->dl[s->dw * 2]     = softclip(*l + tl * fb);
    s->dl[s->dw * 2 + 1] = softclip(*r + tr * fb);
    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + tl * mix;
    *r = *r * (1.0f - mix) + tr * mix;
}

/* --- A.3.4 Compressor -----------------------------------------------------
 * Feed-forward peak compressor with a log-domain gain computer. The eight
 * documented ratios are exposed exactly; SCF is bipolar, low-pass to the left
 * (pumping) and high-pass to the right (no pumping). */
static const float COMP_RATIO[8] = {1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 16.0f, 20.0f};

static void m_comp(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float thr_db = -60.0f + p01(p[0]) * 60.0f;
    float atk    = op_a(pexp(p[1], 800.0f, 3.0f));
    float rel    = op_a(pexp(p[2], 60.0f, 0.4f));
    float mup    = powf(10.0f, (p01(p[3]) * 24.0f) / 20.0f);
    float ratio  = COMP_RATIO[iclamp(p[4] * 8 / 128, 0, 7)];

    /* Sidechain source. An insert slot can only see what reaches it, so the
     * reachable subset is: its own input, or one side of it. */
    float sc;
    switch (iclamp(p[5] * WORK_SC_COUNT / 128, 0, WORK_SC_COUNT - 1)) {
        case WORK_SC_SIDE_L: sc = *l; break;
        case WORK_SC_SIDE_R: sc = *r; break;
        default:             sc = (*l + *r) * 0.5f; break;
    }

    /* SCF is bipolar: left of centre is a low-pass on the sidechain (the
     * compressor hears mostly bass, giving the pumping sound), right is a
     * high-pass (it stops reacting to bass, avoiding pumping). Cutoff has to
     * move AWAY from centre in both directions. */
    float scf = pbi(p[6]);
    if (scf < -0.02f) {
        svf_co_t c; svf_coeffs(&c, 60.0f * powf(66.0f, 1.0f + scf), 0.707f);
        sc = svf_lp(&s->f3[0], &c, sc);
    } else if (scf > 0.02f) {
        svf_co_t c; svf_coeffs(&c, 40.0f * powf(75.0f, scf), 0.707f);
        sc = svf_hp(&s->f3[0], &c, sc);
    }

    float lvl = fabsf(sc);
    s->comp_env += (lvl > s->comp_env ? atk : rel) * (lvl - s->comp_env);

    float env_db = 20.0f * log10f(fmaxf(s->comp_env, 1e-6f));
    float over   = env_db - thr_db;
    float gr_db  = over > 0.0f ? over * (1.0f / ratio - 1.0f) : 0.0f;
    s->gr = gr_db;

    float g = powf(10.0f, gr_db / 20.0f) * mup;
    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + (*l * g) * mix;
    *r = *r * (1.0f - mix) + (*r * g) * mix;
}

/* --- A.3.5 Daisy Delay ----------------------------------------------------
 * Drive into the line, tempo-synced base time, SKEW pulling the two channels
 * into a rhythmic relationship, and a tilt filter that low-passes to the left
 * and high-passes to the right. */
static void m_daisy(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float drv  = 1.0f + p01(p[0]) * 12.0f;
    float base = fclampf(step_frames(m->w) * (0.25f + p01(p[1]) * 7.75f),
                         64.0f, (float)(WORK_DLY_LEN - 4));
    float fb   = p01(p[2]) * 0.95f;
    float wid  = p01(p[3]);
    float skew = pbi(p[5]);

    s->lfo_ph += 0.6f / (float)WORK_SR;
    if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;
    float mod = sinf(s->lfo_ph * 2.0f * (float)M_PI) * p01(p[4]) * 0.02f;

    float dL = fclampf(base * (1.0f + mod) * (1.0f - skew * 0.4f), 2.0f, (float)(WORK_DLY_LEN - 4));
    float dR = fclampf(base * (1.0f - mod) * (1.0f + skew * 0.4f), 2.0f, (float)(WORK_DLY_LEN - 4));

    float tl = dl_read(s->dl, WORK_DLY_LEN, s->dw, dL, 0);
    float tr = dl_read(s->dl, WORK_DLY_LEN, s->dw, dR, 1);

    /* Tilt: crossfade a low-passed and a high-passed copy */
    float tilt = pbi(p[6]);
    svf_co_t tc; svf_coeffs(&tc, 900.0f, 0.707f);
    float ll, hl, lr, hr;
    svf_run(&s->f1[0], &tc, tl, &ll, NULL, &hl);
    svf_run(&s->f1[1], &tc, tr, &lr, NULL, &hr);
    float wl = tilt < 0.0f ? tl + (ll - tl) * (-tilt) : tl + (hl - tl) * tilt;
    float wr = tilt < 0.0f ? tr + (lr - tr) * (-tilt) : tr + (hr - tr) * tilt;

    /* Width applies to the input and the feedback before they are mixed */
    float inl = softclip(*l * drv) * (1.0f - wid * 0.5f) + softclip(*r * drv) * (wid * 0.5f);
    float inr = softclip(*r * drv) * (1.0f - wid * 0.5f) + softclip(*l * drv) * (wid * 0.5f);

    s->dl[s->dw * 2]     = inl + wr * fb * wid + wl * fb * (1.0f - wid);
    s->dl[s->dw * 2 + 1] = inr + wl * fb * wid + wr * fb * (1.0f - wid);
    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + wl * mix;
    *r = *r * (1.0f - mix) + wr * mix;
}

/* --- A.3.6 Degrader -------------------------------------------------------
 * The lo-fi collection: bit reduction, overdrive, sample-rate redux, random
 * drop-outs, sine ring modulation, and random freezes whose length is either
 * a step division or, at the RAND setting, re-rolled each time. */
static void m_degrader(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float x[2] = {*l, *r};

    /* Overdrive */
    float over = 1.0f + p01(p[1]) * 24.0f;
    x[0] = softclip(x[0] * over);
    x[1] = softclip(x[1] * over);

    /* Sample rate reduction (16 bits down to 1 as BR falls) */
    float srr = pexp((uint8_t)(127 - p[2]), 1.0f, 220.0f);
    s->hold_ph += 1.0f / srr;
    if (s->hold_ph >= 1.0f) {
        s->hold_ph -= 1.0f;
        s->hold[0] = x[0];
        s->hold[1] = x[1];
    }
    x[0] = s->hold[0];
    x[1] = s->hold[1];

    /* Bit reduction: BR at 127 is 16 bits, at 0 is 1 bit */
    float bits  = 1.0f + p01(p[0]) * 15.0f;
    float steps = powf(2.0f, bits);
    x[0] = floorf(x[0] * steps + 0.5f) / steps;
    x[1] = floorf(x[1] * steps + 0.5f) / steps;

    /* Ring modulation */
    float dep = p01(p[5]);
    if (dep > 0.001f) {
        s->warp_ph += pexp(p[4], 0.5f, 4000.0f) / (float)WORK_SR;
        if (s->warp_ph >= 1.0f) s->warp_ph -= 1.0f;
        float c = sinf(s->warp_ph * 2.0f * (float)M_PI);
        x[0] = x[0] * (1.0f - dep) + x[0] * c * dep;
        x[1] = x[1] * (1.0f - dep) + x[1] * c * dep;
    }

    /* Random drop-outs: higher DROP means more of them, and longer */
    float drop = p01(p[3]);
    if (s->drop_ctr > 0) {
        s->drop_ctr--;
        x[0] = x[1] = 0.0f;
    } else if (drop > 0.001f && rnd_01(&s->rng) < drop * 0.002f) {
        s->drop_ctr = (int)(drop * 3000.0f * rnd_01(&s->rng)) + 32;
    }

    /* Freeze: capture a slice and repeat it */
    float frez = p01(p[6]);
    s->dl[s->dw * 2]     = x[0];
    s->dl[s->dw * 2 + 1] = x[1];

    if (s->frez_ctr > 0) {
        /* Repeat the window captured just before the freeze started. The write
         * head keeps advancing underneath us, so to replay a FIXED span we
         * must read progressively further back: k frames after the trigger,
         * the sample we want sits (k + len - k%len) frames behind. */
        int k   = (int)s->frez_pos;
        int len = s->frez_len;
        float back = (float)(k + len - (k % len));
        x[0] = dl_read(s->dl, WORK_DLY_LEN, s->dw, back, 0);
        x[1] = dl_read(s->dl, WORK_DLY_LEN, s->dw, back, 1);
        s->frez_pos += 1.0f;
        s->frez_ctr--;
    } else if (frez > 0.001f && rnd_01(&s->rng) < frez * 0.0015f) {
        /* F.TIM: RAND at the top of the range, otherwise a step division */
        float len;
        if (p[7] >= 120) len = step_frames(m->w) * (0.125f + rnd_01(&s->rng) * 1.875f);
        else             len = step_frames(m->w) * (0.125f * powf(2.0f, floorf(p01(p[7]) * 4.0f)));
        s->frez_len = iclamp((int)len, 64, WORK_DLY_LEN / 8);
        s->frez_ctr = s->frez_len * 4;
        s->frez_pos = 0.0f;
    }

    s->dw = (s->dw + 1) % WORK_DLY_LEN;
    *l = x[0];
    *r = x[1];
}

/* --- A.3.7 Dirtshaper -----------------------------------------------------
 * HPF is bipolar and selects WHERE the filter sits: to the left it filters
 * before the distortion, to the right after the rectifier. */
static void m_dirt(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float drv  = 1.0f + p01(p[0]) * 40.0f;
    float rect = p01(p[1]);
    float hp   = pbi(p[2]);

    /* The further HPF is from centre, the higher the corner — in whichever
     * position (pre-distortion or post-rectifier) the sign selects. */
    svf_co_t hc, lc, nc;
    svf_coeffs(&hc, 20.0f * powf(150.0f, fabsf(hp)), 0.707f);
    svf_coeffs(&lc, pexp(p[3], 200.0f, 18000.0f), 0.707f);
    svf_coeffs(&nc, pexp(p[5], 100.0f, 12000.0f), 0.5f + p01(p[6]) * 12.0f);

    float x[2] = {*l, *r};
    for (int ch = 0; ch < 2; ++ch) {
        float v = x[ch];
        if (hp < -0.02f) v = svf_hp(&s->f1[ch], &hc, v);   /* pre-distortion  */

        float d = softclip(v * drv);
        float w = fabsf(d) * 2.0f - 1.0f;                  /* full-wave rectify */
        d = d * (1.0f - rect) + w * rect;
        d = dc_run(&s->dcb[ch], d);

        if (hp > 0.02f) d = svf_hp(&s->f2[ch], &hc, d);    /* post-rectifier  */

        /* Noise ring-modulated with the distorted signal */
        float nois = p01(p[4]);
        if (nois > 0.001f) {
            float n = svf_bp(&s->f3[ch], &nc, rnd_bi(&s->rng));
            d += d * n * nois * 3.0f;
        }

        x[ch] = svf_lp(&s->f4[ch], &lc, d);
    }

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + x[0] * mix;
    *r = *r * (1.0f - mix) + x[1] * mix;
}

/* --- A.3.8 Filter Folder --------------------------------------------------
 * The manual gives the chain explicitly:
 *   Input level > High-pass > Wavefolder > Multimode filter > Dist > Output */
static void m_folder(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float ilev = powf(10.0f, (pbi(p[0]) * 24.0f) / 20.0f);
    float fold = 1.0f + p01(p[2]) * 9.0f;
    float olev = powf(10.0f, (pbi(p[3]) * 24.0f) / 20.0f);
    float dist = p01(p[7]);
    int   type = iclamp(p[6] * 8 / 128, 0, 7);

    svf_co_t hc, fc;
    svf_coeffs(&hc, pexp(p[1], 20.0f, 4000.0f), 0.707f);
    svf_coeffs(&fc, pexp(p[4], 30.0f, 16000.0f), 0.5f + p01(p[5]) * 12.0f);

    float x[2] = {*l, *r};
    for (int ch = 0; ch < 2; ++ch) {
        float v = svf_hp(&s->f1[ch], &hc, x[ch] * ilev);

        /* Triangle wavefolder — sin() folds smoothly and never blows up */
        v = sinf(fclampf(v * fold, -40.0f, 40.0f) * 1.5707963f);
        v = dc_run(&s->dcb[ch], v);

        float lp, bp, hp2;
        svf_run(&s->f2[ch], &fc, v, &lp, &bp, &hp2);
        float lp2 = lp;
        svf_run(&s->f3[ch], &fc, lp, &lp, NULL, NULL);   /* second pole pair */

        switch (type) {
            case 0: v = lp;              break;  /* LP4    */
            case 1: v = lp2;             break;  /* LP2    */
            case 2: v = bp;              break;  /* BP2    */
            case 3: v = hp2;             break;  /* HP2    */
            case 4: v = lp2 + hp2 * 0.5f; break; /* LP notch */
            case 5: v = lp2 + hp2;       break;  /* Notch 1 */
            case 6: v = lp2 - bp + hp2;  break;  /* Notch 2 */
            default: /* Off */           break;
        }

        if (dist > 0.001f) v = softclip(v * (1.0f + dist * 20.0f)) * (1.0f / (1.0f + dist));
        x[ch] = v * olev;
    }

    *l = x[0];
    *r = x[1];
}

/* --- A.3.9 Filterbank -----------------------------------------------------
 * Eight fixed bands at the documented frequencies: a low-pass, six band-pass,
 * and a high-pass, each with its own gain. */
static const float BANK_HZ[8] = {90.0f, 121.9f, 224.8f, 418.0f, 777.4f, 1445.8f, 2688.6f, 4000.0f};

static void m_fbank(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    svf_co_t c[8];
    for (int b = 0; b < 8; ++b) svf_coeffs(&c[b], BANK_HZ[b], b == 0 || b == 7 ? 0.707f : 2.4f);

    float x[2] = {*l, *r};
    for (int ch = 0; ch < 2; ++ch) {
        float sum = 0.0f;
        for (int b = 0; b < 8; ++b) {
            float g = p01(p[b]) * 2.0f;
            float o;
            if (b == 0)      o = svf_lp(&s->bank[b][ch], &c[b], x[ch]);
            else if (b == 7) o = svf_hp(&s->bank[b][ch], &c[b], x[ch]);
            else             o = svf_bp(&s->bank[b][ch], &c[b], x[ch]);
            sum += o * g;
        }
        x[ch] = sum;
    }

    *l = x[0];
    *r = x[1];
}

/* --- A.3.10 Frequency Warper ----------------------------------------------
 * True single-sideband frequency shifting via a Hilbert pair. SBND blends the
 * up-shifted and down-shifted outputs, which is the "alternate output signal
 * with different characteristics" the manual describes. */
static void m_warper(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    s->lfo_ph += pexp(p[0], 0.02f, 20.0f) / (float)WORK_SR;
    if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;

    float lag  = p01(p[3]);
    float shift = pbi(p[4]) * 500.0f;
    float sprd  = pbi(p[5]) * 40.0f;
    float sbnd  = p01(p[6]);

    float x[2] = {*l, *r};
    for (int ch = 0; ch < 2; ++ch) {
        float lph = s->lfo_ph + (ch ? lag * 0.5f : 0.0f);
        if (lph >= 1.0f) lph -= 1.0f;
        float hz = shift + sinf(lph * 2.0f * (float)M_PI) * p01(p[1]) * 500.0f
                 + (ch ? sprd : -sprd);

        float i_s, q_s;
        hilbert_run(&s->hilb[ch], x[ch], &i_s, &q_s);

        /* SPRD gives the two channels different shift distances, so each
         * needs its own carrier phase — sharing one would collapse the
         * spread back to mono. */
        float *carrier = ch ? &s->lfo_ph2 : &s->warp_ph;
        float cw = cosf(*carrier * 2.0f * (float)M_PI);
        float sw = sinf(*carrier * 2.0f * (float)M_PI);

        float up   = i_s * cw - q_s * sw;
        float down = i_s * cw + q_s * sw;
        x[ch] = up * (1.0f - sbnd) + down * sbnd;

        *carrier += hz / (float)WORK_SR;
        if (*carrier >= 1.0f) *carrier -= 1.0f;
        if (*carrier <  0.0f) *carrier += 1.0f;
    }

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + x[0] * mix;
    *r = *r * (1.0f - mix) + x[1] * mix;
}

/* --- A.3.11 Infinite Flanger ----------------------------------------------
 * Barber-pole motion: three delay taps whose lengths ramp continuously and
 * are crossfaded by their own position, so the pitch appears to rise or fall
 * forever without ever turning around. SPD is bipolar; 0 holds still. */
static void m_flanger(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    /* Cubed so the centre of the knob is a slow crawl and the ends are fast,
     * with SPD 0 (centre) holding the delay perfectly still. */
    float spd = pbi(p[0]);
    s->lfo_ph += spd * spd * spd * 4.0f / (float)WORK_SR;
    while (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;
    while (s->lfo_ph <  0.0f) s->lfo_ph += 1.0f;

    float dep  = p01(p[1]);
    float base = 1.0f + p01(p[2]) * 400.0f;   /* TUNE: flange -> comb -> echo */
    float fb   = p01(p[3]) * 0.9f;

    svf_co_t lc;
    svf_coeffs(&lc, pexp(p[4], 200.0f, 16000.0f), 0.707f);

    float wl = 0.0f, wr = 0.0f;
    for (int k = 0; k < 3; ++k) {
        float ph = s->lfo_ph + (float)k / 3.0f;
        if (ph >= 1.0f) ph -= 1.0f;
        float g = 0.5f - 0.5f * cosf(ph * 2.0f * (float)M_PI);  /* Hann fade */
        float d = base * (1.0f + ph * 7.0f) * (0.2f + dep);
        wl += dl_read(s->dl, WORK_DLY_LEN, s->dw, fclampf(d, 2.0f, 20000.0f), 0) * g;
        wr += dl_read(s->dl, WORK_DLY_LEN, s->dw, fclampf(d * 1.02f, 2.0f, 20000.0f), 1) * g;
    }
    wl *= 0.6f; wr *= 0.6f;

    float fl = svf_lp(&s->f1[0], &lc, wl);
    float fr = svf_lp(&s->f1[1], &lc, wr);

    s->dl[s->dw * 2]     = softclip(*l + fl * fb);
    s->dl[s->dw * 2 + 1] = softclip(*r + fr * fb);
    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    /* Flanging is an inherently mixed effect; no MIX knob on this machine */
    *l = (*l + wl) * 0.6f;
    *r = (*r + wr) * 0.6f;
}

/* --- A.3.12 Low-Pass Filter -----------------------------------------------
 * Four poles, 24 dB/oct, built from two cascaded SVF low-pass sections with
 * resonance taken around the pair. SPRD offsets cutoff between channels. */
static void m_lpf(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float steps = pexp(p[0], 32.0f, 0.25f);
    s->lfo_ph += 1.0f / fmaxf(steps * step_frames(m->w), 1.0f);
    if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;

    float lag  = p01(p[3]);
    float base = p01(p[4]);
    float reso = 0.5f + p01(p[5]) * 12.0f;
    float sprd = pbi(p[6]);

    float x[2] = {*l, *r};
    for (int ch = 0; ch < 2; ++ch) {
        float ph = s->lfo_ph + (ch ? lag * 0.5f : 0.0f);
        if (ph >= 1.0f) ph -= 1.0f;
        float lfo = 4.0f * fabsf(ph - 0.5f) - 1.0f;
        float f = fclampf(base + lfo * p01(p[1]) * 0.5f + (ch ? sprd : -sprd) * 0.15f,
                          0.0f, 1.0f);

        svf_co_t c;
        svf_coeffs(&c, 20.0f * powf(900.0f, f), reso);
        float v = svf_lp(&s->f1[ch], &c, x[ch]);
        x[ch]   = svf_lp(&s->f2[ch], &c, v);
    }

    *l = x[0];
    *r = x[1];
}

/* --- A.3.13 Multimode Filter ----------------------------------------------
 * TYPE morphs low-pass -> band-pass -> high-pass. The ADSR envelope tracks
 * note-on/off from the host and modulates cutoff by a bipolar ENV depth. */
static void m_mmf(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    /* Envelope. env_stage: 0 idle, 1 attack, 2 decay, 3 sustain, 4 release */
    float atk = op_a(pexp(p[0], 1000.0f, 1.0f));
    float dec = op_a(pexp(p[1], 1000.0f, 0.5f));
    float rel = op_a(pexp(p[3], 1000.0f, 0.3f));
    float sus = p01(p[2]);

    if (s->env_stage == 1.0f) {
        s->env += atk * (1.05f - s->env);
        if (s->env >= 1.0f) { s->env = 1.0f; s->env_stage = 2.0f; }
    } else if (s->env_stage == 2.0f) {
        s->env += dec * (sus - s->env);
        if (fabsf(s->env - sus) < 0.001f) s->env_stage = 3.0f;
    } else if (s->env_stage == 4.0f || s->env_stage == 0.0f) {
        s->env += rel * (0.0f - s->env);
    }

    float type = p01(p[6]);
    float env  = pbi(p[7]) * s->env;
    float f    = fclampf(p01(p[4]) + env * 0.6f, 0.0f, 1.0f);

    svf_co_t c;
    svf_coeffs(&c, 20.0f * powf(900.0f, f), 0.5f + p01(p[5]) * 12.0f);

    float x[2] = {*l, *r};
    for (int ch = 0; ch < 2; ++ch) {
        float lp, bp, hp;
        svf_run(&s->f1[ch], &c, x[ch], &lp, &bp, &hp);
        /* Morph: 0 = LP, 0.5 = BP, 1 = HP, crossfaded pairwise */
        if (type < 0.5f) {
            float t = type * 2.0f;
            x[ch] = lp * (1.0f - t) + bp * t;
        } else {
            float t = (type - 0.5f) * 2.0f;
            x[ch] = bp * (1.0f - t) + hp * t;
        }
    }

    *l = x[0];
    *r = x[1];
}

/* --- A.3.14 Panoramic Chorus ---------------------------------------------- */
static void m_chorus(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    s->lfo_ph += pexp(p[1], 0.05f, 8.0f) / (float)WORK_SR;
    if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;

    float dep = p01(p[0]) * 8.0f;      /* ms of sweep  */
    float wdt = p01(p[3]);

    svf_co_t hc;
    svf_coeffs(&hc, pexp(p[2], 20.0f, 2000.0f), 0.707f);

    float il = svf_hp(&s->f1[0], &hc, *l);
    float ir = svf_hp(&s->f1[1], &hc, *r);

    s->dl[s->dw * 2]     = il;
    s->dl[s->dw * 2 + 1] = ir;

    float wl = 0.0f, wr = 0.0f;
    for (int k = 0; k < 3; ++k) {
        float ph = s->lfo_ph + (float)k / 3.0f;
        if (ph >= 1.0f) ph -= 1.0f;
        float mod = sinf(ph * 2.0f * (float)M_PI);
        float base = (8.0f + (float)k * 4.0f) * 0.001f * (float)WORK_SR;
        wl += dl_read(s->dl, WORK_DLY_LEN, s->dw, base + mod * dep * 0.001f * WORK_SR, 0);
        wr += dl_read(s->dl, WORK_DLY_LEN, s->dw, base - mod * dep * 0.001f * WORK_SR, 1);
    }
    wl *= 0.33f; wr *= 0.33f;
    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    /* Width: push the two wet voices apart around their own mid */
    float mid  = (wl + wr) * 0.5f;
    float side = (wl - wr) * 0.5f * (0.4f + wdt * 1.6f);
    wl = mid + side;
    wr = mid - side;

    float mix = p01(p[4]);
    *l = *l * (1.0f - mix) + wl * mix;
    *r = *r * (1.0f - mix) + wr * mix;
}

/* --- A.3.15 Phase 98 ------------------------------------------------------
 * Six allpass stages; STG blends the tap after four with the tap after six.
 * SHP morphs the LFO from a descending ramp through triangle to ascending. */
static void m_phase98(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    s->lfo_ph += pexp(p[0], 0.02f, 12.0f) / (float)WORK_SR;
    if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;

    float shp = p01(p[2]);
    float lag = p01(p[3]);
    float fb  = p01(p[5]) * 0.95f;
    float stg = p01(p[6]);

    float x[2] = {*l, *r};
    for (int ch = 0; ch < 2; ++ch) {
        float ph = s->lfo_ph + (ch ? lag * 0.5f : 0.0f);
        if (ph >= 1.0f) ph -= 1.0f;

        /* ramp-down (shp=0) -> triangle (0.5) -> ramp-up (1) */
        float down = 1.0f - ph;
        float up   = ph;
        float tri  = 1.0f - 2.0f * fabsf(ph - 0.5f);
        float lfo  = shp < 0.5f ? down + (tri - down) * (shp * 2.0f)
                                : tri  + (up  - tri)  * ((shp - 0.5f) * 2.0f);

        float f = fclampf(p01(p[4]) + (lfo - 0.5f) * p01(p[1]), 0.0f, 1.0f);
        float fc = 100.0f * powf(80.0f, f);
        float a  = (1.0f - tanf((float)M_PI * fc / (float)WORK_SR))
                 / (1.0f + tanf((float)M_PI * fc / (float)WORK_SR));

        float v = x[ch] + s->o1[ch].z * fb;
        float t4 = 0.0f;
        for (int k = 0; k < 6; ++k) {
            v = ap1_run(&s->phase[k][ch], v, a);
            if (k == 3) t4 = v;
        }
        s->o1[ch].z = v;
        x[ch] = t4 * (1.0f - stg) + v * stg;
    }

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + x[0] * mix;
    *r = *r * (1.0f - mix) + x[1] * mix;
}

/* --- A.3.16 Rumsklang Reverb ----------------------------------------------
 * Large-space voicing: an explicit early-reflection tap bank in front of the
 * tank, with lowcut/highcut shaping the tail. */
static const float ER_TAP[6]  = {0.0043f, 0.0215f, 0.0268f, 0.0362f, 0.0498f, 0.0677f};
static const float ER_GAIN[6] = {0.841f, 0.504f, 0.491f, 0.379f, 0.320f, 0.250f};

static void m_rumsklang(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float pl, pr;
    predelay(s, *l, *r, 1.0f + p01(p[0]) * (float)(WORK_PRE_LEN - 8), &pl, &pr);

    /* Early reflections tapped off the same predelay line */
    float el = 0.0f, er = 0.0f;
    float early = p01(p[1]);
    if (early > 0.001f) {
        for (int k = 0; k < 6; ++k) {
            float d = ER_TAP[k] * (float)WORK_SR;
            el += dl_read(s->pre, WORK_PRE_LEN, s->pw, d, 0) * ER_GAIN[k];
            er += dl_read(s->pre, WORK_PRE_LEN, s->pw, d * 1.07f, 1) * ER_GAIN[k];
        }
        el *= early * 0.4f;
        er *= early * 0.4f;
    }

    float size = 0.5f + p01(p[3]) * 1.5f;
    float fb   = 0.70f + p01(p[3]) * 0.28f;
    float damp = op_a(pexp((uint8_t)(127 - p[2]), 400.0f, 16000.0f));

    float tl, tr;
    tank_run(s, pl + el * 0.5f, pr + er * 0.5f, size, fb, damp, &tl, &tr);

    svf_co_t hc, lc;
    svf_coeffs(&hc, pexp(p[4], 20.0f, 1200.0f), 0.707f);
    svf_coeffs(&lc, pexp((uint8_t)(127 - p[5]), 500.0f, 18000.0f), 0.707f);

    tl = svf_lp(&s->f2[0], &lc, svf_hp(&s->f1[0], &hc, tl + el));
    tr = svf_lp(&s->f2[1], &lc, svf_hp(&s->f1[1], &hc, tr + er));

    *l = tl;
    *r = tr;
}

/* --- A.3.17 Saturator Delay -----------------------------------------------
 * TIME is measured in 128th notes, using the documented divide table. The
 * saturation lives in the feedback path, so repeats degrade as they decay. */
static const float SATDLY_DIV[16] = {
    1.0f, 2.0f, 2.67f, 3.0f, 4.0f, 5.33f, 6.0f, 8.0f,
    10.67f, 12.0f, 16.0f, 21.33f, 24.0f, 32.0f, 64.0f, 128.0f
};

static void m_satdelay(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    /* One 128th note is a 16th-note step divided by 8 */
    float unit = step_frames(m->w) / 8.0f;
    float d    = fclampf(unit * SATDLY_DIV[iclamp(p[0] * 16 / 128, 0, 15)],
                         2.0f, (float)(WORK_DLY_LEN - 4));

    int   pingpong = p[1] >= 64;
    float wid = pbi(p[2]);
    float fb  = p01(p[3]) * 0.95f;

    svf_co_t hc, lc;
    svf_coeffs(&hc, pexp(p[4], 20.0f, 4000.0f), 0.707f);
    svf_coeffs(&lc, pexp(p[5], 200.0f, 18000.0f), 0.707f);

    float tl = dl_read(s->dl, WORK_DLY_LEN, s->dw, d, 0);
    float tr = dl_read(s->dl, WORK_DLY_LEN, s->dw, d, 1);
    tl = svf_lp(&s->f2[0], &lc, svf_hp(&s->f1[0], &hc, tl));
    tr = svf_lp(&s->f2[1], &lc, svf_hp(&s->f1[1], &hc, tr));

    float sl = softclip(tl * 1.4f) * 0.8f;
    float sr = softclip(tr * 1.4f) * 0.8f;

    float in = (*l + *r) * 0.5f;
    if (pingpong) {
        s->dl[s->dw * 2]     = in + sr * fb;
        s->dl[s->dw * 2 + 1] = in + sl * fb;
    } else {
        s->dl[s->dw * 2]     = *l + sl * fb;
        s->dl[s->dw * 2 + 1] = *r + sr * fb;
    }
    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    /* WID is bipolar: it places the repeats, or sets the ping-pong spread */
    float gl = 1.0f, gr = 1.0f;
    if (wid < 0.0f) gr = 1.0f + wid; else gl = 1.0f - wid;

    float mix = p01(p[6]);
    *l = *l * (1.0f - mix) + tl * gl * mix;
    *r = *r * (1.0f - mix) + tr * gr * mix;
}

/* --- A.3.18 Steel Box Reverb ----------------------------------------------
 * Plate character: heavy input diffusion, a bright tank, and deliberately
 * wide parameter ranges as the manual advertises. */
static void m_steelbox(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float pl, pr;
    predelay(s, *l, *r, 1.0f + p01(p[3]) * (float)(WORK_PRE_LEN - 8), &pl, &pr);

    /* Input diffusion: two allpasses per channel, depth set by DIFF */
    float diff = p01(p[5]) * 0.7f;
    for (int ch = 0; ch < 2; ++ch) {
        float *v = ch ? &pr : &pl;
        *v = ap1_run(&s->phase[0][ch], *v, diff);
        *v = ap1_run(&s->phase[1][ch], *v, diff * 0.8f);
    }

    float size = 0.35f + p01(p[0]) * 1.25f;
    float fb   = 0.55f + p01(p[1]) * 0.44f;
    float damp = op_a(pexp(p[2], 600.0f, 18000.0f));

    float tl, tr;
    tank_run(s, pl, pr, size, fb, damp, &tl, &tr);

    svf_co_t hc;
    svf_coeffs(&hc, pexp(p[6], 20.0f, 1500.0f), 0.707f);
    tl = svf_hp(&s->f1[0], &hc, tl);
    tr = svf_hp(&s->f1[1], &hc, tr);

    float wdt  = p01(p[4]);
    float mid  = (tl + tr) * 0.5f;
    float side = (tl - tr) * 0.5f * (0.2f + wdt * 1.8f);
    tl = mid + side;
    tr = mid - side;

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + tl * mix;
    *r = *r * (1.0f - mix) + tr * mix;
}

/* --- A.3.19 Supervoid Reverb ----------------------------------------------
 * Room to huge. FREQ/GAIN form the feedback shelving filter: at max GAIN the
 * treble stays in the tail, lowering it damps progressively. */
static void m_supervoid(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    svf_co_t hc, lc;
    svf_coeffs(&hc, pexp(p[4], 20.0f, 2000.0f), 0.707f);
    svf_coeffs(&lc, pexp(p[5], 400.0f, 18000.0f), 0.707f);

    float il = svf_lp(&s->f3[0], &lc, svf_hp(&s->f1[0], &hc, *l));
    float ir = svf_lp(&s->f3[1], &lc, svf_hp(&s->f1[1], &hc, *r));

    float pl, pr;
    predelay(s, il, ir, 1.0f + p01(p[0]) * (float)(WORK_PRE_LEN - 8), &pl, &pr);

    float size  = 0.5f + p01(p[1]) * 1.5f;
    float fb    = 0.70f + p01(p[1]) * 0.29f;
    /* Shelving: FREQ sets where damping starts, GAIN how much survives it */
    float shelf = pexp(p[2], 500.0f, 16000.0f);
    float damp  = op_a(shelf) * (0.15f + (1.0f - p01(p[3])) * 0.85f);

    float tl, tr;
    tank_run(s, pl, pr, size, fb, fclampf(damp, 0.0f, 1.0f), &tl, &tr);

    float mix = p01(p[6]);
    *l = *l * (1.0f - mix) + tl * mix;
    *r = *r * (1.0f - mix) + tr * mix;
}

/* --- A.3.20 Warble --------------------------------------------------------
 * Tape wobble. SPEED 0 means random, per the manual, so at that setting the
 * modulator becomes a smoothed random walk instead of a sine. The added noise
 * is deliberately outside the MIX law, again per the manual. */
static void m_warble(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float depth = p01(p[1]) * 0.004f * (float)WORK_SR;
    float mod;
    if (p[0] == 0) {
        s->drift += (rnd_bi(&s->rng) - s->drift * 0.02f) * 0.0015f;
        s->drift = fclampf(s->drift, -1.0f, 1.0f);
        mod = s->drift;
    } else {
        s->lfo_ph += pexp(p[0], 0.1f, 12.0f) / (float)WORK_SR;
        if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;
        mod = sinf(s->lfo_ph * 2.0f * (float)M_PI);
    }

    float stereo = p01(p[6]);
    s->dl[s->dw * 2]     = *l;
    s->dl[s->dw * 2 + 1] = *r;

    float base = 0.006f * (float)WORK_SR;
    float wl = dl_read(s->dl, WORK_DLY_LEN, s->dw, base + mod * depth, 0);
    float wr = dl_read(s->dl, WORK_DLY_LEN, s->dw,
                       base + mod * depth * (1.0f - stereo * 2.0f), 1);
    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    /* Band shaping: BASE sets the low edge, WIDTH how far above it we keep */
    svf_co_t hc, lc;
    float b = pexp(p[2], 20.0f, 2000.0f);
    svf_coeffs(&hc, b, 0.707f);
    svf_coeffs(&lc, fclampf(b * (1.0f + p01(p[3]) * 60.0f), b + 40.0f, 18000.0f), 0.707f);
    wl = svf_lp(&s->f2[0], &lc, svf_hp(&s->f1[0], &hc, wl));
    wr = svf_lp(&s->f2[1], &lc, svf_hp(&s->f1[1], &hc, wr));

    float mix = p01(p[7]);
    float ol = *l * (1.0f - mix) + wl * mix;
    float or_ = *r * (1.0f - mix) + wr * mix;

    /* Noise sits outside the mix law */
    float nlev = p01(p[4]);
    if (nlev > 0.001f) {
        svf_co_t nc;
        svf_coeffs(&nc, pexp(p[5], 20.0f, 8000.0f), 0.707f);
        ol += svf_hp(&s->f3[0], &nc, rnd_bi(&s->rng)) * nlev * 0.25f;
        or_ += svf_hp(&s->f3[1], &nc, rnd_bi(&s->rng)) * nlev * 0.25f;
    }

    *l = ol;
    *r = or_;
}

/* ------------------------------------------------- 5. dispatch and plumbing */

static void slot_reset(work_slot_t *s) {
    /* Clear everything except the allocated buffers themselves. */
    float *dl = s->dl, *pre = s->pre, *comb = s->comb, *ap = s->ap;
    uint32_t rng = s->rng;
    memset(s, 0, sizeof(*s));
    s->dl = dl; s->pre = pre; s->comb = comb; s->ap = ap;
    s->rng = rng ? rng : 0x9E3779B9u;

    if (dl)   memset(dl,   0, sizeof(float) * WORK_DLY_LEN * 2);
    if (pre)  memset(pre,  0, sizeof(float) * WORK_PRE_LEN * 2);
    if (comb) memset(comb, 0, sizeof(float) * WORK_TANK_COMBS * 2 * WORK_TANK_LEN);
    if (ap)   memset(ap,   0, sizeof(float) * WORK_TANK_APS * 2 * WORK_AP_LEN);
}

static void run_machine(mctx_t *m, int machine, float *l, float *r) {
    switch (machine) {
        case WORK_FX_CHRONO:    m_chrono(m, l, r);    break;
        case WORK_FX_COMB:      m_comb(m, l, r);      break;
        case WORK_FX_COMP:      m_comp(m, l, r);      break;
        case WORK_FX_DAISY:     m_daisy(m, l, r);     break;
        case WORK_FX_DEGRADER:  m_degrader(m, l, r);  break;
        case WORK_FX_DIRT:      m_dirt(m, l, r);      break;
        case WORK_FX_FOLDER:    m_folder(m, l, r);    break;
        case WORK_FX_FBANK:     m_fbank(m, l, r);     break;
        case WORK_FX_WARPER:    m_warper(m, l, r);    break;
        case WORK_FX_FLANGER:   m_flanger(m, l, r);   break;
        case WORK_FX_LPF:       m_lpf(m, l, r);       break;
        case WORK_FX_MMF:       m_mmf(m, l, r);       break;
        case WORK_FX_CHORUS:    m_chorus(m, l, r);    break;
        case WORK_FX_PHASE98:   m_phase98(m, l, r);   break;
        case WORK_FX_RUMSKLANG: m_rumsklang(m, l, r); break;
        case WORK_FX_SATDELAY:  m_satdelay(m, l, r);  break;
        case WORK_FX_STEELBOX:  m_steelbox(m, l, r);  break;
        case WORK_FX_SUPERVOID: m_supervoid(m, l, r); break;
        case WORK_FX_WARBLE:    m_warble(m, l, r);    break;
        case WORK_FX_BYPASS:
        default:                                      break;
    }
}

/* ------------------------------------------------------------- sequencer */

/* Does this step's condition allow it to fire on this pass? */
static int cond_fires(work_t *w, int cond) {
    switch (cond) {
        case WORK_COND_OFF:       return 1;
        case WORK_COND_FILL:      return w->fill != 0;
        case WORK_COND_NOT_FILL:  return w->fill == 0;
        case WORK_COND_PRE:       return w->pre_result != 0;
        case WORK_COND_NOT_PRE:   return w->pre_result == 0;
        case WORK_COND_FIRST:     return w->pass == 0;
        case WORK_COND_NOT_FIRST: return w->pass != 0;
        case WORK_COND_P25:       return rnd_01(&w->cond_rng) < 0.25f;
        case WORK_COND_P50:       return rnd_01(&w->cond_rng) < 0.50f;
        case WORK_COND_P75:       return rnd_01(&w->cond_rng) < 0.75f;
        /* A:B fires on the Ath pass of every B passes */
        case WORK_COND_1_2:       return (w->pass % 2) == 0;
        case WORK_COND_2_2:       return (w->pass % 2) == 1;
        case WORK_COND_1_3:       return (w->pass % 3) == 0;
        case WORK_COND_2_3:       return (w->pass % 3) == 1;
        case WORK_COND_3_3:       return (w->pass % 3) == 2;
        case WORK_COND_1_4:       return (w->pass % 4) == 0;
        case WORK_COND_2_4:       return (w->pass % 4) == 1;
        case WORK_COND_3_4:       return (w->pass % 4) == 2;
        case WORK_COND_4_4:       return (w->pass % 4) == 3;
        default:                  return 1;
    }
}

/* The step whose start time we have most recently passed. Micro-timing shifts
 * a step's start by micro/24 of a step, so the active step is not simply
 * floor(position / step). Recomputed per block, which puts micro-timing
 * resolution at one block (~2.9 ms) — finer than a 1/24 step at any sane
 * tempo, but not sample-accurate. */
static int active_step(const work_t *w, double sf) {
    int len = w->seq_len ? w->seq_len : 1;
    int best = -1;
    double best_start = -1e18;

    for (int i = 0; i < len; ++i) {
        double start = (double)i * sf + ((double)w->step[i].micro / 24.0) * sf;
        if (start <= w->seq_frame && start > best_start) {
            best_start = start;
            best = i;
        }
    }
    /* Nothing has started yet this pass (step 0 nudged late): hold the last
     * step of the pattern, whose lock state is still the current one. */
    return best < 0 ? len - 1 : best;
}

static void seq_run(work_t *w, int frames) {
    if (!w->seq_on) {
        w->held_mask = 0;
        return;
    }

    double sf    = (double)step_frames(w);
    int    len   = w->seq_len ? w->seq_len : 1;
    double total = sf * (double)len;

    w->seq_frame += (double)frames;
    while (w->seq_frame >= total) {
        w->seq_frame -= total;
        w->pass++;
    }

    int cur = active_step(w, sf);
    w->seq_pos = cur;
    if (cur == w->last_step) return;
    w->last_step = cur;

    const work_step_t *st = &w->step[cur];
    if (!st->active) return;                  /* no trig: nothing changes */

    if (!cond_fires(w, st->cond)) {
        w->pre_result = 0;
        return;
    }
    w->pre_result = 1;

    /* The trig latches its locks. Parameters this trig does NOT lock revert to
     * their base value, so each firing trig is a complete snapshot of the FX
     * state — predictable to program, and the behaviour documented in help. */
    w->held_mask = st->lock_mask;
    for (int i = 0; i < WORK_LOCKABLE; ++i) w->held[i] = st->lock[i];

    /* Retrig restarts the FX LFOs and the Multimode Filter envelope. It does
     * not stutter audio — the Degrader's FREZ is the machine for that. */
    if (st->retrig != WORK_RETRIG_OFF) {
        for (int n = 0; n < WORK_LFOS; ++n) w->lfo_ph[n] = 0.0f;
        for (int s = 0; s < WORK_SLOTS; ++s) w->slot[s].env_stage = 1.0f;
    }
}

/* Build the effective parameter set for this block: base, then the locks the
 * current trig latched, then the FX LFOs on top. */
static void build_effective(work_t *w, int frames) {
    for (int s = 0; s < WORK_SLOTS; ++s)
        for (int i = 0; i < WORK_PARAMS; ++i)
            w->eff[s][i] = w->cfg[s].p[i];
    w->eff_machine[0] = w->cfg[0].machine;
    w->eff_machine[1] = w->cfg[1].machine;
    w->eff_mix        = w->mix;

    if (w->seq_on && w->held_mask) {
        for (int i = 0; i < WORK_LOCKABLE; ++i) {
            if (!(w->held_mask & (1u << i))) continue;
            uint8_t v = w->held[i];
            if (i < 16)       w->eff[i / WORK_PARAMS][i % WORK_PARAMS] = v;
            else if (i < 18)  w->eff_machine[i - 16] =
                                  (uint8_t)iclamp(v, 0, WORK_FX_COUNT - 1);
            else              w->eff_mix = v;
        }
    }

    for (int n = 0; n < WORK_LFOS; ++n) {
        work_lfo_cfg_t *L = &w->lfo[n];

        /* Multiplier scales speed; both are on the tempo grid like Tonverk */
        float steps = pexp(L->speed, 64.0f, 0.125f);
        float mult  = powf(2.0f, (float)(L->mult / 16) - 4.0f);
        float per   = fmaxf(steps * mult * step_frames(w), 1.0f);
        w->lfo_ph[n] += (float)frames / per;
        while (w->lfo_ph[n] >= 1.0f) w->lfo_ph[n] -= 1.0f;

        if (L->dest < 0 || L->dest >= WORK_SLOTS * WORK_PARAMS) continue;

        float ph = w->lfo_ph[n] + p01(L->phase);
        if (ph >= 1.0f) ph -= 1.0f;

        float v;
        switch (L->wave % 7) {
            case 0: v = 1.0f - 2.0f * fabsf(2.0f * ph - 1.0f); break;       /* tri  */
            case 1: v = sinf(ph * 2.0f * (float)M_PI); break;               /* sine */
            case 2: v = ph < 0.5f ? 1.0f : -1.0f; break;                    /* sqr  */
            case 3: v = 1.0f - 2.0f * ph; break;                            /* saw  */
            case 4: v = 2.0f * ph - 1.0f; break;                            /* ramp */
            case 5: v = expf(-3.0f * ph) * 2.0f - 1.0f; break;              /* exp  */
            default: v = rnd_bi(&w->rng); break;                            /* rand */
        }

        int slot = L->dest / WORK_PARAMS;
        int idx  = L->dest % WORK_PARAMS;
        /* Modulate around the LOCKED value, not the base one — otherwise a
         * p-lock and an LFO pointed at the same parameter fight each other. */
        int base = w->eff[slot][idx];
        int out  = base + (int)(v * pbi(L->depth) * 127.0f);
        w->eff[slot][idx] = (uint8_t)iclamp(out, 0, 127);
    }
}

work_t *work_create(const host_api_v1_t *host) {
    work_t *w = (work_t *)calloc(1, sizeof(work_t));
    if (!w) return NULL;

    w->host = host;
    w->bpm  = 120.0f;
    w->mix  = 127;
    w->rng  = 0xC2B2AE35u;

    /* Sequencer starts off, so the audio_fx build behaves as a plain static
     * FX chain until something turns it on. */
    w->seq_on    = 0;
    w->seq_len   = WORK_PAGE_STEPS;
    w->last_step = -1;
    w->cond_rng  = 0x6C078965u;

    for (int i = 0; i < WORK_LFOS; ++i) {
        w->lfo[i].dest  = -1;
        w->lfo[i].speed = 32;
        w->lfo[i].mult  = 64;
        w->lfo[i].depth = 64;
    }

    for (int i = 0; i < WORK_SLOTS; ++i) {
        work_slot_t *s = &w->slot[i];
        s->dl   = (float *)calloc((size_t)WORK_DLY_LEN * 2, sizeof(float));
        s->pre  = (float *)calloc((size_t)WORK_PRE_LEN * 2, sizeof(float));
        s->comb = (float *)calloc((size_t)WORK_TANK_COMBS * 2 * WORK_TANK_LEN, sizeof(float));
        s->ap   = (float *)calloc((size_t)WORK_TANK_APS * 2 * WORK_AP_LEN, sizeof(float));
        if (!s->dl || !s->pre || !s->comb || !s->ap) { work_destroy(w); return NULL; }

        s->rng = 0x9E3779B9u ^ (uint32_t)(i * 0x85EBCA6Bu);
        s->last_machine = WORK_FX_BYPASS;

        w->cfg[i].machine = WORK_FX_BYPASS;
        for (int k = 0; k < WORK_PARAMS; ++k)
            w->cfg[i].p[k] = PARAM_DEFAULT[WORK_FX_BYPASS][k];
    }

    return w;
}

void work_destroy(work_t *w) {
    if (!w) return;
    for (int i = 0; i < WORK_SLOTS; ++i) {
        free(w->slot[i].dl);
        free(w->slot[i].pre);
        free(w->slot[i].comb);
        free(w->slot[i].ap);
    }
    free(w);
}

void work_process(work_t *w, const int16_t *in, int16_t *out, int frames) {
    if (!w || frames <= 0) return;

    if (w->host && w->host->get_bpm && !w->clock_running) {
        float b = w->host->get_bpm();
        if (b > 20.0f && b < 400.0f) w->bpm = b;
    }

    seq_run(w, frames);
    build_effective(w, frames);

    /* A machine change resets that slot's state so a reverb tail or delay
     * line from the previous machine cannot leak into the new one. This uses
     * the EFFECTIVE machine, so a per-step machine lock swaps cleanly too. */
    for (int i = 0; i < WORK_SLOTS; ++i) {
        if (w->slot[i].last_machine != w->eff_machine[i]) {
            int keep = w->eff_machine[i];
            slot_reset(&w->slot[i]);
            w->slot[i].last_machine = keep;
        }
    }

    /* Multimode Filter envelope gate, from note events seen since last block */
    if (w->note_pending) {
        for (int i = 0; i < WORK_SLOTS; ++i) {
            w->slot[i].env_stage = 1.0f;
            for (int n = 0; n < WORK_LFOS; ++n)
                if (w->lfo[n].trig) w->lfo_ph[n] = 0.0f;
        }
        w->note_pending = 0;
    }

    float gmix = p01(w->eff_mix);

    for (int f = 0; f < frames; ++f) {
        float dry_l = (float)in[f * 2]     * (1.0f / 32768.0f);
        float dry_r = (float)in[f * 2 + 1] * (1.0f / 32768.0f);
        float l = dry_l, r = dry_r;

        for (int i = 0; i < WORK_SLOTS; ++i) {
            mctx_t m = { w, &w->slot[i], w->eff[i], frames };
            run_machine(&m, w->eff_machine[i], &l, &r);
            l = sane(l);
            r = sane(r);
        }

        l = dry_l * (1.0f - gmix) + l * gmix;
        r = dry_r * (1.0f - gmix) + r * gmix;

        int vl = (int)lrintf(fclampf(l, -1.0f, 1.0f) * 32767.0f);
        int vr = (int)lrintf(fclampf(r, -1.0f, 1.0f) * 32767.0f);
        out[f * 2]     = (int16_t)iclamp(vl, -32768, 32767);
        out[f * 2 + 1] = (int16_t)iclamp(vr, -32768, 32767);
    }
}

void work_on_midi(work_t *w, const uint8_t *msg, int len, int source) {
    (void)source;
    if (!w || !msg || len < 1) return;

    switch (msg[0]) {
        case 0xF8:                       /* clock tick, 24 ppqn */
            w->clock_ticks++;
            w->clock_running = 1;
            break;
        case 0xFA:                       /* start */
        case 0xFB:                       /* continue */
            w->clock_running = 1;
            w->clock_ticks = 0;
            for (int n = 0; n < WORK_LFOS; ++n) w->lfo_ph[n] = 0.0f;
            /* Restart the pattern from the top. 0xFB (continue) restarts too:
             * an FX pattern has no note state worth resuming mid-bar, and
             * landing on step 0 is what a performer expects. */
            w->seq_frame  = 0.0;
            w->last_step  = -1;
            w->pass       = 0;
            w->pre_result = 0;
            break;
        case 0xFC:                       /* stop */
            w->clock_running = 0;
            break;
        default:
            if (len >= 3 && (msg[0] & 0xF0) == 0x90 && msg[2] > 0) {
                w->note_pending = 1;
            } else if (len >= 3 && ((msg[0] & 0xF0) == 0x80 ||
                                    ((msg[0] & 0xF0) == 0x90 && msg[2] == 0))) {
                for (int i = 0; i < WORK_SLOTS; ++i) w->slot[i].env_stage = 4.0f;
            }
            break;
    }
}

/* ---------------------------------------------------------- parameter I/O */

/* Accept either a machine index or its name, so a UI can send whichever. */
static int parse_machine(const char *val) {
    if (!val || !*val) return -1;
    if (val[0] >= '0' && val[0] <= '9') return iclamp(atoi(val), 0, WORK_FX_COUNT - 1);
    for (int i = 0; i < WORK_FX_COUNT; ++i)
        if (strcasecmp(val, MACHINE_NAME[i]) == 0) return i;
    return -1;
}

/* Parse the trailing integer of a key like "step12" or "locks7". Returns -1 if
 * the suffix is missing or out of range. */
static int key_index(const char *key, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(key, prefix, n) != 0) return -1;
    if (key[n] < '0' || key[n] > '9') return -1;
    int v = atoi(key + n);
    return (v >= 0 && v < WORK_STEPS) ? v : -1;
}

/* "lock12_5" -> step 12, lock index 5. Returns 0 if not of that shape. */
static int parse_lock_key(const char *key, int *stp, int *idx) {
    if (strncmp(key, "lock", 4) != 0) return 0;
    if (key[4] < '0' || key[4] > '9') return 0;
    const char *us = strchr(key + 4, '_');
    if (!us) return 0;
    int s = atoi(key + 4);
    int i = atoi(us + 1);
    if (s < 0 || s >= WORK_STEPS) return 0;
    if (i < 0 || i >= WORK_LOCKABLE) return 0;
    *stp = s; *idx = i;
    return 1;
}

static void step_set_lock(work_step_t *st, int idx, int value) {
    if (value < 0) {
        st->lock_mask &= ~(1u << idx);       /* -1 clears the lock */
    } else {
        st->lock_mask |= (1u << idx);
        st->lock[idx] = (uint8_t)iclamp(value, 0, 127);
    }
}

/* "p=v,p=v,..." — replaces every lock on the step. An empty string clears. */
static void step_set_locks(work_step_t *st, const char *s) {
    st->lock_mask = 0;
    while (s && *s) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        int idx = atoi(s);
        const char *eq = strchr(s, '=');
        if (!eq) break;
        int val = atoi(eq + 1);
        if (idx >= 0 && idx < WORK_LOCKABLE) step_set_lock(st, idx, val);
        const char *comma = strchr(eq, ',');
        if (!comma) break;
        s = comma + 1;
    }
}

/* "fx1_p3" -> slot 0, param 2. Returns 0 if the key is not of that shape. */
static int parse_slot_param(const char *key, int *slot, int *idx) {
    if (strncmp(key, "fx", 2) != 0) return 0;
    if (key[2] != '1' && key[2] != '2') return 0;
    if (strncmp(key + 3, "_p", 2) != 0) return 0;
    int n = atoi(key + 5);
    if (n < 1 || n > WORK_PARAMS) return 0;
    *slot = key[2] - '1';
    *idx  = n - 1;
    return 1;
}

/* Restore from the blob written by get_param("state"). Keys absent from the
 * blob keep their current value, so a partial blob is a legal patch. */
static void apply_state(work_t *w, const char *json) {
    const char *q;

    if ((q = strstr(json, "\"mix\":")) != NULL) w->mix = (uint8_t)iclamp(atoi(q + 6), 0, 127);

    for (int s = 0; s < WORK_SLOTS; ++s) {
        char key[16];
        snprintf(key, sizeof(key), "\"m%d\":", s + 1);
        if ((q = strstr(json, key)) != NULL) {
            int mc = iclamp(atoi(q + strlen(key)), 0, WORK_FX_COUNT - 1);
            w->cfg[s].machine = (uint8_t)mc;
        }
        snprintf(key, sizeof(key), "\"p%d\":[", s + 1);
        if ((q = strstr(json, key)) != NULL) {
            const char *c = q + strlen(key);
            for (int i = 0; i < WORK_PARAMS && *c && *c != ']'; ++i) {
                w->cfg[s].p[i] = (uint8_t)iclamp(atoi(c), 0, 127);
                while (*c && *c != ',' && *c != ']') c++;
                if (*c == ',') c++;
            }
        }
    }

    for (int n = 0; n < WORK_LFOS; ++n) {
        char key[16];
        snprintf(key, sizeof(key), "\"l%d\":[", n + 1);
        if ((q = strstr(json, key)) != NULL) {
            const char *c = q + strlen(key);
            int v[7] = {-1, 32, 64, 0, 64, 0, 0};
            for (int i = 0; i < 7 && *c && *c != ']'; ++i) {
                v[i] = atoi(c);
                while (*c && *c != ',' && *c != ']') c++;
                if (*c == ',') c++;
            }
            w->lfo[n].dest  = (int8_t)iclamp(v[0], -1, WORK_SLOTS * WORK_PARAMS - 1);
            w->lfo[n].speed = (uint8_t)iclamp(v[1], 0, 127);
            w->lfo[n].mult  = (uint8_t)iclamp(v[2], 0, 127);
            w->lfo[n].wave  = (uint8_t)iclamp(v[3], 0, 127);
            w->lfo[n].depth = (uint8_t)iclamp(v[4], 0, 127);
            w->lfo[n].phase = (uint8_t)iclamp(v[5], 0, 127);
            w->lfo[n].trig  = (uint8_t)iclamp(v[6], 0, 1);
        }
    }

    if ((q = strstr(json, "\"sq\":[")) != NULL) {
        const char *c = q + 6;
        w->seq_on = (uint8_t)(atoi(c) ? 1 : 0);
        if ((c = strchr(c, ',')) != NULL)
            w->seq_len = (uint8_t)iclamp(atoi(c + 1), 1, WORK_STEPS);
    }

    /* The pattern replaces whatever was loaded — a blob carrying "stp" is a
     * complete pattern, so a stale step from the previous patch must not
     * survive underneath it. */
    if ((q = strstr(json, "\"stp\":\"")) != NULL) {
        memset(w->step, 0, sizeof(w->step));
        w->held_mask = 0;
        w->last_step = -1;

        const char *c = q + 7;
        while (*c && *c != '"') {
            int idx = atoi(c);
            work_step_t *st = (idx >= 0 && idx < WORK_STEPS) ? &w->step[idx] : NULL;
            int field = 0;

            while (*c && *c != '|' && *c != '"') {
                if (*c == ',') {
                    field++;
                    int v = atoi(c + 1);
                    if (st) {
                        if (field == 1) st->active = (uint8_t)(v ? 1 : 0);
                        else if (field == 2) st->cond = (uint8_t)iclamp(v, 0, WORK_COND_COUNT - 1);
                        else if (field == 3) st->micro = (int8_t)iclamp(v, -23, 23);
                        else if (field == 4) st->retrig = (uint8_t)iclamp(v, 0, WORK_RETRIG_COUNT - 1);
                    }
                } else if (*c == '+' && st) {
                    int k = atoi(c + 1);
                    const char *eq = strchr(c, '=');
                    if (eq && k >= 0 && k < WORK_LOCKABLE)
                        step_set_lock(st, k, atoi(eq + 1));
                }
                c++;
            }
            if (*c == '|') c++;
        }
    }
}

void work_set_param(work_t *w, const char *key, const char *val) {
    if (!w || !key || !val) return;

    int slot, idx;
    if (parse_slot_param(key, &slot, &idx)) {
        w->cfg[slot].p[idx] = (uint8_t)iclamp(atoi(val), 0, 127);
        return;
    }

    if (strcmp(key, "fx1") == 0 || strcmp(key, "fx2") == 0) {
        int s  = key[2] - '1';
        int mc = parse_machine(val);
        if (mc >= 0 && mc != w->cfg[s].machine) {
            w->cfg[s].machine = (uint8_t)mc;
            /* Loading a machine installs its defaults, as on Tonverk */
            for (int i = 0; i < WORK_PARAMS; ++i)
                w->cfg[s].p[i] = PARAM_DEFAULT[mc][i];
        }
        return;
    }

    if (strcmp(key, "mix") == 0) { w->mix = (uint8_t)iclamp(atoi(val), 0, 127); return; }
    if (strcmp(key, "state") == 0) { if (val[0] == '{') apply_state(w, val); return; }

    /* ------------------------------------------------------- sequencer */
    if (strcmp(key, "seq_on") == 0) {
        int on = atoi(val) ? 1 : 0;
        if (on && !w->seq_on) {          /* restart cleanly when switched on */
            w->seq_frame = 0.0; w->last_step = -1; w->pass = 0; w->pre_result = 0;
        }
        w->seq_on = (uint8_t)on;
        return;
    }
    if (strcmp(key, "seq_len") == 0) {
        w->seq_len = (uint8_t)iclamp(atoi(val), 1, WORK_STEPS);
        if (w->last_step >= w->seq_len) w->last_step = -1;
        return;
    }
    if (strcmp(key, "fill") == 0) { w->fill = (uint8_t)(atoi(val) ? 1 : 0); return; }

    if (strcmp(key, "seq_clear") == 0) {
        memset(w->step, 0, sizeof(w->step));
        w->held_mask = 0;
        w->last_step = -1;
        return;
    }

    {
        int stp, idx;
        if (parse_lock_key(key, &stp, &idx)) {
            step_set_lock(&w->step[stp], idx, atoi(val));
            return;
        }
    }

    {
        int n = key_index(key, "locks");
        if (n >= 0) { step_set_locks(&w->step[n], val); return; }
    }

    {
        int n = key_index(key, "step");
        if (n >= 0) {
            /* "active:cond:micro:retrig" — trailing fields may be omitted */
            work_step_t *st = &w->step[n];
            const char *c = val;
            st->active = (uint8_t)(atoi(c) ? 1 : 0);
            if ((c = strchr(c, ':')) != NULL) {
                st->cond = (uint8_t)iclamp(atoi(++c), 0, WORK_COND_COUNT - 1);
                if ((c = strchr(c, ':')) != NULL) {
                    st->micro = (int8_t)iclamp(atoi(++c), -23, 23);
                    if ((c = strchr(c, ':')) != NULL)
                        st->retrig = (uint8_t)iclamp(atoi(++c), 0, WORK_RETRIG_COUNT - 1);
                }
            }
            w->last_step = -1;   /* re-evaluate: the edited step may be current */
            return;
        }
    }
    if (strcmp(key, "bpm") == 0) {
        float b = (float)atof(val);
        if (b > 20.0f && b < 400.0f) w->bpm = b;
        return;
    }

    if (strncmp(key, "lfo", 3) == 0 && (key[3] == '1' || key[3] == '2') && key[4] == '_') {
        work_lfo_cfg_t *L = &w->lfo[key[3] - '1'];
        const char *f = key + 5;
        int v = atoi(val);
        if      (strcmp(f, "dest")  == 0) L->dest  = (int8_t)iclamp(v, -1, WORK_SLOTS * WORK_PARAMS - 1);
        else if (strcmp(f, "spd")   == 0) L->speed = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "mult")  == 0) L->mult  = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "wave")  == 0) L->wave  = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "depth") == 0) L->depth = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "phase") == 0) L->phase = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "trig")  == 0) L->trig  = (uint8_t)iclamp(v, 0, 1);
        return;
    }
}

int work_get_param(work_t *w, const char *key, char *buf, int buf_len) {
    if (!w || !key || !buf || buf_len <= 1) return -1;
    int cap = buf_len - 1;

    int slot, idx;
    if (parse_slot_param(key, &slot, &idx))
        return nclamp(snprintf(buf, buf_len, "%d", w->cfg[slot].p[idx]), cap);

    if (strcmp(key, "fx1") == 0 || strcmp(key, "fx2") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", w->cfg[key[2] - '1'].machine), cap);

    if (strcmp(key, "mix") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", w->mix), cap);

    /* The values actually reaching the DSP this block, after locks and LFOs.
     * The UI shows these live so a moving parameter reads as moving. */
    if (strcmp(key, "eff1") == 0 || strcmp(key, "eff2") == 0) {
        int s = key[3] - '1';
        int n = 0;
        for (int i = 0; i < WORK_PARAMS; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", w->eff[s][i]), cap);
        return n;
    }
    if (strcmp(key, "effm") == 0)
        return nclamp(snprintf(buf, buf_len, "%d,%d,%d",
                               w->eff_machine[0], w->eff_machine[1], w->eff_mix), cap);

    if (strcmp(key, "seq_on") == 0)  return nclamp(snprintf(buf, buf_len, "%d", w->seq_on), cap);
    if (strcmp(key, "seq_len") == 0) return nclamp(snprintf(buf, buf_len, "%d", w->seq_len), cap);
    if (strcmp(key, "fill") == 0)    return nclamp(snprintf(buf, buf_len, "%d", w->fill), cap);
    if (strcmp(key, "seq_pos") == 0) return nclamp(snprintf(buf, buf_len, "%d", w->seq_pos), cap);

    /* "a:c:m:r:nlocks" — one poll per step for the UI's grid */
    {
        int n = key_index(key, "step");
        if (n >= 0) {
            const work_step_t *st = &w->step[n];
            int nl = 0;
            for (int i = 0; i < WORK_LOCKABLE; ++i)
                if (st->lock_mask & (1u << i)) nl++;
            return nclamp(snprintf(buf, buf_len, "%d:%d:%d:%d:%d",
                                   st->active, st->cond, st->micro, st->retrig, nl), cap);
        }
    }

    /* A single lock: its value, or -1 when that parameter is not locked on
     * that step. The UI reads this to show "*value" while a step is held and
     * to nudge an existing lock rather than restarting from the base value —
     * without the getter it silently did the wrong thing in both places. */
    {
        int stp, idx;
        if (parse_lock_key(key, &stp, &idx)) {
            const work_step_t *st = &w->step[stp];
            int v = (st->lock_mask & (1u << idx)) ? st->lock[idx] : -1;
            return nclamp(snprintf(buf, buf_len, "%d", v), cap);
        }
    }

    {
        int n = key_index(key, "locks");
        if (n >= 0) {
            const work_step_t *st = &w->step[n];
            int written = 0, out = 0;
            for (int i = 0; i < WORK_LOCKABLE; ++i) {
                if (!(st->lock_mask & (1u << i))) continue;
                out = nclamp(out + snprintf(buf + out, (size_t)(buf_len - out),
                                            "%s%d=%d", written ? "," : "",
                                            i, st->lock[i]), cap);
                written = 1;
            }
            if (!written) { buf[0] = '\0'; return 0; }
            return out;
        }
    }

    {
        int n = key_index(key, "locklabel");
        if (n >= 0) return work_lock_label(w, n, buf, buf_len);
    }

    if (strcmp(key, "conds") == 0) {
        int n = 0;
        for (int i = 0; i < WORK_COND_COUNT; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%s",
                                    i ? "," : "", COND_NAME[i]), cap);
        return n;
    }

    if (strcmp(key, "meter") == 0)
        return nclamp(snprintf(buf, buf_len, "%d:%d",
                               (int)(-w->slot[0].gr), (int)(-w->slot[1].gr)), cap);

    /* The eight knob labels for whichever machine a slot currently holds.
     * The UI reads these rather than carrying its own copy of the table —
     * a second copy in JS is a copy that drifts. */
    if (strcmp(key, "labels1") == 0 || strcmp(key, "labels2") == 0) {
        int mc = w->cfg[key[6] - '1'].machine;
        int n = 0;
        for (int i = 0; i < WORK_PARAMS; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%s",
                                    i ? "," : "", PARAM_NAME[mc][i]), cap);
        return n;
    }

    if (strcmp(key, "machines") == 0) {
        int n = 0;
        for (int i = 0; i < WORK_FX_COUNT; ++i) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%s",
                                    i ? "," : "", MACHINE_NAME[i]), cap);
        }
        return n;
    }

    if (strncmp(key, "lfo", 3) == 0 && (key[3] == '1' || key[3] == '2') && key[4] == '_') {
        work_lfo_cfg_t *L = &w->lfo[key[3] - '1'];
        const char *f = key + 5;
        int v = 0;
        if      (strcmp(f, "dest")  == 0) v = L->dest;
        else if (strcmp(f, "spd")   == 0) v = L->speed;
        else if (strcmp(f, "mult")  == 0) v = L->mult;
        else if (strcmp(f, "wave")  == 0) v = L->wave;
        else if (strcmp(f, "depth") == 0) v = L->depth;
        else if (strcmp(f, "phase") == 0) v = L->phase;
        else if (strcmp(f, "trig")  == 0) v = L->trig;
        else return -1;
        return nclamp(snprintf(buf, buf_len, "%d", v), cap);
    }

    /* The Shadow UI and the auto-generated Master FX knob pages read their
     * parameter hierarchy from module.json OR from this key. module.json is
     * static, so it can only ever say "A".."H" — which tells you nothing about
     * what a knob does. Serving it here instead means the labels follow
     * whichever machine each slot currently holds.
     *
     * Kept deliberately compact: the on-device host reads get_param into a
     * 16 KB buffer, so this must not sprawl. Only the two loaded machines'
     * labels are emitted, not all twenty machines' worth. */
    if (strcmp(key, "ui_hierarchy") == 0) {
        int n = 0;
        int m1 = w->cfg[0].machine, m2 = w->cfg[1].machine;

        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
            "{\"levels\":{\"root\":{\"name\":\"Work\",\"params\":["
            "{\"key\":\"fx1\",\"name\":\"FX 1 Machine\",\"type\":\"enum\",\"options\":["), cap);
        for (int i = 0; i < WORK_FX_COUNT; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\"",
                                    i ? "," : "", MACHINE_NAME[i]), cap);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
            "]},{\"key\":\"fx2\",\"name\":\"FX 2 Machine\",\"type\":\"enum\",\"options\":["), cap);
        for (int i = 0; i < WORK_FX_COUNT; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\"",
                                    i ? "," : "", MACHINE_NAME[i]), cap);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
            "]},{\"key\":\"mix\",\"name\":\"Dry/Wet\",\"type\":\"int\",\"min\":0,\"max\":127},"
            "{\"level\":\"fx1p\",\"label\":\"FX 1: %s\"},"
            "{\"level\":\"fx2p\",\"label\":\"FX 2: %s\"},"
            "{\"level\":\"lfo1\",\"label\":\"FX LFO 1\"},"
            "{\"level\":\"lfo2\",\"label\":\"FX LFO 2\"}],"
            "\"knobs\":[\"fx1\",\"fx2\",\"mix\"]}", MACHINE_NAME[m1], MACHINE_NAME[m2]), cap);

        /* One level per slot, named after the machine, carrying its real
         * knob labels. Unused knobs are omitted rather than shown blank. */
        for (int s = 0; s < WORK_SLOTS; ++s) {
            int mc = w->cfg[s].machine;
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                ",\"fx%dp\":{\"name\":\"%s\",\"params\":[", s + 1, MACHINE_NAME[mc]), cap);
            int first = 1;
            for (int i = 0; i < WORK_PARAMS; ++i) {
                if (!PARAM_NAME[mc][i][0]) continue;
                n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                    "%s{\"key\":\"fx%d_p%d\",\"name\":\"%s\",\"type\":\"int\",\"min\":0,\"max\":127}",
                    first ? "" : ",", s + 1, i + 1, PARAM_NAME[mc][i]), cap);
                first = 0;
            }
            if (first)  /* Bypass has no parameters — say so rather than be empty */
                n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                    "{\"key\":\"fx%d_p1\",\"name\":\"(no parameters)\",\"type\":\"int\","
                    "\"min\":0,\"max\":127}", s + 1), cap);
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "],\"knobs\":["), cap);
            first = 1;
            for (int i = 0; i < WORK_PARAMS; ++i) {
                if (!PARAM_NAME[mc][i][0]) continue;
                n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s\"fx%d_p%d\"",
                                        first ? "" : ",", s + 1, i + 1), cap);
                first = 0;
            }
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "]}"), cap);
        }

        for (int l = 0; l < WORK_LFOS; ++l) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                ",\"lfo%d\":{\"name\":\"FX LFO %d\",\"params\":["
                "{\"key\":\"lfo%d_dest\",\"name\":\"Destination\",\"type\":\"int\",\"min\":-1,\"max\":15},"
                "{\"key\":\"lfo%d_spd\",\"name\":\"Speed\",\"type\":\"int\",\"min\":0,\"max\":127},"
                "{\"key\":\"lfo%d_mult\",\"name\":\"Multiplier\",\"type\":\"int\",\"min\":0,\"max\":127},"
                "{\"key\":\"lfo%d_wave\",\"name\":\"Waveform\",\"type\":\"enum\","
                "\"options\":[\"Triangle\",\"Sine\",\"Square\",\"Saw\",\"Ramp\",\"Exponential\",\"Random\"]},"
                "{\"key\":\"lfo%d_depth\",\"name\":\"Depth\",\"type\":\"int\",\"min\":0,\"max\":127},"
                "{\"key\":\"lfo%d_phase\",\"name\":\"Start Phase\",\"type\":\"int\",\"min\":0,\"max\":127},"
                "{\"key\":\"lfo%d_trig\",\"name\":\"Trig Mode\",\"type\":\"enum\","
                "\"options\":[\"Free\",\"Retrig\"]}],"
                "\"knobs\":[\"lfo%d_dest\",\"lfo%d_spd\",\"lfo%d_mult\",\"lfo%d_wave\","
                "\"lfo%d_depth\",\"lfo%d_phase\",\"lfo%d_trig\"]}",
                l + 1, l + 1, l + 1, l + 1, l + 1, l + 1, l + 1, l + 1, l + 1,
                l + 1, l + 1, l + 1, l + 1, l + 1, l + 1, l + 1), cap);
        }

        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "}}"), cap);
        return n;
    }

    if (strcmp(key, "state") == 0) {
        int n = 0;
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                "{\"v\":1,\"mix\":%d", w->mix), cap);
        for (int s = 0; s < WORK_SLOTS; ++s) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"m%d\":%d,\"p%d\":[", s + 1,
                                    w->cfg[s].machine, s + 1), cap);
            for (int i = 0; i < WORK_PARAMS; ++i)
                n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                        i ? "," : "", w->cfg[s].p[i]), cap);
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "]"), cap);
        }
        for (int l = 0; l < WORK_LFOS; ++l) {
            work_lfo_cfg_t *L = &w->lfo[l];
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"l%d\":[%d,%d,%d,%d,%d,%d,%d]", l + 1,
                                    L->dest, L->speed, L->mult, L->wave,
                                    L->depth, L->phase, L->trig), cap);
        }
        /* Sequencer. Only steps that differ from empty are emitted, so a
         * static patch's blob stays small. Steps are separated by '|',
         * fields by ',', and a step's locks by '+' as "index=value". */
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                ",\"sq\":[%d,%d],\"stp\":\"",
                                w->seq_on, w->seq_len), cap);
        int emitted = 0;
        for (int i = 0; i < WORK_STEPS; ++i) {
            const work_step_t *st = &w->step[i];
            if (!st->active && !st->lock_mask && !st->cond && !st->micro && !st->retrig)
                continue;
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    "%s%d,%d,%d,%d,%d", emitted ? "|" : "",
                                    i, st->active, st->cond, st->micro, st->retrig), cap);
            emitted = 1;
            for (int k = 0; k < WORK_LOCKABLE; ++k) {
                if (!(st->lock_mask & (1u << k))) continue;
                n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                        "+%d=%d", k, st->lock[k]), cap);
            }
        }
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\""), cap);

        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "}"), cap);
        return n;
    }

    return -1;
}
