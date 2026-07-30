/*
 * Work — a clean-room FX engine for the Ableton Move. See work_core.h.
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

/* Parameter 0..127 -> -1..+1 with an exact zero at 64 (bipolar knob law) */
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

    /* Roomtone: Schroeder comb + allpass tank */
    float *comb;                 /* [WORK_TANK_COMBS][2][WORK_TANK_LEN] */
    float *ap;                   /* [WORK_TANK_APS][2][WORK_AP_LEN]     */
    int    comb_w[WORK_TANK_COMBS][2];
    int    ap_w[WORK_TANK_APS][2];
    op_t   comb_damp[WORK_TANK_COMBS][2];

    /* Iron Room: Dattorro plate */
    float *pl_diff;              /* [WORK_PLATE_DIFF][WORK_PLATE_DLEN]  */
    float *pl_ap;                /* [WORK_PLATE_APS][WORK_PLATE_APLEN]  */
    float *pl_del;               /* [WORK_PLATE_DELS][WORK_PLATE_DELEN] */
    int    pl_diff_w[WORK_PLATE_DIFF];
    int    pl_ap_w[WORK_PLATE_APS];
    int    pl_del_w[WORK_PLATE_DELS];
    float  pl_a, pl_b;           /* the two branch feedback nodes       */
    float  pl_lfo;
    op_t   pl_damp[2], pl_bw;

    /* Granulator: grains in flight over the rolling input buffer */
    struct {
        int   on;
        float pos;      /* read offset behind the write head, in frames */
        float step;     /* per-sample advance = pitch ratio             */
        float age;      /* samples elapsed                              */
        float len;      /* grain length in samples                      */
        float gl, gr;   /* per-grain pan gains                          */
    } grain[WORK_GRAINS];
    float gr_next;      /* samples until the next grain is launched     */
    float gr_scan;      /* base playhead, driven by SCAN                */

    /* Voidspace: feedback delay network */
    float *fdn;                  /* [WORK_FDN_LINES][WORK_FDN_LEN]      */
    int    fdn_w[WORK_FDN_LINES];
    op_t   fdn_shelf[WORK_FDN_LINES];

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
    float  grain_ph;             /* Clock Pitch grain cursor             */
    float  warp_ph;              /* Spectrum Bender carrier phase        */
    float  drift;                /* Flutter random walk                    */
    float  hold[2];              /* Decimator sample-and-hold              */
    float  hold_ph;
    int    drop_ctr, frez_ctr, frez_len;
    float  frez_pos;
    uint32_t rng;

    /* One Shot voice. One-shot playback of the loaded sample, retriggered
     * by a sequencer trig or an incoming note. `pos` is a fractional read
     * cursor in frames; -1 means idle. */
    double sp_pos;
    float  sp_env;               /* AD envelope level                     */
    int    sp_stage;             /* 0 idle, 1 attack, 2 decay             */
    int    sp_note;              /* note that fired it, for key tracking  */
    work_vfilt_t sp_filt;        /* One Shot is monophonic: one filter    */
    /* Voice-filter coefficients, refreshed once per BLOCK. Per sample would
     * mean a tanf() per voice per channel per frame, which does not fit. */
    svf_co_t v_hp[WORK_VOICES], v_lp[WORK_VOICES];
    svf_co_t sp_hp, sp_lp;
    int      vf_on;              /* 0 skips the filter entirely           */

    /* Polyphonic voices for Polysample and Slicer. */
    work_voice_t voice[WORK_VOICES];
    uint32_t     voice_clock;    /* monotonic, for oldest-first stealing  */

    /* Wavescan: two oscillator phases and their animation phases. */
    double wf_ph[2];
    float  wf_anim[2];
    float  wf_sh[2];             /* sample-and-hold value per oscillator  */

    /* Tilt: shelving filter state, two channels each. TWO poles per band,
     * one pole per band: `in - low` is an exact complementary highpass only for a
     * single pole, and cascading a second one breaks the null. See m_shape. */
    float  sh_lo[2], sh_hi[2];

    int    last_machine;         /* reset state when the machine changes  */
} work_slot_t;

/* ------------------------------------------------------------------ track
 *
 * Everything that belongs to ONE track: its machine chain, the voices that
 * feed it, the sample they read, and the modulators that reach them. The
 * reference device has eight of these; this build has WORK_TRACKS of them and
 * currently that is 1, so the split is a pure reorganisation with no
 * behavioural change. See DESIGN-8TRACK.md.
 *
 * What is NOT here is as deliberate as what is: transport, the pattern bank,
 * song mode and the global dry/wet stay on `struct work` because they are the
 * same for every track. Anything that ends up needing to differ per track has
 * to move here explicitly rather than by accident. */
typedef struct {
    work_vfilt_cfg_t     vfilt;        /* voice filter, shared by this track's slots */
    work_slot_cfg_t      cfg[WORK_STAGES];
    work_lfo_cfg_t       lfo[WORK_LFOS];
    work_slot_t          slot[WORK_STAGES];

    /* Where the track sits in the mix, applied after the last insert — the
     * "routing" end of the reference device's per-track chain.
     *
     * They exist now, at one track, because the LOCK MAP needs them: it is
     * rebuilt exactly once and the two indices had to be either real or absent,
     * not reserved holes that answer "?" to every UI that asks. They are also
     * what replaces locking the global dry/wet, which stops being per-track.
     *
     * level 127 is unity and pan 64 is centre, so a preset that has never heard
     * of either loads sounding exactly as it did. */
    /* The locks this track's lane latched on its last firing trig. Per track,
     * because each lane fires its own trigs — one shared set would let track 3
     * apply track 5's locks. */
    uint8_t              held[WORK_LOCKABLE];
    uint64_t             held_mask;

    uint8_t              level;        /* 0..127, 127 = unity   */
    uint8_t              pan;          /* 0..127, 64 = centre   */
    float                lgain, rgain; /* resolved once per block from those   */

    uint8_t              src_trig_pending;  /* this track's trig wants a voice */
    /* Edge detection, PER LANE. Micro-timing shifts a step's start time, and
     * it is a property of the step in the lane — so two lanes are genuinely
     * not on the same step at the same moment, and one shared edge detector
     * would let whichever lane advanced first swallow the others' trigs. */
    int                  last_step;

    /* Effective values, recomputed per block as
     *   base (cfg) -> parameter locks from the current step -> FX LFOs
     * which is the order that makes locks predictable: a lock sets the value,
     * and the LFO then moves around
     * whatever the lock set. */
    uint8_t              eff[WORK_STAGES][WORK_PARAMS];
    uint8_t              eff_machine[WORK_STAGES];
    uint8_t              eff_level, eff_pan;
    work_vfilt_cfg_t     eff_vfilt;
    float                lfo_ph[WORK_LFOS];

    /* modulation envelope, and its per-trig runtime */
    work_modenv_cfg_t    menv;
    float                menv_val;
    float                menv_stage;   /* 0 idle, 1 attack, 2 hold, 3 decay */
    float                menv_t;

    int                  note_pending;  /* note-on seen since last block */
    int                  note_num;      /* its pitch, for polyphonic SRC  */
    int                  note_vel;

    uint32_t             rng;           /* LFO random wave; kept separate from
                                         * the slots' so an LFO cannot shift a
                                         * Decimator's dropout sequence */

    /* ------------------------------------------------------ sample memory
     *
     * Allocated once in work_create() and never resized — the render path
     * must not allocate. Interleaved stereo int16, which is both half the
     * size of float and exactly the format the host already speaks.
     *
     * `sample_frames` is what has been COMMITTED. `sample_fill` is how far
     * an in-progress transfer has got; the render path reads only
     * sample_frames, so a partial upload is never audible. */
    int16_t             *sample;
    int                  sample_frames; /* committed length, 0 = empty       */
    int                  sample_fill;   /* frames written by the transfer    */
    int                  sample_declared; /* frames the transfer promised    */
    char                 sample_name[32];
    /* Where the sample came from. The ENGINE never opens it — work_set_param
     * runs on the audio thread — it only carries the string so a preset can
     * record which file the patch expects. The UI reads it back after loading
     * a preset and reloads the audio itself. Without this a preset restored
     * every parameter, LFO, pattern and lock of an SRC patch and then played
     * silence, which reads as a broken module rather than a missing file. */
    char                 sample_path[192];
} work_track_t;

/* The track the PARAMETER interface addresses: the selected one.
 *
 * This is not the track the audio is on. The render path walks every track and
 * names each explicitly (see work_process); nothing per-block may use this
 * macro, because it answers "the track the UI is pointed at" and would make
 * all eight share the selected track's chain.
 *
 * MIDI is the third case again: a channel selects the track, so an incoming CC
 * addresses the track its channel names rather than the selected one. */
#define TRK(w) (&(w)->trk[(w)->sel_track])

struct work {
    const host_api_v1_t *host;
    uint32_t             rui_rev;      /* bumped on every write; see rui_poll */

    work_track_t         trk[WORK_TRACKS];

    uint8_t              mix;          /* global wet/dry, 0..127 */
    uint8_t              eff_mix;

    /* live recording: knob moves land on the playing step */
    uint8_t              live_rec;

    /* Feedback protection. schwung's own guard walks chain SLOTS only, so it
     * is blind to overtake modules entirely — Smack hit this on hardware and
     * had to grow its own. `monitor` 0 mutes the live input; `hw_input` marks
     * the builds that actually read the mic so a UI knows whether to arm the
     * guard at all. */
    uint8_t              monitor;
    uint8_t              hw_input;

    /* MIDI CC duplicate guard. A channel-matched chain slot can deliver one
     * external CC twice (channel dispatch + FX broadcast), so identical
     * messages inside ~2 blocks are dropped — the Mono convention. */
    uint8_t              cc_last[3];
    uint64_t             cc_last_frames;
    uint64_t             cc_frames;

    /* NRPN assembly. Hardware sequencers have shipped repeated bugs where
     * NRPN could not reach a parameter's full range, so this resolves the
     * 14-bit value and scales it across the destination's real range rather
     * than truncating to 7 bits. */
    int                  nrpn_num;
    int                  nrpn_msb;

    /* sequencer. A bank of patterns; CURPAT() is the one being edited and
     * played, which song mode moves underneath the editor. */
    work_pattern_t       pat[WORK_PATTERNS];
    uint8_t              cur_pattern;
    /* Which track the PARAMETER interface addresses. The render path ignores
     * it entirely — see the loop in work_process. */
    uint8_t              sel_track;
    uint8_t              seq_on;

    /* edit history: one undo level plus a separate memorize slot, both
     * whole-pattern snapshots (a pattern is ~1.8 kB, so 2 copies is cheap) */
    work_pattern_t       undo_buf;
    work_pattern_t       redo_buf;
    work_pattern_t       memo_buf;
    uint8_t              undo_valid, redo_valid, memo_valid;

    /* song */
    work_song_row_t      song[WORK_SONG_ROWS];
    uint8_t              song_on;
    uint8_t              song_len;
    uint8_t              song_row;
    uint8_t              song_rep;
    uint8_t              fill;
    double               seq_frame;     /* frames since the pattern restarted */
    int                  seq_pos;       /* the SELECTED lane's step, for display */
    int                  pass;          /* pattern repetitions, for A:B and 1ST*/
    int                  pre_result;    /* last conditional outcome, for PRE   */
    uint32_t             cond_rng;      /* probability conditions              */


    /* What the last state load had to change or drop, for the UI to show. A
     * migration that silently lands a lock somewhere else is exactly the
     * failure this exists to prevent: the preset would load, sound wrong, and
     * say nothing. Empty when the blob needed no translation. */
    char                 load_note[64];

    /* Scratch for the state blob, and for one lane's packed bytes on the way
     * in or out. Both live on the instance rather than the stack because
     * work_get_param runs on the shim's AUDIO THREAD, where 48 KB of automatic
     * storage is not something to assume; and rather than being static because
     * a file-scope buffer would be shared by every instance in the chain.
     *
     * The blob is built whole and then served in windows — see the "state"
     * and "state@<offset>" keys. */
    char                 state_buf[WORK_STATE_MAX];
    uint8_t              lane_buf[WORK_LANE_MAX];

    /* transport */
    float                bpm;
    int                  clock_ticks;
    int                  clock_running;
};

/* ---------------------------------------------------------- machine names */

static const char *MACHINE_NAME[WORK_FX_COUNT] = {
    "Bypass", "Clock Pitch", "Comb Filter", "Compressor", "Chain Delay",
    "Decimator", "Gritshaper", "Fold Filter", "Filterbank", "Spectrum Bender",
    "Endless Flanger", "Low-Pass Filter", "Multimode Filter", "Wide Chorus",
    "Phase Array", "Roomtone Reverb", "Drive Delay", "Iron Room Reverb",
    "Voidspace Reverb", "Flutter", "Granulator", "One Shot",
    "Polysample", "Slicer", "Wavescan", "Tilt"
};

/* Knob labels A-H per machine, using the reference material's abbreviations.
 * An empty string means the machine leaves that knob unused. */
static const char *PARAM_NAME[WORK_FX_COUNT][WORK_PARAMS] = {
/* Bypass     */ {"","","","","","","",""},
/* Clock      */ {"TUNE","WIN","FDBK","DEP","HPF","LPF","SPD","MIX"},
/* Comb       */ {"SPD","DEP","SPH","DTUN","FREQ","FDBK","LPF","MIX"},
/* Comp       */ {"THR","ATK","REL","MUP","RAT","SCS","SCF","MIX"},
/* Chain      */ {"DRV","TIME","FDBK","WIDH","MOD","SKEW","FILT","MIX"},
/* Decimator  */ {"BR","OVER","SRR","DROP","RATE","DEP","FREZ","F.TIM"},
/* Grit       */ {"DRV","RECT","HPF","LPF","NOIS","N.FRQ","N.RES","MIX"},
/* Fold       */ {"ILEV","HP","FOLD","OLEV","FREQ","RESO","TYPE","DIST"},
/* Filterbank — the manual calls these Gain A..H, but the band each one
 * controls is the useful thing to know, so the label IS the frequency. */
/* Filterbank */ {"90Hz","122Hz","225Hz","418Hz","777Hz","1k4","2k7","4k+"},
/* Bender     */ {"SPD","DEP","SPH","LAG","SHFT","SPRD","SBND","MIX"},
/* Flanger    */ {"SPD","DEP","TUNE","FDBK","LPF","","",""},
/* LPF        */ {"SPD","DEP","SPH","LAG","FREQ","RESO","SPRD",""},
/* MMF        */ {"ATK","DEC","SUS","REL","FREQ","RESO","TYPE","ENV"},
/* Chorus     */ {"DEP","SPD","HPF","WDTH","MIX","","",""},
/* PhsArray   */ {"SPD","DEP","SHP","LAG","FREQ","FDBK","STG","MIX"},
/* Roomtone   */ {"PRE","EARLY","DAMP","SIZE","LOWC","HIGHC","",""},
/* DrvDelay   */ {"TIME","PPONG","WID","FDBK","HPF","LPF","MIX",""},
/* IronRoom   */ {"SIZE","FDBK","BRIT","PRE","WDTH","DIFF","LOWC","MIX"},
/* Voidspace  */ {"PRE","DEC","FREQ","GAIN","HPF","LPF","MIX",""},
/* Flutter    */ {"SPEED","DEPTH","BASE","WIDTH","N.LEV","N.HPF","STEREO","MIX"},
/* Granulator */ {"TUNE","DENS","SIZE","POS","SCAN","SPRD","AMNT","MIX"},
/* OneShot    */ {"TUNE","STRT","LEN","LOOP","ATK","DEC","LEV","PAN"},
/* Polysamp   */ {"TUNE","VIBR","SPD","FADE","STRT","LEN","LEV","PAN"},
/* Slicer     */ {"TUNE","MODE","STRT","LEN","L.ST","ATK","DEC","LEV"},
/* Wavescan   */ {"TUNE","POS","A.POS","ANIM","SPD","DTUN","LEV","MIX"},
/* Tilt       */ {"LO.G","LO.F","HI.F","HI.G","WDTH","DRV","LEV","MIX"},
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

/* How a stage is named wherever one has to fit in a few characters: lock
 * labels, and the UIs that read them back. Indexed by stage, so it cannot
 * disagree with the stage order the rest of the engine uses. */
static const char *const STAGE_TAG[WORK_STAGES] = { "SRC", "FX1", "FX2" };

/* The suffix each stage answers to on the keys that carry one — "labels_src" /
 * "labels1" / "labels2", "eff_src" / "eff1" / "eff2", and the flat state
 * mirrors. The emitting counterpart to parse_stage_suffix, kept next to
 * STAGE_TAG so the two cannot drift apart. */
static const char *const STAGE_SFX[WORK_STAGES] = { "_src", "1", "2" };

/* The voice filter's seven fields, in edit-page and lock-index order. */
static const char *const VFILT_NAME[WORK_LOCK_VF_COUNT] = {
    "BASE", "WDTH", "RESO", "ENV", "ATK", "DEC", "KEY"
};

/* "SRC:TUNE" for the source stage's knob A, "FX2:MACH" for the second insert's
 * machine select, "LEVEL" for the track level. The label follows whichever
 * machine the stage holds.
 *
 * Derived from the decode helpers rather than a hand-written chain of cases.
 * The chain is how the last stage added ended up labelled "?" — it was added to
 * the map and to the UI and not to this function, and nothing failed until a
 * lock was displayed on hardware.
 *
 * Every index below WORK_LOCKABLE must produce a real label. A "?" here means
 * the map grew and this function did not, which is a bug rather than a display
 * quirk — test_every_lock_index_has_a_label is the guard. */
int work_lock_label(work_t *w, int index, char *buf, int buf_len) {
    if (!w || !buf || buf_len <= 1) return -1;
    int cap = buf_len - 1;

    if (index < 0 || index >= WORK_LOCKABLE)
        return nclamp(snprintf(buf, (size_t)buf_len, "?"), cap);
    if (index == WORK_LOCK_LEVEL)
        return nclamp(snprintf(buf, (size_t)buf_len, "LEVEL"), cap);
    if (index == WORK_LOCK_PAN)
        return nclamp(snprintf(buf, (size_t)buf_len, "PAN"), cap);

    int mstage = work_lock_decode_machine(index);
    if (mstage >= 0)
        return nclamp(snprintf(buf, (size_t)buf_len, "%s:MACH",
                               STAGE_TAG[mstage]), cap);

    int vf = work_lock_decode_vfilt(index);
    if (vf >= 0)
        return nclamp(snprintf(buf, (size_t)buf_len, "VF:%s", VFILT_NAME[vf]), cap);

    int knob = 0;
    int stage = work_lock_decode(index, &knob);
    if (stage < 0) return nclamp(snprintf(buf, (size_t)buf_len, "?"), cap);
    const char *nm = PARAM_NAME[TRK(w)->cfg[stage].machine][knob];
    if (!nm[0]) nm = "-";
    return nclamp(snprintf(buf, (size_t)buf_len, "%s:%s", STAGE_TAG[stage], nm), cap);
}

/* Default parameter values per machine. Chosen so that loading a machine and
 * touching nothing produces something musical rather than silence or a
 * screaming feedback path. */
static const uint8_t PARAM_DEFAULT[WORK_FX_COUNT][WORK_PARAMS] = {
/* Bypass     */ {0,0,0,0,0,0,0,0},
/* Clock      */ {76,48,32,0,0,127,32,64},
/* Comb       */ {32,0,0,64,80,80,96,64},
/* Comp       */ {80,24,64,64,32,0,64,127},
/* Chain      */ {32,32,48,64,16,64,64,48},
/* Decimator  */ {127,32,127,0,32,0,0,32},
/* Grit       */ {48,0,64,110,0,64,32,80},
/* Fold       */ {64,16,40,64,90,32,8,24},
/* Filterbank */ {64,64,64,64,64,64,64,64},
/* Bender     */ {24,0,0,64,64,64,0,64},
/* Flanger    */ {70,72,32,64,96,0,0,0},
/* LPF        */ {32,0,0,64,96,32,64,0},
/* MMF        */ {0,48,64,40,90,40,0,64},
/* Chorus     */ {48,40,16,80,64,0,0,0},
/* PhsArray   */ {36,64,64,72,56,48,64,64},
/* Roomtone   */ {16,48,64,72,24,16,0,0},
/* DrvDelay   */ {32,0,64,56,16,96,48,0},
/* IronRoom   */ {64,72,72,16,80,64,24,48},
/* Voidspace  */ {16,72,72,64,16,110,48,0},
/* Flutter    */ {40,40,48,72,16,64,64,64},
/* Granulator */ {64,72,40,24,68,24,80,80},
/* OneShot    */ {  64,    0,  127,    0,    0,  127,  100,   64},
/* Polysamp   */ {  64,    0,   40,   40,    0,  127,  100,   64},
/* Slicer     */ {  64,    0,    0,  127,    0,    0,  127,  100},
/* Wavescan   */ {  64,   32,   64,    0,   40,   68,  100,  127},
/* Tilt       */ {  64,   40,   80,   64,   64,    0,  100,  127},
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
    work_t        *w;
    /* The track this stage belongs to. Explicit, because the render path walks
     * EVERY track while the parameter path addresses the SELECTED one: a
     * machine reaching for TRK(w) would read whichever track the UI happens to
     * be pointed at, and eight tracks would quietly share track 0's sample and
     * voice filter. */
    work_track_t  *tr;
    work_slot_t   *s;
    const uint8_t *p;
    int            frames;
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

/* --- A.3.2 Clock Pitch ---------------------------------------------------
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

/* --- A.3.3 Comb Filter ------------------------------------------------
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

/* --- A.3.5 Chain Delay ----------------------------------------------------
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

/* --- A.3.6 Decimator -------------------------------------------------------
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

/* --- A.3.7 Gritshaper -----------------------------------------------------
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

/* --- A.3.8 Fold Filter --------------------------------------------------
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

/* --- A.3.10 Spectrum Bender ----------------------------------------------
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

/* --- A.3.11 Endless Flanger ----------------------------------------------
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

/* --- A.3.14 Wide Chorus ---------------------------------------------- */
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

/* --- A.3.15 Phase Array ------------------------------------------------------
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

/* --- A.3.16 Roomtone Reverb ----------------------------------------------
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

/* --- A.3.18 Iron Room Reverb ----------------------------------------------
 * A Dattorro figure-of-eight plate, which is a genuinely different animal from
 * Roomtone's comb tank: input diffusion into a single loop that crosses over
 * between two branches, with a modulated allpass in each to break up the
 * metallic ringing a static plate develops. Output is tapped from several
 * points inside the loop rather than from the loop output, which is what gives
 * a plate its dense, early-arriving stereo.
 *
 * Tunings are the classic ones, scaled by SIZE.
 */
static const int PL_DIFF_LEN[WORK_PLATE_DIFF] = {142, 107, 379, 277};
static const int PL_AP_LEN[WORK_PLATE_APS]    = {672, 1800, 908, 2656};
static const int PL_DEL_LEN[WORK_PLATE_DELS]  = {4453, 3720, 4217, 3163};

/* Allpass through a dedicated line. `mod` shifts the read point for the two
 * modulated tank allpasses. */
static float plate_ap(float *line, int cap, int *w, int len, float g,
                      float in, float mod) {
    len = iclamp(len, 8, cap - 2);
    float rp = (float)*w - (float)len + mod;
    while (rp < 0.0f) rp += (float)cap;
    while (rp >= (float)cap) rp -= (float)cap;
    int   i0 = (int)rp;
    float fr = rp - (float)i0;
    int   i1 = (i0 + 1) % cap;
    float dly = line[i0] * (1.0f - fr) + line[i1] * fr;

    float v = in + dly * g;
    line[*w] = v;
    *w = (*w + 1) % cap;
    return dly - v * g;
}

static float plate_tap(const float *line, int cap, int w, int len) {
    len = iclamp(len, 1, cap - 1);
    int i = w - len;
    while (i < 0) i += cap;
    return line[i];
}

static void m_steelbox(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    float pl, pr;
    predelay(s, *l, *r, 1.0f + p01(p[3]) * (float)(WORK_PRE_LEN - 8), &pl, &pr);
    float in = (pl + pr) * 0.5f;

    /* BRIT is the loop's brightness: input bandwidth and in-loop damping */
    float brit = p01(p[2]);
    in = op_lp(&s->pl_bw, in, op_a(300.0f + brit * 15000.0f));

    /* DIFF sets how hard the input diffusers smear the transient */
    float size = 0.4f + p01(p[0]) * 1.6f;
    float dg   = 0.45f + p01(p[5]) * 0.3f;
    for (int i = 0; i < WORK_PLATE_DIFF; ++i) {
        int len = (int)((float)PL_DIFF_LEN[i] * size);
        in = plate_ap(s->pl_diff + i * WORK_PLATE_DLEN, WORK_PLATE_DLEN,
                      &s->pl_diff_w[i], len, i < 2 ? dg : dg * 0.85f, in, 0.0f);
    }

    /* a slow LFO detunes the two tank allpasses so the plate never settles */
    s->pl_lfo += 0.7f / (float)WORK_SR;
    if (s->pl_lfo >= 1.0f) s->pl_lfo -= 1.0f;
    float mod = sinf(s->pl_lfo * 2.0f * (float)M_PI) * 8.0f;

    float decay = 0.30f + p01(p[1]) * 0.68f;
    float damp  = op_a(500.0f + brit * 14000.0f);

    /* branch A takes the input plus branch B's output, and vice versa —
     * the figure of eight */
    float a = in + s->pl_b * decay;
    a = plate_ap(s->pl_ap + 0 * WORK_PLATE_APLEN, WORK_PLATE_APLEN,
                 &s->pl_ap_w[0], (int)(PL_AP_LEN[0] * size), -0.7f, a, mod);
    s->pl_del[0 * WORK_PLATE_DELEN + s->pl_del_w[0]] = a;
    a = plate_tap(s->pl_del + 0 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                  s->pl_del_w[0], (int)(PL_DEL_LEN[0] * size));
    s->pl_del_w[0] = (s->pl_del_w[0] + 1) % WORK_PLATE_DELEN;
    a = op_lp(&s->pl_damp[0], a, damp) * decay;
    a = plate_ap(s->pl_ap + 1 * WORK_PLATE_APLEN, WORK_PLATE_APLEN,
                 &s->pl_ap_w[1], (int)(PL_AP_LEN[1] * size), 0.5f, a, 0.0f);
    s->pl_del[1 * WORK_PLATE_DELEN + s->pl_del_w[1]] = a;
    float a_out = plate_tap(s->pl_del + 1 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                            s->pl_del_w[1], (int)(PL_DEL_LEN[1] * size));
    s->pl_del_w[1] = (s->pl_del_w[1] + 1) % WORK_PLATE_DELEN;

    float b = in + s->pl_a * decay;
    b = plate_ap(s->pl_ap + 2 * WORK_PLATE_APLEN, WORK_PLATE_APLEN,
                 &s->pl_ap_w[2], (int)(PL_AP_LEN[2] * size), -0.7f, b, -mod);
    s->pl_del[2 * WORK_PLATE_DELEN + s->pl_del_w[2]] = b;
    b = plate_tap(s->pl_del + 2 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                  s->pl_del_w[2], (int)(PL_DEL_LEN[2] * size));
    s->pl_del_w[2] = (s->pl_del_w[2] + 1) % WORK_PLATE_DELEN;
    b = op_lp(&s->pl_damp[1], b, damp) * decay;
    b = plate_ap(s->pl_ap + 3 * WORK_PLATE_APLEN, WORK_PLATE_APLEN,
                 &s->pl_ap_w[3], (int)(PL_AP_LEN[3] * size), 0.5f, b, 0.0f);
    s->pl_del[3 * WORK_PLATE_DELEN + s->pl_del_w[3]] = b;
    float b_out = plate_tap(s->pl_del + 3 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                            s->pl_del_w[3], (int)(PL_DEL_LEN[3] * size));
    s->pl_del_w[3] = (s->pl_del_w[3] + 1) % WORK_PLATE_DELEN;

    s->pl_a = sane(a_out);
    s->pl_b = sane(b_out);

    /* Tap each output from points INSIDE the opposite branch — this is what
     * makes a plate arrive early and wide instead of swelling. */
    float wl = plate_tap(s->pl_del + 2 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                         s->pl_del_w[2], (int)(266 * size))
             + plate_tap(s->pl_del + 2 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                         s->pl_del_w[2], (int)(2974 * size))
             - plate_tap(s->pl_ap + 3 * WORK_PLATE_APLEN, WORK_PLATE_APLEN,
                         s->pl_ap_w[3], (int)(1913 * size))
             + plate_tap(s->pl_del + 3 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                         s->pl_del_w[3], (int)(1996 * size));
    float wr = plate_tap(s->pl_del + 0 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                         s->pl_del_w[0], (int)(353 * size))
             + plate_tap(s->pl_del + 0 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                         s->pl_del_w[0], (int)(3627 * size))
             - plate_tap(s->pl_ap + 1 * WORK_PLATE_APLEN, WORK_PLATE_APLEN,
                         s->pl_ap_w[1], (int)(1228 * size))
             + plate_tap(s->pl_del + 1 * WORK_PLATE_DELEN, WORK_PLATE_DELEN,
                         s->pl_del_w[1], (int)(2673 * size));
    wl *= 0.75f;
    wr *= 0.75f;

    /* LOWC trims the rumble the loop accumulates */
    svf_co_t hc;
    svf_coeffs(&hc, pexp(p[6], 20.0f, 1500.0f), 0.707f);
    wl = svf_hp(&s->f1[0], &hc, wl);
    wr = svf_hp(&s->f1[1], &hc, wr);

    float wdt  = p01(p[4]);
    float mid  = (wl + wr) * 0.5f;
    float side = (wl - wr) * 0.5f * (0.2f + wdt * 1.8f);
    wl = mid + side;
    wr = mid - side;

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + wl * mix;
    *r = *r * (1.0f - mix) + wr * mix;
}

/* --- A.3.17 Drive Delay -----------------------------------------------
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

/* --- A.3.19 Voidspace Reverb ----------------------------------------------
 * A Householder feedback delay network. Eight mutually-prime delay lines are
 * mixed by an orthogonal matrix every sample, which builds echo density far
 * faster than a comb bank and is why this one goes from small room to huge
 * without the fluttering a Schroeder tank gets at long decays.
 *
 * FREQ and GAIN form the shelving filter inside the loop, exactly as the
 * manual describes: at full GAIN the treble stays in the tail, lowering it
 * damps progressively above FREQ.
 */
static const int FDN_LEN[WORK_FDN_LINES] = {
    1063, 1291, 1583, 1867, 2153, 2411, 2719, 3011   /* mutually prime */
};

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

    float size  = 0.35f + p01(p[1]) * 1.65f;
    float decay = 0.72f + p01(p[1]) * 0.27f;
    float shelf = op_a(pexp(p[2], 400.0f, 16000.0f));
    float keep  = p01(p[3]);          /* GAIN: how much treble survives */

    float y[WORK_FDN_LINES];
    float sum = 0.0f;
    for (int i = 0; i < WORK_FDN_LINES; ++i) {
        int len = iclamp((int)((float)FDN_LEN[i] * size), 16, WORK_FDN_LEN - 1);
        int rp  = s->fdn_w[i] - len;
        if (rp < 0) rp += WORK_FDN_LEN;
        y[i] = s->fdn[i * WORK_FDN_LEN + rp];
        sum += y[i];
    }

    /* Householder: out_i = y_i - (2/N) * sum. Orthogonal, so it mixes without
     * gain, and it is one multiply per line rather than N. */
    float hh = sum * (2.0f / (float)WORK_FDN_LINES);

    for (int i = 0; i < WORK_FDN_LINES; ++i) {
        float v = y[i] - hh;
        /* shelving damper in the loop: blend the full-band signal with its
         * low-passed copy, GAIN deciding how much treble comes back */
        float lp = op_lp(&s->fdn_shelf[i], v, shelf);
        v = lp + (v - lp) * keep;

        float inj = (i & 1) ? pr : pl;
        s->fdn[i * WORK_FDN_LEN + s->fdn_w[i]] = sane(inj * 0.5f + v * decay);
        s->fdn_w[i] = (s->fdn_w[i] + 1) % WORK_FDN_LEN;
    }

    /* alternate lines to each side keeps the tail wide */
    /* Output gain sits OUTSIDE the loop, so this is a level match against the
     * other two reverbs, not a stability change. */
    float wl = (y[0] + y[2] + y[4] + y[6]) * 0.9f;
    float wr = (y[1] + y[3] + y[5] + y[7]) * 0.9f;

    float mix = p01(p[6]);
    *l = *l * (1.0f - mix) + wl * mix;
    *r = *r * (1.0f - mix) + wr * mix;
}

/* --- A.3.20 Flutter --------------------------------------------------------
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

/* --- Granulator (A.2.4, adapted) ---------------------------------------------
 * The reference design granulates a LOADED SAMPLE. Work granulates the rolling
 * buffer of whatever is coming in instead — the last two seconds of live audio
 * stand in for the sample when no sample is loaded.
 *
 * Everything else follows the manual's semantics: POS is where in the buffer
 * grains start, SCAN moves that point forward or backward (and wraps), SPRD
 * randomises each grain's start, SIZE is the grain length from 10 ms up, DENS
 * governs how often grains launch, AMNT how many may sound at once, and TUNE
 * is bipolar over +/- 2 octaves.
 *
 * Not carried across, and worth knowing: the AMNT/DIR/MODE and PAN controls of
 * SRC page 2, and the FADE/SHAPE window pair — grains use a fixed Hann window
 * here, which is the smooth end of what SHAPE offers.
 *
 * SOURCE (Tier B): if a sample is loaded, the grains read THAT instead of the
 * live input, which is what makes this a real SRC machine rather than a
 * live-input effect. POS and SCAN then scan the sample rather than the last
 * two seconds of history. With no sample loaded it falls back to the live
 * input exactly as before, so an existing patch sounds unchanged.
 */
static void m_granulator(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    /* The buffer under the grains is a loaded sample when there is one, and
     * the live input otherwise. The ring keeps recording either way, so
     * clearing the sample returns to live granulation with history intact. */
    s->dl[s->dw * 2]     = *l;
    s->dl[s->dw * 2 + 1] = *r;

    const int   frames = m->tr->sample_frames;
    const int   from_sample = (frames > 4 && m->tr->sample != NULL);

    float rate  = powf(2.0f, pbi(p[0]) * 2.0f);                  /* TUNE +/-2 oct */
    float size  = fclampf(0.01f + p01(p[2]) * 1.2f, 0.01f, 1.4f) * (float)WORK_SR;
    float dens  = p01(p[1]);
    float posf  = p01(p[3]);
    float scan  = pbi(p[4]);
    float sprd  = p01(p[5]);
    int   amnt  = iclamp(1 + (int)(p01(p[6]) * (WORK_GRAINS - 1) + 0.5f), 1, WORK_GRAINS);

    /* SCAN walks the base read point through the buffer and wraps, so a small
     * positive value drifts forward through recent history. */
    s->gr_scan += scan * 4.0f;
    float span = (float)WORK_DLY_LEN - size - 4.0f;
    if (span < 16.0f) span = 16.0f;
    while (s->gr_scan >= span)   s->gr_scan -= span;
    while (s->gr_scan < 0.0f)    s->gr_scan += span;

    float base = 2.0f + posf * (span * 0.85f) + s->gr_scan;

    /* Launch interval: dense settings overlap grains heavily, sparse ones
     * leave gaps. Referenced to grain length so SIZE and DENS interact the
     * way the manual describes for OSC mode. */
    float interval = fmaxf(size * (1.05f - dens), 32.0f);
    s->gr_next -= 1.0f;
    if (s->gr_next <= 0.0f) {
        s->gr_next = interval;
        int live = 0;
        for (int g = 0; g < WORK_GRAINS; ++g) if (s->grain[g].on) live++;
        if (live < amnt) {
            for (int g = 0; g < WORK_GRAINS; ++g) {
                if (s->grain[g].on) continue;
                float jitter = sprd * rnd_01(&s->rng) * span * 0.5f;
                s->grain[g].on   = 1;
                s->grain[g].pos  = fclampf(base + jitter, 2.0f, (float)(WORK_DLY_LEN - 4));
                s->grain[g].step = rate;
                s->grain[g].age  = 0.0f;
                s->grain[g].len  = size;
                /* a little pan scatter keeps a grain cloud from collapsing */
                float pan = 0.5f + (rnd_bi(&s->rng) * sprd * 0.5f);
                pan = fclampf(pan, 0.0f, 1.0f);
                s->grain[g].gl = sqrtf(1.0f - pan);
                s->grain[g].gr = sqrtf(pan);
                break;
            }
        }
    }

    float wl = 0.0f, wr = 0.0f;
    for (int g = 0; g < WORK_GRAINS; ++g) {
        if (!s->grain[g].on) continue;

        float t = s->grain[g].age / s->grain[g].len;
        if (t >= 1.0f) { s->grain[g].on = 0; continue; }

        /* Hann window — the smooth end of the described fade/shape pair */
        float win = 0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI);

        /* Read behind the write head. The grain's own advance is (step - 1)
         * relative to the head, so a rate of 1 holds a fixed offset. */
        float back = s->grain[g].pos - s->grain[g].age * (s->grain[g].step - 1.0f);
        back = fclampf(back, 2.0f, (float)(WORK_DLY_LEN - 4));

        if (from_sample) {
            /* `back` is an offset behind a notional head; map it onto the
             * sample by treating the buffer span as the sample length. Every
             * index is clamped against sample_frames rather than trusted — a
             * transfer can shorten the sample between blocks. */
            float scaled = back * (float)frames / (float)WORK_DLY_LEN;
            int   i0 = (int)scaled;
            if (i0 < 0) i0 = 0;
            if (i0 >= frames - 1) i0 = frames - 2;
            float fr = scaled - (float)i0;
            const float a0 = m->tr->sample[i0 * 2]           / 32768.0f;
            const float a1 = m->tr->sample[(i0 + 1) * 2]     / 32768.0f;
            const float b0 = m->tr->sample[i0 * 2 + 1]       / 32768.0f;
            const float b1 = m->tr->sample[(i0 + 1) * 2 + 1] / 32768.0f;
            wl += (a0 + (a1 - a0) * fr) * win * s->grain[g].gl;
            wr += (b0 + (b1 - b0) * fr) * win * s->grain[g].gr;
        } else {
            wl += dl_read(s->dl, WORK_DLY_LEN, s->dw, back, 0) * win * s->grain[g].gl;
            wr += dl_read(s->dl, WORK_DLY_LEN, s->dw, back, 1) * win * s->grain[g].gr;
        }

        s->grain[g].age += 1.0f;
    }

    /* Overlapping Hann grains sum to roughly unity; normalise by the count
     * allowed rather than the count sounding, so density changes do not jump. */
    float norm = 1.4f / sqrtf((float)amnt);
    wl *= norm;
    wr *= norm;

    s->dw = (s->dw + 1) % WORK_DLY_LEN;

    float mix = p01(p[7]);
    *l = *l * (1.0f - mix) + wl * mix;
    *r = *r * (1.0f - mix) + wr * mix;
}

/* ------------------------------------------------- 5. dispatch and plumbing */

static void slot_reset(work_slot_t *s) {
    /* Clear everything except the allocated buffers themselves. */
    float *dl = s->dl, *pre = s->pre, *comb = s->comb, *ap = s->ap;
    float *pld = s->pl_diff, *pla = s->pl_ap, *plx = s->pl_del, *fdn = s->fdn;
    uint32_t rng = s->rng;
    memset(s, 0, sizeof(*s));
    s->dl = dl; s->pre = pre; s->comb = comb; s->ap = ap;
    s->pl_diff = pld; s->pl_ap = pla; s->pl_del = plx; s->fdn = fdn;
    s->rng = rng ? rng : 0x9E3779B9u;

    if (dl)   memset(dl,   0, sizeof(float) * WORK_DLY_LEN * 2);
    if (pre)  memset(pre,  0, sizeof(float) * WORK_PRE_LEN * 2);
    if (comb) memset(comb, 0, sizeof(float) * WORK_TANK_COMBS * 2 * WORK_TANK_LEN);
    if (ap)   memset(ap,   0, sizeof(float) * WORK_TANK_APS * 2 * WORK_AP_LEN);
    if (pld)  memset(pld,  0, sizeof(float) * WORK_PLATE_DIFF * WORK_PLATE_DLEN);
    if (pla)  memset(pla,  0, sizeof(float) * WORK_PLATE_APS * WORK_PLATE_APLEN);
    if (plx)  memset(plx,  0, sizeof(float) * WORK_PLATE_DELS * WORK_PLATE_DELEN);
    if (fdn)  memset(fdn,  0, sizeof(float) * WORK_FDN_LINES * WORK_FDN_LEN);
}


/* ------------------------------------------------------------ voice filter
 *
 * A band, not a ladder: BASE sets the high-pass edge and WDTH the octaves up
 * to the low-pass edge, so one control moves the whole band and the other
 * opens or closes it. That is more useful on a sample than two unrelated
 * cutoffs, because the thing you usually want is "less of the bottom, less of
 * the top" in one gesture.
 *
 * BASE 0 with WDTH at maximum is fully open, and those are the defaults, so a
 * patch that never visits this page sounds exactly as it did before the filter
 * existed.
 *
 * Coefficients are computed ONCE PER BLOCK per voice, not per sample: at 128
 * frames that is 344 updates a second, far above any envelope or key movement
 * you can hear, and tanf() per sample per voice would not fit the budget. */
static void vfilt_reset(work_vfilt_t *f) {
    for (int c = 0; c < 2; ++c) {
        f->hp1[c] = f->hp2[c] = 0.0f;
        f->lp1[c] = f->lp2[c] = 0.0f;
    }
    f->env = 0.0f;
    f->stage = 1;
}

/* Is the filter doing anything at all? A wide-open band with no envelope is
 * skipped outright, so the machines cost exactly what they used to when the
 * page is untouched. */
static int vfilt_active(const work_vfilt_cfg_t *v) {
    return v->base > 0 || v->width < 127 || v->env != 64;
}

/* Advance one voice's filter envelope by a block and derive its coefficients.
 * `note` is the note that fired the voice, for key tracking; -1 means none. */
static void vfilt_block(const work_vfilt_cfg_t *v, work_vfilt_t *f, int note,
                        int frames, svf_co_t *hp, svf_co_t *lp) {
    const float dt  = (float)frames / (float)WORK_SR;
    const float atk = pexp(v->attack, 0.001f, 4.0f);
    const float dec = pexp(v->decay,  0.005f, 8.0f);

    if (f->stage == 1) {
        f->env += dt / atk;
        if (f->env >= 1.0f) { f->env = 1.0f; f->stage = 2; }
    } else if (f->stage == 2) {
        f->env -= dt / dec;
        if (f->env <= 0.0f) { f->env = 0.0f; f->stage = 0; }
    }

    /* BASE spans 20 Hz to about 8 kHz; the envelope is bipolar around 64 so
     * it can sweep the band down as well as up. */
    float oct = (float)v->base / 127.0f * 8.6f;
    oct += ((float)v->env - 64.0f) / 63.0f * 6.0f * f->env;
    if (note >= 0 && v->track)
        oct += ((float)note - 60.0f) / 12.0f * ((float)v->track / 127.0f);

    const float fc_hp = 20.0f * powf(2.0f, fclampf(oct, 0.0f, 11.0f));
    /* WDTH 127 puts the low-pass above the band limit, i.e. open. */
    const float fc_lp = fc_hp * powf(2.0f, (float)v->width / 127.0f * 11.0f);
    const float q     = 0.7f + (float)v->reso / 127.0f * 6.0f;

    svf_coeffs(hp, fc_hp, q);
    svf_coeffs(lp, fc_lp, q);
}

/* One sample through one voice's band. */
static void vfilt_run(work_vfilt_t *f, const svf_co_t *hp, const svf_co_t *lp,
                      float *l, float *r) {
    float *io[2] = { l, r };
    for (int c = 0; c < 2; ++c) {
        svf_t h = { f->hp1[c], f->hp2[c] };
        svf_t o = { f->lp1[c], f->lp2[c] };
        float x = svf_hp(&h, hp, *io[c]);
        x = svf_lp(&o, lp, x);
        f->hp1[c] = h.ic1; f->hp2[c] = h.ic2;
        f->lp1[c] = o.ic1; f->lp2[c] = o.ic2;
        *io[c] = x;
    }
}

static int machine_is_source(int machine);

/* Refresh every voice filter's coefficients for this block. Called once per
 * work_process, never from the per-sample loop. */
static void vfilt_prepare(work_track_t *tr, int frames) {
    const int on = vfilt_active(&tr->eff_vfilt);
    for (int i = 0; i < WORK_STAGES; ++i) {
        work_slot_t *s = &tr->slot[i];
        s->vf_on = on && machine_is_source(tr->eff_machine[i]);
        if (!s->vf_on) continue;
        vfilt_block(&tr->eff_vfilt, &s->sp_filt, s->sp_note, frames, &s->sp_hp, &s->sp_lp);
        for (int v = 0; v < WORK_VOICES; ++v) {
            if (s->voice[v].stage == 0) continue;
            vfilt_block(&tr->eff_vfilt, &s->voice[v].filt, s->voice[v].note, frames,
                        &s->v_hp[v], &s->v_lp[v]);
        }
    }
}

/* ----------------------------------------------------------- voice helpers
 *
 * The sample machines are eight-voice polyphonic. A voice here is a read
 * cursor plus an envelope, so eight of them cost almost nothing next to the
 * FX machines.
 *
 * Voice allocation is oldest-first: with every voice busy, the one that has
 * been sounding longest is taken. That is the least surprising rule for a
 * sample player — the note you started first is the one you have stopped
 * caring about.
 */
static work_voice_t *voice_alloc(work_slot_t *s) {
    work_voice_t *best = &s->voice[0];
    for (int i = 0; i < WORK_VOICES; ++i) {
        if (s->voice[i].stage == 0) { best = &s->voice[i]; break; }
        if (s->voice[i].age < best->age) best = &s->voice[i];
    }
    best->age = ++s->voice_clock;
    return best;
}

/* Read the sample at a fractional frame, clamped. Every SRC machine goes
 * through this: the cursor is never trusted against sample_frames, because a
 * transfer can shorten the sample between blocks. */
/* Reads THIS TRACK's sample. The buffer belongs to the track, not the engine —
 * eight tracks each load their own audio. */
static void sample_read(const work_track_t *tr, double pos, float *l, float *r) {
    const int frames = tr->sample_frames;
    if (frames < 2 || !tr->sample) { *l = 0.0f; *r = 0.0f; return; }
    int i0 = (int)pos;
    if (i0 < 0) i0 = 0;
    if (i0 > frames - 2) i0 = frames - 2;
    const float fr = (float)(pos - (double)i0);
    const float a0 = tr->sample[i0 * 2]           / 32768.0f;
    const float a1 = tr->sample[(i0 + 1) * 2]     / 32768.0f;
    const float b0 = tr->sample[i0 * 2 + 1]       / 32768.0f;
    const float b1 = tr->sample[(i0 + 1) * 2 + 1] / 32768.0f;
    *l = a0 + (a1 - a0) * fr;
    *r = b0 + (b1 - b0) * fr;
}

/* Equal-power pan, 64 = centre. */
static void pan_gains(uint8_t p, float *gl, float *gr) {
    const float a = (float)p / 127.0f * 1.57079633f;
    *gl = cosf(a) * 1.41421356f;
    *gr = sinf(a) * 1.41421356f;
}

/* ------------------------------------------------------------ Polysample
 *
 * The manual describes Polysample as an eight-voice polyphonic player of
 * multi-sampled instruments, with TUNE, VIBR (vibrato depth), SPD (vibrato
 * speed) and FADE (vibrato fade-in).
 *
 * Not carried across: multi-sampled INSTRUMENTS. The reference design maps a
 * set of samples across the keyboard from a memory card; Work has one sample
 * buffer, so every note plays that one sample transposed. The polyphony,
 * the vibrato and the parameter set are real; the multisampling is not, and
 * calling that out matters more than pretending otherwise.
 *
 * The remaining four knobs carry STRT/LEN/LEV/PAN so the machine is playable
 * without a second page, which Work has no room for.
 */
static void m_multi(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    const int frames = m->tr->sample_frames;
    if (frames < 2 || !m->tr->sample) { *l = 0.0f; *r = 0.0f; return; }

    int start = (int)((double)p[4] / 127.0 * (frames - 1));
    int len   = (int)((double)p[5] / 127.0 * (frames - start));
    if (len < 2) len = 2;
    if (start + len > frames) len = frames - start;

    const float base = powf(2.0f, ((float)p[0] - 64.0f) / 12.8f);   /* +/-5 oct */
    const float vdep = (float)p[1] / 127.0f * 0.06f;                /* semitone-ish */
    const float vspd = 0.5f + (float)p[2] / 127.0f * 11.5f;         /* Hz */
    const float fade = 0.01f + (float)p[3] / 127.0f * 3.0f;         /* seconds */
    const float lev  = (float)p[6] / 127.0f;
    float gl, gr; pan_gains(p[7], &gl, &gr);

    float ol = 0.0f, orr = 0.0f;
    for (int i = 0; i < WORK_VOICES; ++i) {
        work_voice_t *v = &s->voice[i];
        if (v->stage == 0) continue;

        /* Vibrato fades in over FADE, which is what the manual describes. */
        v->vib_fade += 1.0f / (fade * (float)WORK_SR);
        if (v->vib_fade > 1.0f) v->vib_fade = 1.0f;
        v->vib_ph += vspd / (float)WORK_SR;
        if (v->vib_ph >= 1.0f) v->vib_ph -= 1.0f;
        const float vib = sinf(v->vib_ph * 6.28318531f) * vdep * v->vib_fade;

        float sl, sr;
        sample_read(m->tr, v->pos, &sl, &sr);
        if (s->vf_on) vfilt_run(&v->filt, &s->v_hp[i], &s->v_lp[i], &sl, &sr);
        ol += sl * v->env * v->gain;
        orr += sr * v->env * v->gain;

        v->pos += (double)(v->rate * base * powf(2.0f, vib));
        if (v->pos >= start + len || v->pos < start) { v->stage = 0; v->env = 0.0f; }

        /* short AD so a released note does not click */
        if (v->stage == 1) {
            v->env += 1.0f / (0.002f * (float)WORK_SR);
            if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; }
        }
    }

    /* Per VOICE, above, not here — one filter on the sum would make every new
     * note's envelope sweep the notes already sounding. */
    *l = ol * lev * gl;
    *r = orr * lev * gr;
}

/* -------------------------------------------------------------- Slicer
 *
 * The manual's Slicer machine loads eight samples onto eight sequencer
 * subtracks, each with its own SRC/FLTR/AMP/MOD pages, plus a supertrack for
 * the shared FX parameters.
 *
 * That architecture is NOT reproduced, and cannot be: Work is a two-slot FX
 * chain, not an eight-track sampler with per-track parameter pages. What IS
 * reproduced is the machine's documented PLAYBACK parameter set, which is the
 * part that makes a sound: TUNE, PLAY MODE (forward / reverse / forward loop /
 * reverse loop), STRT, LEN and L.ST (loop start).
 *
 * That is a real gain over One Shot — reverse playback and a separate
 * loop point are both things One Shot cannot do — so the machine earns
 * its slot even without the routing. The docs say so in as many words rather
 * than letting the name imply the rest.
 */
enum { SUB_FWD = 0, SUB_REV, SUB_FWD_LOOP, SUB_REV_LOOP };

static void m_subtracks(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    const int frames = m->tr->sample_frames;
    if (frames < 2 || !m->tr->sample) { *l = 0.0f; *r = 0.0f; return; }

    const int mode = iclamp((int)p[1] * 4 / 128, 0, 3);

    int start = (int)((double)p[2] / 127.0 * (frames - 1));
    int len   = (int)((double)p[3] / 127.0 * (frames - start));
    if (len < 2) len = 2;
    if (start + len > frames) len = frames - start;
    const int end = start + len;
    /* L.ST is the point playback returns to, between STRT and the end. */
    int lst = start + (int)((double)p[4] / 127.0 * (len - 1));
    if (lst < start) lst = start;
    if (lst > end - 2) lst = end - 2;

    const float rate = powf(2.0f, ((float)p[0] - 64.0f) / 12.8f);   /* +/-5 oct */
    const float atk  = 0.0005f + (float)p[5] / 127.0f * 2.0f;
    const float dec  = 0.0100f + (float)p[6] / 127.0f * 8.0f;
    const float lev  = (float)p[7] / 127.0f;

    float ol = 0.0f, orr = 0.0f;
    for (int i = 0; i < WORK_VOICES; ++i) {
        work_voice_t *v = &s->voice[i];
        if (v->stage == 0) continue;

        float sl, sr;
        sample_read(m->tr, v->pos, &sl, &sr);
        if (s->vf_on) vfilt_run(&v->filt, &s->v_hp[i], &s->v_lp[i], &sl, &sr);
        ol += sl * v->env * v->gain;
        orr += sr * v->env * v->gain;

        v->pos += (double)(v->rate * rate) * (double)v->dir;

        if (v->dir > 0 && v->pos >= end) {
            if (mode == SUB_FWD_LOOP) v->pos = lst;
            else { v->stage = 0; v->env = 0.0f; }
        } else if (v->dir < 0 && v->pos <= lst) {
            if (mode == SUB_REV_LOOP) v->pos = end - 1;
            else if (v->pos <= start) { v->stage = 0; v->env = 0.0f; }
        }

        if (v->stage == 1) {
            v->env += 1.0f / (atk * (float)WORK_SR);
            if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; }
        } else if (v->stage == 2) {
            v->env -= 1.0f / (dec * (float)WORK_SR);
            if (v->env <= 0.0f) { v->env = 0.0f; v->stage = 0; }
        }
    }

    *l = ol * lev;
    *r = orr * lev;
}

/* ------------------------------------------------------------- Wavescan
 *
 * Two wavetable oscillators blended together, each with a position into the
 * table (POS), an internal modulator that animates that position (ANIM, SPD,
 * A.POS) and a detune between them (DTUN). Moving through the table is
 * interpolated, which is what makes it sound like wavetable synthesis rather
 * than switching.
 *
 * WHERE THE TABLE COMES FROM is the departure. The reference design browses
 * wavetable files on a memory card and holds 127 per project in numbered
 * slots. Work has no card and no slot store — but it has a loaded sample,
 * so the sample IS
 * the wavetable: it is read as a series of WF_WAVE-frame waves and POS selects
 * between them, interpolating across the boundary. Load a wavetable file and
 * it behaves as one; load a drum loop and you get something else entirely,
 * which is a fair trade for not having a browser.
 *
 * ANIM shapes, in the manual's order: one-shot exp down / ramp down / tri /
 * ramp up / exp up, then looping ramp down / tri / square / ramp up, random,
 * and sample-and-hold.
 */
#define WF_WAVE 2048

static float wf_anim_shape(int shape, float t, float *sh, uint32_t *rng) {
    switch (shape) {
        case 0:  return expf(-4.0f * t);                       /* exp down 1shot */
        case 1:  return 1.0f - t;                              /* ramp down      */
        case 2:  return t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
        case 3:  return t;                                     /* ramp up        */
        case 4:  return 1.0f - expf(-4.0f * t);                /* exp up         */
        case 5:  return 1.0f - t;                              /* ramp down loop */
        case 6:  return t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
        case 7:  return t < 0.5f ? 1.0f : 0.0f;                /* square loop    */
        case 8:  return t;                                     /* ramp up loop   */
        case 9:  return rnd_01(rng);                           /* random         */
        default:                                               /* sample & hold  */
            if (t < 0.02f) *sh = rnd_01(rng);
            return *sh;
    }
}

static void m_wavefinder(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    const int frames = m->tr->sample_frames;
    if (frames < WF_WAVE * 2 || !m->tr->sample) { *l = 0.0f; *r = 0.0f; return; }

    const int waves = frames / WF_WAVE;
    const float tune = powf(2.0f, ((float)p[0] - 64.0f) / 32.0f);
    const float dtun = powf(2.0f, ((float)p[5] - 64.0f) / 256.0f);   /* fine */
    const float posk = (float)p[1] / 127.0f;
    const float apos = ((float)p[2] - 64.0f) / 64.0f;                /* bipolar */
    const int   anim = iclamp((int)p[3] * 11 / 128, 0, 10);
    const float spd  = 0.05f + (float)p[4] / 127.0f * 8.0f;          /* Hz */
    const float lev  = (float)p[6] / 127.0f;
    const float mix  = (float)p[7] / 127.0f;

    /* One wave is WF_WAVE frames, so a "note" is SR / WF_WAVE Hz at rate 1. */
    const float step = tune * 1.0f;

    float ol = 0.0f, orr = 0.0f;
    for (int o = 0; o < 2; ++o) {
        const float rate = (o == 0) ? step : step * dtun;

        s->wf_anim[o] += spd / (float)WORK_SR;
        if (s->wf_anim[o] >= 1.0f) s->wf_anim[o] -= 1.0f;
        const float a = wf_anim_shape(anim, s->wf_anim[o], &s->wf_sh[o], &s->rng);

        float pos = posk + apos * a;
        if (pos < 0.0f) pos = 0.0f;
        if (pos > 1.0f) pos = 1.0f;

        /* Interpolate ACROSS waves as well as within one, which is what the
         * manual means by "transitioning between waves is interpolated". */
        const float wf = pos * (float)(waves - 1);
        const int   w0 = iclamp((int)wf, 0, waves - 2);
        const float wfr = wf - (float)w0;

        s->wf_ph[o] += (double)rate;
        while (s->wf_ph[o] >= (double)WF_WAVE) s->wf_ph[o] -= (double)WF_WAVE;

        float al, ar, bl, br;
        sample_read(m->tr, (double)(w0 * WF_WAVE) + s->wf_ph[o], &al, &ar);
        sample_read(m->tr, (double)((w0 + 1) * WF_WAVE) + s->wf_ph[o], &bl, &br);

        const float vl = al + (bl - al) * wfr;
        const float vr = ar + (br - ar) * wfr;
        ol += vl * 0.5f;
        orr += vr * 0.5f;
    }

    /* No note voices here — two oscillators, not eight cursors — so the
     * filter treats the machine's output using the slot's own state. It is
     * still the same band and the same envelope. */
    if (s->vf_on) vfilt_run(&s->sp_filt, &s->sp_hp, &s->sp_lp, &ol, &orr);

    *l = (*l) * (1.0f - mix) + ol * lev * mix;
    *r = (*r) * (1.0f - mix) + orr * lev * mix;
}

/* Shelf gain from a bipolar 0..127 knob, 64 = flat. See the table in m_shape. */
static float shelf_gain(uint8_t v) {
    if (v >= 64) {
        const float db = ((float)v - 64.0f) / 63.0f * 12.0f;
        return powf(10.0f, db / 20.0f);
    }
    const float t = (float)v / 64.0f;      /* 0 at full cut, 1 at flat */
    return t * t;                          /* -12 dB at t = 0.5, silence at 0 */
}

/* ------------------------------------------------------------------- Tilt
 *
 * A low shelf and a high shelf, each with a frequency and a gain, where 64
 * turns the shelf into a full high-pass or low-pass response. It produces no
 * sound of its own — it shapes what is already on the bus.
 *
 * That maps onto Work exactly, because a Work insert IS a bus insert: the one
 * machine here that needed no adaptation at all. The remaining knobs carry
 * stereo width, a little drive, level and mix.
 *
 * It is an EFFECT, not a source: it has nothing to play. See
 * machine_in_src_family.
 */
static void m_shape(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;
    const uint8_t *p = m->p;

    /* Shelf frequencies, 30 Hz - 1 kHz low and 500 Hz - 12 kHz high. */
    const float lof = 30.0f * powf(1000.0f / 30.0f, (float)p[1] / 127.0f);
    const float hif = 500.0f * powf(12000.0f / 500.0f, (float)p[2] / 127.0f);

    /* The documented scale is bipolar with a HARD end, and the ends are the
     * point of the control:
     *
     *     -64  highpass response   (the low band is GONE, not merely reduced)
     *     -32  -12 dB
     *     -16   -6 dB
     *       0  flat
     *     +32   +6 dB
     *     +63  +12 dB
     *
     * Capping the cut at -12 dB made the machine feel tame — reported from
     * hardware as "it's not extreme, but able to cut lows or highs". Boost
     * stays linear in dB; cut runs quadratically to SILENCE, which puts -12 dB
     * at half travel and -6 dB at three quarters, matching the table, and
     * turns the last of the travel into the full pass the manual describes. */
    const float lgain = shelf_gain(p[0]);
    const float hgain = shelf_gain(p[3]);

    const float wid = (float)p[4] / 127.0f * 2.0f;      /* 0 mono, 1 as-is, 2 wide */
    const float drv = 1.0f + (float)p[5] / 127.0f * 6.0f;
    const float lev = (float)p[6] / 127.0f;
    const float mix = (float)p[7] / 127.0f;

    const float alo = expf(-6.28318531f * lof / (float)WORK_SR);
    const float ahi = expf(-6.28318531f * hif / (float)WORK_SR);

    float in[2] = { *l, *r }, out[2];
    for (int c = 0; c < 2; ++c) {
        /* ONE pole per band, deliberately.
         *
         * `in - low` is an exact complementary highpass only while `low` is a
         * single pole: the pair sums back to the input by construction. I
         * cascaded a second pole to steepen the slope and it made the extreme
         * WORSE, not better — two poles shift phase about 10 degrees at 60 Hz
         * against a 645 Hz corner, so the subtraction no longer nulls and the
         * full cut bottomed out at -14.7 dB instead of continuing down.
         * Measured, not reasoned: the cascade cost roughly 6 dB of depth at
         * the very end of the travel, which is the part of the control this
         * was supposed to improve. */
        s->sh_lo[c] = in[c] + alo * (s->sh_lo[c] - in[c]);
        s->sh_hi[c] = in[c] + ahi * (s->sh_hi[c] - in[c]);
        const float low  = s->sh_lo[c];
        const float high = in[c] - s->sh_hi[c];

        /* Standard shelving form: take the input and add back the band scaled
         * by how far the shelf departs from unity.
         *
         *   gain 1  ->  v = in                     (flat)
         *   gain 0  ->  v = in - low               (the band is GONE)
         *   gain 4  ->  v = in + 3*low             (+12 dB)
         *
         * The earlier version split the signal into low/mid/high and summed
         * them with gains, which cannot reach a true highpass: whatever the
         * low band failed to capture stayed in the middle at unity and leaked
         * through at full cut. Subtracting the band itself removes exactly the
         * band, which is what the manual's "highpass filter response" at the
         * extreme actually means. */
        float v = in[c] + low * (lgain - 1.0f) + high * (hgain - 1.0f);
        if (drv > 1.0f) v = softclip(v * drv) / drv;
        out[c] = v;
    }

    /* mid/side width */
    const float mid = (out[0] + out[1]) * 0.5f;
    const float side = (out[0] - out[1]) * 0.5f * wid;
    out[0] = mid + side;
    out[1] = mid - side;

    *l = *l * (1.0f - mix) + out[0] * lev * mix;
    *r = *r * (1.0f - mix) + out[1] * lev * mix;
}

/* ---------------------------------------------------------- One Shot
 *
 * The simplest of the source machines: one shot of the sample per trig.
 * TUNE is +/- two octaves around 64, STRT and LEN scan a window into the
 * sample, LOOP holds the window instead of stopping at its end, ATK and DEC
 * shape an AD envelope, LEV is output level and PAN places it.
 *
 * This is a SOURCE, not an insert effect: it replaces its input rather than
 * processing it, which is what makes Work's chain slot able to originate
 * audio. Nothing is read until a sample is committed, so an empty slot is
 * silent rather than noisy.
 *
 * Realtime rules: no allocation, and every read of the sample buffer is
 * clamped against sample_frames rather than trusting the cursor. A transfer
 * running concurrently only ever moves sample_fill, which this never reads. */
static void m_single(mctx_t *m, float *l, float *r) {
    work_slot_t *s = m->s;

    const int frames = m->tr->sample_frames;
    if (frames <= 0 || !m->tr->sample) { *l = 0.0f; *r = 0.0f; return; }

    /* window into the sample */
    int start = (int)((double)m->p[1] / 127.0 * (frames - 1));
    int len   = (int)((double)m->p[2] / 127.0 * (frames - start));
    if (len < 2) len = 2;
    if (start + len > frames) len = frames - start;

    const int   loop = m->p[3] >= 64;
    const float rate = powf(2.0f, ((float)m->p[0] - 64.0f) / 32.0f);  /* +/-2 oct */

    if (s->sp_stage == 0) { *l = 0.0f; *r = 0.0f; return; }

    /* AD envelope. ATK 0 is an instant start, which is what a one-shot
     * sample player normally wants; DEC 127 holds until the window ends. */
    const float atk = 0.0005f + (float)m->p[4] / 127.0f * 2.0f;   /* seconds */
    const float dec = 0.0100f + (float)m->p[5] / 127.0f * 8.0f;
    if (s->sp_stage == 1) {
        s->sp_env += 1.0f / (atk * (float)WORK_SR);
        if (s->sp_env >= 1.0f) { s->sp_env = 1.0f; s->sp_stage = 2; }
    } else {
        s->sp_env -= 1.0f / (dec * (float)WORK_SR);
        if (s->sp_env <= 0.0f) { s->sp_env = 0.0f; s->sp_stage = 0; }
    }

    /* linear interpolation between neighbouring frames */
    double pos = s->sp_pos;
    int    i0  = (int)pos;
    float  fr  = (float)(pos - i0);
    int    i1  = i0 + 1;

    if (i0 < start) i0 = start;
    if (i1 >= start + len) i1 = loop ? start : start + len - 1;
    if (i0 >= frames) i0 = frames - 1;
    if (i1 >= frames) i1 = frames - 1;

    const float a_l = m->tr->sample[i0 * 2]     / 32768.0f;
    const float a_r = m->tr->sample[i0 * 2 + 1] / 32768.0f;
    const float b_l = m->tr->sample[i1 * 2]     / 32768.0f;
    const float b_r = m->tr->sample[i1 * 2 + 1] / 32768.0f;

    float ol = a_l + (b_l - a_l) * fr;
    float orr = a_r + (b_r - a_r) * fr;

    pos += rate;
    if (pos >= start + len) {
        if (loop) pos = start + (pos - (start + len));
        else { s->sp_stage = 0; s->sp_env = 0.0f; }
    }
    s->sp_pos = pos;

    /* level and pan — equal power, 64 = centre */
    const float lev = (float)m->p[6] / 127.0f;
    const float pan = (float)m->p[7] / 127.0f;
    const float gl  = cosf(pan * 1.57079633f);
    const float gr  = sinf(pan * 1.57079633f);
    const float g   = lev * s->sp_env;

    /* One Shot is monophonic, so its one filter IS its voice filter. */
    if (s->vf_on) vfilt_run(&s->sp_filt, &s->sp_hp, &s->sp_lp, &ol, &orr);

    *l = ol * g * gl * 1.41421356f;
    *r = orr * g * gr * 1.41421356f;
}

static void run_machine(mctx_t *m, int machine, float *l, float *r) {
    switch (machine) {
        case WORK_FX_CLOCK:    m_chrono(m, l, r);    break;
        case WORK_FX_COMB:      m_comb(m, l, r);      break;
        case WORK_FX_COMP:      m_comp(m, l, r);      break;
        case WORK_FX_CHAIN:     m_daisy(m, l, r);     break;
        case WORK_FX_DECIMATOR:  m_degrader(m, l, r);  break;
        case WORK_FX_GRIT:      m_dirt(m, l, r);      break;
        case WORK_FX_FOLD:    m_folder(m, l, r);    break;
        case WORK_FX_FBANK:     m_fbank(m, l, r);     break;
        case WORK_FX_BENDER:    m_warper(m, l, r);    break;
        case WORK_FX_FLANGER:   m_flanger(m, l, r);   break;
        case WORK_FX_LPF:       m_lpf(m, l, r);       break;
        case WORK_FX_MMF:       m_mmf(m, l, r);       break;
        case WORK_FX_CHORUS:    m_chorus(m, l, r);    break;
        case WORK_FX_PHASEARRAY:   m_phase98(m, l, r);   break;
        case WORK_FX_ROOMTONE: m_rumsklang(m, l, r); break;
        case WORK_FX_DRIVEDELAY:  m_satdelay(m, l, r);  break;
        case WORK_FX_IRONROOM:  m_steelbox(m, l, r);  break;
        case WORK_FX_VOIDSPACE: m_supervoid(m, l, r); break;
        case WORK_FX_FLUTTER:    m_warble(m, l, r);    break;
        case WORK_FX_GRANULATOR:   m_granulator(m, l, r);   break;
        case WORK_FX_ONESHOT:    m_single(m, l, r);    break;
        case WORK_FX_POLYSAMPLE:     m_multi(m, l, r);     break;
        case WORK_FX_SLICER: m_subtracks(m, l, r); break;
        case WORK_FX_WAVESCAN:m_wavefinder(m, l, r);break;
        case WORK_FX_TILT:     m_shape(m, l, r);     break;
        case WORK_FX_BYPASS:
        default:                                      break;
    }
}

/* ------------------------------------------------------------- sequencer */

/* The pattern currently being edited and played. Song mode moves this
 * underneath the editor, which is why every sequencer path goes through it
 * rather than caching a pointer. */
#define CURPAT(w) (&(w)->pat[(w)->cur_pattern])

/* A lane is one track's steps inside the current pattern. LANE() names the
 * track; CURLANE() is the SELECTED track's, which is what every step, lock and
 * pattern-edit parameter addresses — the same rule as TRK(). The sequencer
 * walks all of them. */
#define LANE(w, t) (&CURPAT(w)->lane[t])
#define CURLANE(w) LANE((w), (w)->sel_track)

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

/* Re-arm every lane's edge detector, so the next block fires whatever step it
 * lands on instead of treating it as already played. Every lane, because they
 * each keep their own — see work_track_t.last_step. */
static void seq_rearm(work_t *w) {
    for (int t = 0; t < WORK_TRACKS; ++t) w->trk[t].last_step = -1;
}

/* The step in THIS LANE whose start time we have most recently passed.
 * Micro-timing shifts a step's start by micro/24 of a step, so the active step
 * is not simply floor(position / step). Recomputed per block, which puts
 * micro-timing resolution at one block (~2.9 ms) — finer than a 1/24 step at
 * any sane tempo, but not sample-accurate.
 *
 * Per lane, and that is the point: micro is stored on the step, so it belongs
 * to one track's pattern. Reading it from the selected lane — which is what
 * this did while there was only one — would have made a nudge on track 1 move
 * all eight, made a nudge on track 5 do nothing at all, and made the audio
 * depend on where the UI happened to be pointed. */
static int active_step(const work_lane_t *ln, int len, double seq_frame, double sf) {
    int best = -1;
    double best_start = -1e18;

    for (int i = 0; i < len; ++i) {
        double start = (double)i * sf + ((double)ln->step[i].micro / 24.0) * sf;
        if (start <= seq_frame && start > best_start) {
            best_start = start;
            best = i;
        }
    }
    /* Nothing has started yet this pass (step 0 nudged late): hold the last
     * step of the pattern, whose lock state is still the current one. */
    return best < 0 ? len - 1 : best;
}

/* Snapshot the pattern before a destructive edit, so undo has somewhere to go.
 * Called by every operation that rewrites more than one step. */
static void push_undo(work_t *w) {
    w->undo_buf   = *CURPAT(w);
    w->undo_valid = 1;
    w->redo_valid = 0;
}

/* Pages can be muted individually: a step on a silenced page is skipped
 * entirely rather than firing with its locks. */
static int page_plays(const work_t *w, int step) {
    int page = step / WORK_PAGE_STEPS;
    if (page < 0 || page > 3) return 1;
    return (CURPAT(w)->page_mask >> page) & 1;
}

/* Song mode: at the end of each pattern pass, count repeats and move to the
 * next row, wrapping at the end of the song. */
static void song_advance(work_t *w) {
    if (!w->song_on || w->song_len == 0) return;
    if (++w->song_rep >= (w->song[w->song_row].repeats ? w->song[w->song_row].repeats : 1)) {
        w->song_rep = 0;
        w->song_row = (uint8_t)((w->song_row + 1) % w->song_len);
    }
    w->cur_pattern = (uint8_t)iclamp(w->song[w->song_row].pattern, 0, WORK_PATTERNS - 1);
    seq_rearm(w);
}

/* Start every SRC voice from the top of its window. Called by a full trig and
 * by an incoming note; both are the "something happened" edge. Only slots
 * actually holding an SRC machine react, so this is free on an FX-only chain. */
/* Start whatever voice the stages' machines have, from the EFFECTIVE state.
 *
 * Effective, not base, on both counts. A machine lock puts a different machine
 * on this step — reading the base machine here meant the locked machine was
 * never triggered at all, so a step that locked a sampler onto the source stage
 * played silence. And a p-locked STRT or window has to apply to the voice this
 * trig starts, not to the next one. "Each firing trig is a complete snapshot"
 * is the documented rule; this is what makes it true of the voice as well as
 * the parameters. */
static void work_src_trigger(work_track_t *tr, int note, int vel) {
    const int frames = tr->sample_frames;
    if (frames <= 0) return;

    /* 60 is unity, the sampler convention: a C3 trig plays at the recorded
     * pitch and everything else transposes from there. A sequencer trig has no
     * note of its own, so it uses unity. */
    const float pitch = powf(2.0f, ((float)note - 60.0f) / 12.0f);
    const float gain  = vel > 0 ? (float)vel / 127.0f : 1.0f;

    for (int i = 0; i < WORK_STAGES; ++i) {
        work_slot_t *s = &tr->slot[i];
        switch (tr->eff_machine[i]) {
        case WORK_FX_ONESHOT: {
            int start = (int)((double)tr->eff[i][1] / 127.0 * (frames - 1));
            s->sp_pos   = start;
            s->sp_env   = 0.0f;
            s->sp_stage = 1;
            s->sp_note  = note;
            vfilt_reset(&s->sp_filt);
            break;
        }
        case WORK_FX_POLYSAMPLE: {
            int start = (int)((double)tr->eff[i][4] / 127.0 * (frames - 1));
            work_voice_t *v = voice_alloc(s);
            v->pos = start; v->env = 0.0f; v->stage = 1;
            v->rate = pitch; v->gain = gain; v->dir = 1;
            v->vib_ph = 0.0f; v->vib_fade = 0.0f; v->note = note;
            vfilt_reset(&v->filt);
            break;
        }
        case WORK_FX_SLICER: {
            const int mode = iclamp((int)tr->eff[i][1] * 4 / 128, 0, 3);
            int start = (int)((double)tr->eff[i][2] / 127.0 * (frames - 1));
            int len   = (int)((double)tr->eff[i][3] / 127.0 * (frames - start));
            if (len < 2) len = 2;
            if (start + len > frames) len = frames - start;
            work_voice_t *v = voice_alloc(s);
            /* The reverse modes start at the END of the window and walk back,
             * which is what the manual describes for REV and REV.L. */
            const int reverse = (mode == SUB_REV || mode == SUB_REV_LOOP);
            v->pos   = reverse ? (start + len - 1) : start;
            v->dir   = reverse ? -1 : 1;
            v->env   = 0.0f; v->stage = 1;
            v->rate  = pitch; v->gain = gain; v->note = note;
            vfilt_reset(&v->filt);
            break;
        }
        default: break;
        }
    }
}

/* One lane's trig for this step. Split out of seq_run because the transport is
 * global and the trigs are not: the playhead advances once, then every track's
 * lane decides for itself whether it fires. */
static void lane_fire(work_t *w, work_track_t *tr, const work_step_t *st, int cur) {
    if (!st->active) return;                  /* no trig: nothing changes */
    if (!page_plays(w, cur)) return;          /* this page is silenced    */

    if (!cond_fires(w, st->cond)) {
        w->pre_result = 0;
        return;
    }
    /* PROB is a separate gate from the condition: both must
     * pass. 100 (the default) always passes. */
    if (st->prob < 100 && rnd_01(&w->cond_rng) * 100.0f >= (float)st->prob) {
        w->pre_result = 0;
        return;
    }
    w->pre_result = 1;

    /* The trig latches its locks. Parameters this trig does NOT lock revert to
     * their base value, so each firing trig is a complete snapshot of the FX
     * state — predictable to program, and the behaviour documented in help. */
    tr->held_mask = st->lock_mask;
    for (int i = 0; i < WORK_LOCKABLE; ++i) tr->held[i] = st->lock[i];

    /* A LOCK trig applies its locks and stops there — the trigless lock. Only
     * a FULL trig restarts the modulators. Retrig restarts the FX LFOs and the
     * Multimode Filter envelope; it does not stutter audio — the Decimator's
     * FREZ is the machine for that. */
    if (st->trig_type == WORK_TRIG_FULL) {
        if (st->retrig != WORK_RETRIG_OFF) {
            for (int n = 0; n < WORK_LFOS; ++n) tr->lfo_ph[n] = 0.0f;
            for (int sl = 0; sl < WORK_STAGES; ++sl) tr->slot[sl].env_stage = 1.0f;
        }
        tr->menv_stage = 1.0f;
        tr->menv_t     = 0.0f;
        /* An SRC machine has a voice to start, unlike every FX machine before
         * it: a full trig fires the sample from its window start. A LOCK trig
         * deliberately does not. That is what makes it a lock trig.
         *
         * PENDING rather than immediate, because the machine-change reset has
         * not run yet: it runs after build_effective, and it clears the slot —
         * including a voice started here. That swallowed the trig whenever a
         * machine changed in the same block, which is every first block after
         * loading one, and every step that carries a MACHINE LOCK. The second
         * is the one that matters: locking a machine onto a step is meant to
         * make that step sound like the new machine, not to make it silent. */
        tr->src_trig_pending = 1;
    }
}

static void seq_run(work_t *w, int frames) {
    if (!w->seq_on) {
        for (int t = 0; t < WORK_TRACKS; ++t) w->trk[t].held_mask = 0;
        return;
    }

    double sf    = (double)step_frames(w);
    int    len   = CURPAT(w)->len ? CURPAT(w)->len : 1;
    double total = sf * (double)len;

    w->seq_frame += (double)frames;
    while (w->seq_frame >= total) {
        w->seq_frame -= total;
        w->pass++;
        song_advance(w);            /* a completed pass may change pattern */
    }

    /* One transport, but each lane finds its own step in it: micro-timing is
     * per step and therefore per lane, so at any moment two lanes can be on
     * different steps. Each keeps its own edge detector to match. */
    for (int t = 0; t < WORK_TRACKS; ++t) {
        work_track_t *tr = &w->trk[t];
        int cur = active_step(LANE(w, t), len, w->seq_frame, sf);
        if (t == w->sel_track) w->seq_pos = cur;   /* the playhead the UI draws */
        if (cur == tr->last_step) continue;
        tr->last_step = cur;
        lane_fire(w, tr, &LANE(w, t)->step[cur], cur);
    }
}

/* The voice filter's fields in edit-page order, which is also lock-index order.
 * A switch rather than pointer arithmetic across the struct: the seven fields
 * are all uint8_t, but C is free to pad between members, and "it works on this
 * compiler" is not a reason to index a struct as an array. */
static void vfilt_set_field(work_vfilt_cfg_t *v, int field, uint8_t value) {
    switch (field) {
        case 0: v->base   = value; break;
        case 1: v->width  = value; break;
        case 2: v->reso   = value; break;
        case 3: v->env    = value; break;
        case 4: v->attack = value; break;
        case 5: v->decay  = value; break;
        case 6: v->track  = value; break;
        default: break;
    }
}

/* Build the effective parameter set for this block: base, then the locks the
 * current trig latched, then the FX LFOs on top. */
static void build_effective(work_t *w, work_track_t *tr, int frames) {
    for (int s = 0; s < WORK_STAGES; ++s)
        for (int i = 0; i < WORK_PARAMS; ++i)
            tr->eff[s][i] = tr->cfg[s].p[i];
    for (int s = 0; s < WORK_STAGES; ++s) tr->eff_machine[s] = tr->cfg[s].machine;
    tr->eff_level = tr->level;
    tr->eff_pan   = tr->pan;
    tr->eff_vfilt = tr->vfilt;
    w->eff_mix = w->mix;

    if (w->seq_on && tr->held_mask) {
        for (int i = 0; i < WORK_LOCKABLE; ++i) {
            if (!(tr->held_mask & (1ull << i))) continue;
            uint8_t v = tr->held[i];
            int knob = 0;
            int stage = work_lock_decode(i, &knob);
            if (stage >= 0)              { tr->eff[stage][knob] = v; continue; }
            if (i == WORK_LOCK_LEVEL)    { tr->eff_level = v;        continue; }
            if (i == WORK_LOCK_PAN)      { tr->eff_pan   = v;        continue; }

            /* The voice filter's seven fields, in the order the edit page shows
             * them. Locking these is what makes a sampled patch move per step
             * without spending one of the machine's eight knobs on a filter. */
            int vf = work_lock_decode_vfilt(i);
            if (vf >= 0) { vfilt_set_field(&tr->eff_vfilt, vf, v); continue; }

            /* A machine lock goes through the SAME family gate as
             * work_set_param. A lock is a way to change a machine, not a way
             * around the rule about which machines a stage accepts — without
             * this, a p-lock could drop a reverb into the source stage or a
             * sampler into an insert, and no surface could show or undo it
             * because the base machine underneath would still look right.
             *
             * An out-of-family lock is IGNORED, not substituted: there is no
             * near-enough machine, and a stage quietly playing something else
             * is worse than one that left the lock on the floor. */
            int ms = work_lock_decode_machine(i);
            if (ms >= 0 && work_machine_fits_stage(ms, v))
                tr->eff_machine[ms] = v;
        }
    }

    /* Modulation envelope: AHD, advanced once per block. Applied before the
     * LFOs so an LFO on the same destination rides the envelope's output. */
    {
        float dt = (float)frames / (float)WORK_SR;
        float atk = pexp(tr->menv.attack, 0.001f, 4.0f);
        float hld = pexp(tr->menv.hold,   0.001f, 4.0f);
        float dec = pexp(tr->menv.decay,  0.005f, 8.0f);

        if (tr->menv_stage == 1.0f) {
            tr->menv_t += dt;
            tr->menv_val = tr->menv_t / atk;
            if (tr->menv_val >= 1.0f) { tr->menv_val = 1.0f; tr->menv_stage = 2.0f; tr->menv_t = 0.0f; }
        } else if (tr->menv_stage == 2.0f) {
            tr->menv_t += dt;
            tr->menv_val = 1.0f;
            if (tr->menv_t >= hld) { tr->menv_stage = 3.0f; tr->menv_t = 0.0f; }
        } else if (tr->menv_stage == 3.0f) {
            tr->menv_t += dt;
            tr->menv_val = 1.0f - tr->menv_t / dec;
            if (tr->menv_val <= 0.0f) { tr->menv_val = 0.0f; tr->menv_stage = 0.0f; }
        }

        if (tr->menv.dest >= 0 && tr->menv.dest < WORK_STAGES * WORK_PARAMS) {
            int slot = tr->menv.dest / WORK_PARAMS;
            int idx  = tr->menv.dest % WORK_PARAMS;
            int out  = tr->eff[slot][idx] +
                       (int)(tr->menv_val * pbi(tr->menv.depth) * 127.0f);
            tr->eff[slot][idx] = (uint8_t)iclamp(out, 0, 127);
        }
    }

    for (int n = 0; n < WORK_LFOS; ++n) {
        work_lfo_cfg_t *L = &tr->lfo[n];

        /* Multiplier scales speed; both stay on the tempo grid */
        float steps = pexp(L->speed, 64.0f, 0.125f);
        float mult  = powf(2.0f, (float)(L->mult / 16) - 4.0f);
        float per   = fmaxf(steps * mult * step_frames(w), 1.0f);
        tr->lfo_ph[n] += (float)frames / per;
        while (tr->lfo_ph[n] >= 1.0f) tr->lfo_ph[n] -= 1.0f;

        if (L->dest < 0 || L->dest >= WORK_STAGES * WORK_PARAMS) continue;

        float ph = tr->lfo_ph[n] + p01(L->phase);
        if (ph >= 1.0f) ph -= 1.0f;

        float v;
        switch (L->wave % 7) {
            case 0: v = 1.0f - 2.0f * fabsf(2.0f * ph - 1.0f); break;       /* tri  */
            case 1: v = sinf(ph * 2.0f * (float)M_PI); break;               /* sine */
            case 2: v = ph < 0.5f ? 1.0f : -1.0f; break;                    /* sqr  */
            case 3: v = 1.0f - 2.0f * ph; break;                            /* saw  */
            case 4: v = 2.0f * ph - 1.0f; break;                            /* ramp */
            case 5: v = expf(-3.0f * ph) * 2.0f - 1.0f; break;              /* exp  */
            default: v = rnd_bi(&tr->rng); break;                            /* rand */
        }

        int slot = L->dest / WORK_PARAMS;
        int idx  = L->dest % WORK_PARAMS;
        /* Modulate around the LOCKED value, not the base one — otherwise a
         * p-lock and an LFO pointed at the same parameter fight each other. */
        int base = tr->eff[slot][idx];
        int out  = base + (int)(v * pbi(L->depth) * 127.0f);
        tr->eff[slot][idx] = (uint8_t)iclamp(out, 0, 127);
    }
}

/* Everything about a track that a preset can carry, set to its default.
 *
 * Split out of work_create because loading a v3 preset needs exactly this: a
 * blob names the tracks it uses, and every track it does NOT name has to come
 * back empty rather than keeping whatever the previous preset left there. When
 * these defaults lived inline in the create loop, the only way to reset a track
 * was to duplicate them at the load site — and a duplicated default list drifts,
 * so a preset would have loaded track 3 with the filter wide open and track 4
 * with it closed.
 *
 * Deliberately does NOT touch the allocations, the RNG seeds or the slot DSP
 * state. Those belong to the instance, not the patch: freeing and re-seeding
 * them on every preset load would drop a reverb tail and reshuffle every
 * machine's noise mid-performance. `index` only seeds the per-track RNG, so it
 * is unused here and named for the call site's benefit. */
static void track_defaults(work_track_t *tr, int index) {
    (void)index;

    /* Wide open. A patch that never visits the filter page must sound
     * exactly as it did before the page existed. */
    tr->vfilt.base   = 0;
    tr->vfilt.width  = 127;
    tr->vfilt.reso   = 0;
    tr->vfilt.env    = 64;      /* bipolar centre = no envelope */
    tr->vfilt.attack = 0;
    tr->vfilt.decay  = 48;
    tr->vfilt.track  = 0;

    tr->menv.dest   = -1;
    tr->menv.attack = 0;
    tr->menv.hold   = 8;
    tr->menv.decay  = 48;
    tr->menv.depth  = 64;

    for (int i = 0; i < WORK_LFOS; ++i) {
        tr->lfo[i].dest  = -1;
        tr->lfo[i].speed = 32;
        tr->lfo[i].mult  = 64;
        tr->lfo[i].wave  = 0;
        tr->lfo[i].depth = 64;
        tr->lfo[i].phase = 0;
        tr->lfo[i].trig  = 0;
    }

    tr->level = 127;               /* unity  */
    tr->pan   = 64;                /* centre */

    for (int i = 0; i < WORK_STAGES; ++i) {
        tr->cfg[i].machine = WORK_FX_BYPASS;
        for (int k = 0; k < WORK_PARAMS; ++k)
            tr->cfg[i].p[k] = PARAM_DEFAULT[WORK_FX_BYPASS][k];
    }

    /* The sample itself stays. A preset records the PATH and the UI reloads the
     * audio; clearing the buffer here would silence the track between the load
     * and the reload, which reads as the preset being broken. */
    tr->sample_path[0] = '\0';
}

work_t *work_create(const host_api_v1_t *host) {
    work_t *w = (work_t *)calloc(1, sizeof(work_t));
    if (!w) return NULL;

    w->host = host;
    w->bpm  = 120.0f;
    w->mix  = 127;
    w->monitor = 1;              /* input passes until a guard says otherwise */

    /* Sequencer starts off, so the audio_fx build behaves as a plain static
     * FX chain until something turns it on. */
    w->seq_on    = 0;
    seq_rearm(w);
    w->cond_rng  = 0x6C078965u;

    /* Every pattern in the bank gets its defaults: PROB 100 (always) on each
     * step, 16 steps long, all four pages playing. */
    for (int p = 0; p < WORK_PATTERNS; ++p) {
        w->pat[p].len = WORK_PAGE_STEPS;
        w->pat[p].page_mask = 0x0F;
        for (int t = 0; t < WORK_TRACKS; ++t)
            for (int i = 0; i < WORK_STEPS; ++i) w->pat[p].lane[t].step[i].prob = 100;
    }
    for (int r = 0; r < WORK_SONG_ROWS; ++r) {
        w->song[r].pattern = (uint8_t)r;
        w->song[r].repeats = 1;
    }
    w->song_len = 4;

    /* Per-track allocation and defaults. Loops over WORK_TRACKS rather than
     * touching track 0 directly, so raising the track count cannot silently
     * leave tracks 1..N with null delay lines — which would not crash, it would
     * render silence from a track that looked configured. */
    for (int t = 0; t < WORK_TRACKS; ++t) {
        work_track_t *tr = &w->trk[t];

        /* Sample memory: allocated ONCE, here, never in the render path. If the
         * allocation fails the engine still runs — every SRC machine checks
         * ->sample before reading, so the module degrades to silence in that
         * slot rather than refusing to load. */
        tr->sample = (int16_t *)calloc((size_t)WORK_SAMPLE_FRAMES * 2, sizeof(int16_t));

        track_defaults(tr, t);

        /* Seeded per track as well as per slot, so two tracks holding the same
         * machine do not generate bit-identical noise. Instance state, not
         * patch state — so it is here and not in track_defaults. */
        tr->rng = 0xC2B2AE35u ^ (uint32_t)(t * 0x27D4EB2Fu);

        for (int i = 0; i < WORK_STAGES; ++i) {
            work_slot_t *s = &tr->slot[i];
            s->dl   = (float *)calloc((size_t)WORK_DLY_LEN * 2, sizeof(float));
            s->pre  = (float *)calloc((size_t)WORK_PRE_LEN * 2, sizeof(float));
            s->comb = (float *)calloc((size_t)WORK_TANK_COMBS * 2 * WORK_TANK_LEN, sizeof(float));
            s->ap   = (float *)calloc((size_t)WORK_TANK_APS * 2 * WORK_AP_LEN, sizeof(float));
            s->pl_diff = (float *)calloc((size_t)WORK_PLATE_DIFF * WORK_PLATE_DLEN, sizeof(float));
            s->pl_ap   = (float *)calloc((size_t)WORK_PLATE_APS * WORK_PLATE_APLEN, sizeof(float));
            s->pl_del  = (float *)calloc((size_t)WORK_PLATE_DELS * WORK_PLATE_DELEN, sizeof(float));
            s->fdn     = (float *)calloc((size_t)WORK_FDN_LINES * WORK_FDN_LEN, sizeof(float));
            if (!s->dl || !s->pre || !s->comb || !s->ap ||
                !s->pl_diff || !s->pl_ap || !s->pl_del || !s->fdn) {
                work_destroy(w); return NULL;
            }

            s->rng = 0x9E3779B9u ^ (uint32_t)((t * WORK_STAGES + i) * 0x85EBCA6Bu);
            s->last_machine = WORK_FX_BYPASS;
        }
    }

    return w;
}

void work_destroy(work_t *w) {
    if (!w) return;
    /* Mirrors work_create's track loop. It also has to survive being called
     * from a FAILED create, where tracks past the failing one are still all
     * zeroes — free(NULL) is fine, which is why nothing here checks first. */
    for (int t = 0; t < WORK_TRACKS; ++t) {
        work_track_t *tr = &w->trk[t];
        free(tr->sample);
        for (int i = 0; i < WORK_STAGES; ++i) {
            free(tr->slot[i].dl);
            free(tr->slot[i].pre);
            free(tr->slot[i].comb);
            free(tr->slot[i].ap);
            free(tr->slot[i].pl_diff);
            free(tr->slot[i].pl_ap);
            free(tr->slot[i].pl_del);
            free(tr->slot[i].fdn);
        }
    }
    free(w);
}

/* Which machines each stage will accept.
 *
 * The families are NOT complements. Granulator sits in the source family
 * because that is where a player looks for it, but it still reads the stage's
 * input when no sample is loaded -- which is what makes live granulation work
 * from the source stage. Bypass belongs to both, because every stage needs a
 * way to be empty.
 *
 * The counts matter as much as the membership: 21 effects and 6 sources,
 * against 21 free palette pads. Adding a machine to the effect family without
 * freeing a pad puts it out of reach of the surface entirely, which has now
 * happened twice. */
static int machine_in_src_family(int machine) {
    return machine == WORK_FX_BYPASS || machine == WORK_FX_GRANULATOR ||
           machine == WORK_FX_ONESHOT || machine == WORK_FX_POLYSAMPLE ||
           machine == WORK_FX_SLICER  || machine == WORK_FX_WAVESCAN;
}

static int machine_in_fx_family(int machine) {
    if (machine < 0 || machine >= WORK_FX_COUNT) return 0;
    return machine == WORK_FX_BYPASS || !machine_in_src_family(machine);
}

int work_machine_fits_stage(int stage, int machine) {
    if (machine < 0 || machine >= WORK_FX_COUNT) return 0;
    if (stage == WORK_STAGE_SRC) return machine_in_src_family(machine);
    if (stage > WORK_STAGE_SRC && stage < WORK_STAGES)
        return machine_in_fx_family(machine);
    return 0;
}

/* Machines that REPLACE the stage's input rather than processing it. Granulator
 * is deliberately absent: it is a source you can also feed. This is the
 * predicate the input mute and the voice filter both key off, and it is not the
 * same question as "is it in the source family". */
static int machine_is_source(int machine) {
    return machine == WORK_FX_ONESHOT || machine == WORK_FX_POLYSAMPLE ||
           machine == WORK_FX_SLICER || machine == WORK_FX_WAVESCAN;
}

void work_process(work_t *w, const int16_t *in, int16_t *out, int frames) {
    if (!w || frames <= 0) return;

    if (w->host && w->host->get_bpm && !w->clock_running) {
        float b = w->host->get_bpm();
        if (b > 20.0f && b < 400.0f) w->bpm = b;
    }

    w->cc_frames += (uint64_t)frames;   /* clock for the CC duplicate guard */

    seq_run(w, frames);

    /* Per-track preparation, once per block. Everything from here to the render
     * is per track and must NOT reach for TRK(w): that macro answers "the track
     * the UI is pointed at", which is the selected one, not the one being
     * prepared. */
    for (int t = 0; t < WORK_TRACKS; ++t) {
        work_track_t *tr = &w->trk[t];

        build_effective(w, tr, frames);
        vfilt_prepare(tr, frames);

        /* A machine change resets that slot's state so a reverb tail or delay
         * line from the previous machine cannot leak into the new one. This
         * uses the EFFECTIVE machine, so a per-step machine lock swaps cleanly
         * too. */
        for (int i = 0; i < WORK_STAGES; ++i) {
            if (tr->slot[i].last_machine != tr->eff_machine[i]) {
                int keep = tr->eff_machine[i];
                slot_reset(&tr->slot[i]);
                tr->slot[i].last_machine = keep;
            }
        }

        /* NOW start the voice a sequencer trig asked for. Doing it inside
         * seq_run put it before the reset above, which then wiped it. */
        if (tr->src_trig_pending) {
            work_src_trigger(tr, 60, 0);
            tr->src_trig_pending = 0;
        }

        /* Multimode Filter envelope gate, from notes seen since last block */
        if (tr->note_pending) {
            for (int i = 0; i < WORK_STAGES; ++i) {
                tr->slot[i].env_stage = 1.0f;
                for (int n = 0; n < WORK_LFOS; ++n)
                    if (tr->lfo[n].trig) tr->lfo_ph[n] = 0.0f;
            }
            /* A played note fires the SRC voice too, so Work is usable from a
             * keyboard or Move's own pads without the sequencer running. */
            work_src_trigger(tr, tr->note_num, tr->note_vel);
            tr->note_pending = 0;
        }

        /* Level and pan resolve once per block, not per sample: they are
         * block-rate controls (a lock lands on a step boundary), and two trig
         * calls per sample per track would be eight of them for no audible
         * gain.
         *
         * Constant power, normalised so CENTRE IS UNITY rather than -3 dB.
         * That normalisation is not a stylistic choice: bypass has to be
         * bit-transparent, and a -3 dB centre would put a 4358-LSB dent in it.
         * Hard left or right is correspondingly +3 dB, the usual bargain for a
         * 0 dB centre.
         *
         * pbi() rather than p01() because pan is a BIPOLAR control: it has an
         * exact zero at 64, where p01 would give 0.50394 and leave centre 0.4%
         * off unity — inaudible, and still enough to fail transparency by
         * ~130 LSB. */
        const float lvl = p01(tr->eff_level);
        const float ang = (pbi(tr->eff_pan) + 1.0f) * 0.25f * 3.14159265f;
        tr->lgain = lvl * cosf(ang) * 1.41421356f;
        tr->rgain = lvl * sinf(ang) * 1.41421356f;
    }

    float gmix = p01(w->eff_mix);


    /* Muting at the INPUT rather than the output is deliberate, and differs
     * from Smack: Smack has a recorded loop that stays audible with the live
     * input muted, but Work's output IS its processed input, so muting the
     * output would just be silence. Zeroing the input instead breaks the
     * feedback path while reverb tails and delay repeats ring out and decay
     * naturally. */
    float in_gain = w->monitor ? 1.0f : 0.0f;

    /* A SOURCE machine in the first slot means the chain generates rather than
     * inserts, and then the hardware input has no business being in the signal
     * path at all: there is nothing to blend it with and it is pure feedback
     * risk. Zeroing it here removes the loop structurally instead of relying on
     * the guard to notice the risk and mute in time — which is what the
     * feedback reported from hardware came down to.
     *
     * Only the FIRST slot decides. A source in slot 2 is fed by slot 1, and a
     * source in slot 1 with an FX in slot 2 is the normal generate-then-treat
     * arrangement; both leave slot 1 as the thing that determines whether the
     * input matters. */
    if (machine_is_source(TRK(w)->eff_machine[0])) in_gain = 0.0f;

    /* The track being rendered. Named here rather than reached for through
     * TRK() so a machine cannot reach the wrong one. */
    for (int f = 0; f < frames; ++f) {
        float dry_l = (float)in[f * 2]     * (1.0f / 32768.0f) * in_gain;
        float dry_r = (float)in[f * 2 + 1] * (1.0f / 32768.0f) * in_gain;

        /* Every track renders and the results SUM, which is what makes them
         * tracks rather than one deep chain. */
        float wet_l = 0.0f, wet_r = 0.0f;
        for (int t = 0; t < WORK_TRACKS; ++t) {
            work_track_t *tr = &w->trk[t];

            /* Only track 1 is fed the live input. The rest start from silence,
             * because they are SOURCE tracks: they generate.
             *
             * This is not a limitation, it is what keeps the module usable as
             * an insert. Handing the input to all eight and summing would make
             * eight bypassed tracks eight times as loud as the signal that
             * arrived — bypass would stop being transparent the moment a track
             * count went up, and every existing patch would clip. */
            float l = (t == 0) ? dry_l : 0.0f;
            float r = (t == 0) ? dry_r : 0.0f;

            for (int i = 0; i < WORK_STAGES; ++i) {
                mctx_t m = { w, tr, &tr->slot[i], tr->eff[i], frames };
                run_machine(&m, tr->eff_machine[i], &l, &r);
                l = sane(l);
                r = sane(r);
            }

            /* Track level and pan, at the routing end — after the stages,
             * because they place the WHOLE track in the mix rather than
             * trimming what one stage produced. */
            wet_l += l * tr->lgain;
            wet_r += r * tr->rgain;
        }

        /* The global dry/wet applies ONCE, to the summed tracks against the
         * input — not per track, which would blend the dry signal in eight
         * times over. */
        float l = dry_l * (1.0f - gmix) + wet_l * gmix;
        float r = dry_r * (1.0f - gmix) + wet_r * gmix;

        int vl = (int)lrintf(fclampf(l, -1.0f, 1.0f) * 32767.0f);
        int vr = (int)lrintf(fclampf(r, -1.0f, 1.0f) * 32767.0f);
        out[f * 2]     = (int16_t)iclamp(vl, -32768, 32767);
        out[f * 2 + 1] = (int16_t)iclamp(vr, -32768, 32767);
    }
}

/* --- MIDI CC control ------------------------------------------------------
 * An external controller reaches every parameter the UI can touch. The map is
 * documented in work_core.h; it follows the sibling modules' layout, which
 * starts at CC 8 and leaves 0-7 and 120+ alone (mod wheel, bank select,
 * channel mode).
 *
 * Values write through work_set_param, so a CC move records a parameter lock
 * when live record is armed exactly like a knob move does.
 */
/* Pick a machine for `stage` from a controller value, scaled across that
 * stage's FAMILY rather than across the whole machine list.
 *
 * Scaling across the list looks equivalent and is not: work_set_param refuses a
 * machine the stage does not accept, so every refused code becomes a dead spot
 * in the controller's travel. On the source stage that is 21 positions out of
 * 26 — five sixths of the knob doing nothing — which reads as broken hardware
 * rather than as a rule being enforced. `range` is the controller's full-scale
 * value: 127 for CC, 16383 for NRPN. */
static int family_pick(int stage, long value, long range) {
    int fam[WORK_FX_COUNT], n = 0;
    for (int mc = 0; mc < WORK_FX_COUNT; ++mc)
        if (work_machine_fits_stage(stage, mc)) fam[n++] = mc;
    if (n <= 0) return WORK_FX_BYPASS;
    long i = (value * (n - 1) + range / 2) / range;
    return fam[iclamp((int)i, 0, n - 1)];
}

/* NRPN parameter numbers mirror the CC map, so CC 8 and NRPN 8 reach the same
 * place — one map to learn, two resolutions. */
static void nrpn_apply(work_t *w, int num, int value14) {
    char key[16], val[8];
    int  v7 = (value14 * 127) / 16383;

    if (num >= 8 && num <= 23) {
        int slot = (num - 8) / WORK_PARAMS, idx = (num - 8) % WORK_PARAMS;
        snprintf(key, sizeof(key), "fx%d_p%d", slot + 1, idx + 1);
        snprintf(val, sizeof(val), "%d", v7);
        work_set_param(w, key, val);
        return;
    }
    if (num == 24 || num == 25) {
        snprintf(key, sizeof(key), "machine%d", num - 23);
        /* full range from 14 bits — the whole reason NRPN exists here */
        snprintf(val, sizeof(val), "%d",
                 family_pick(num - 23, value14, 16383));
        work_set_param(w, key, val);
        return;
    }
    if (num >= 26 && num <= 28) {
        static const char *const K[3] = { "mix", "level", "pan" };
        snprintf(val, sizeof(val), "%d", v7);
        work_set_param(w, K[num - 26], val);
        return;
    }
    /* The source stage, on the free block at 80 — mirroring the CC map above. */
    if (num >= 80 && num <= 87) {
        snprintf(key, sizeof(key), "src_p%d", num - 79);
        snprintf(val, sizeof(val), "%d", v7);
        work_set_param(w, key, val);
        return;
    }
    if (num == 88) {
        snprintf(val, sizeof(val), "%d",
                 family_pick(WORK_STAGE_SRC, value14, 16383));
        work_set_param(w, "src", val);
        return;
    }
    if (num >= 100 && num < 100 + WORK_PATTERNS) {   /* pattern select */
        snprintf(val, sizeof(val), "%d", num - 100);
        work_set_param(w, "pattern", val);
        return;
    }
}

static void cc_apply(work_t *w, int cc, int v) {
    char key[16], val[8];
    snprintf(val, sizeof(val), "%d", v);

    /* NRPN assembly: 99/98 select the parameter, 6/38 carry the value. */
    if (cc == 99) { w->nrpn_num = (w->nrpn_num & 0x7F) | (v << 7); return; }
    if (cc == 98) { w->nrpn_num = (w->nrpn_num & 0x3F80) | v;      return; }
    if (cc == 6)  { w->nrpn_msb = v; nrpn_apply(w, w->nrpn_num, v << 7); return; }
    if (cc == 38) { nrpn_apply(w, w->nrpn_num, (w->nrpn_msb << 7) | v); return; }

    if (cc >= 8 && cc <= 23) {                    /* FX 1 A-H, then FX 2 A-H */
        int slot = (cc - 8) / WORK_PARAMS;
        int idx  = (cc - 8) % WORK_PARAMS;
        snprintf(key, sizeof(key), "fx%d_p%d", slot + 1, idx + 1);
        work_set_param(w, key, val);
        return;
    }
    if (cc == 24 || cc == 25) {                   /* machine select, scaled */
        snprintf(key, sizeof(key), "machine%d", cc - 23);
        snprintf(val, sizeof(val), "%d", family_pick(cc - 23, v, 127));
        work_set_param(w, key, val);
        return;
    }
    if (cc == 26) { work_set_param(w, "mix", val); return; }
    /* Track level and pan, next to the dry/wet they sit beside on the GLOBAL
     * page. 27 and 28 were free: the source stage went to 80 precisely because
     * 27..31 is not eight controls wide, which leaves room for exactly two. */
    if (cc == 27) { work_set_param(w, "level", val); return; }
    if (cc == 28) { work_set_param(w, "pan",   val); return; }

    /* The SOURCE stage, on the free block at 80. It does not continue at 27
     * because 27-31 is not eight controls wide, and because 8..26 was published
     * meaning the inserts and the dry/wet -- which is still exactly what it
     * means. Nothing anyone already mapped moved. It also clears Move's own
     * encoder CCs (71-78), which only matters if an external controller is
     * echoing them. */
    if (cc >= 80 && cc <= 87) {
        snprintf(key, sizeof(key), "src_p%d", cc - 79);
        work_set_param(w, key, val);
        return;
    }
    if (cc == 88) {
        snprintf(val, sizeof(val), "%d", family_pick(WORK_STAGE_SRC, v, 127));
        work_set_param(w, "src", val);
        return;
    }

    /* CC 32/40/48 start LFO 1/2/3; the seven fields run in page order */
    if ((cc >= 32 && cc <= 38) || (cc >= 40 && cc <= 46) || (cc >= 48 && cc <= 54)) {
        static const char *F[7] = {"dest", "spd", "mult", "wave", "depth", "phase", "trig"};
        int n   = (cc - 32) / 8;
        int fld = (cc - 32) % 8;
        if (fld > 6 || n >= WORK_LFOS) return;
        snprintf(key, sizeof(key), "lfo%d_%s", n + 1, F[fld]);
        if (fld == 0) snprintf(val, sizeof(val), "%d",
                               (v * (WORK_STAGES * WORK_PARAMS) + 63) / 127 - 1);
        else if (fld == 3) snprintf(val, sizeof(val), "%d", (v * 6 + 63) / 127);
        else if (fld == 6) snprintf(val, sizeof(val), "%d", v >= 64 ? 1 : 0);
        work_set_param(w, key, val);
        return;
    }

    if (cc >= 56 && cc <= 60) {                   /* modulation envelope */
        static const char *F[5] = {"dest", "atk", "hold", "dec", "depth"};
        snprintf(key, sizeof(key), "menv_%s", F[cc - 56]);
        if (cc == 56) snprintf(val, sizeof(val), "%d",
                               (v * (WORK_STAGES * WORK_PARAMS) + 63) / 127 - 1);
        work_set_param(w, key, val);
        return;
    }

    if (cc == 64) { work_set_param(w, "seq_on",   v >= 64 ? "1" : "0"); return; }
    if (cc == 65) { work_set_param(w, "fill",     v >= 64 ? "1" : "0"); return; }
    if (cc == 66) { work_set_param(w, "live_rec", v >= 64 ? "1" : "0"); return; }
}

/* Which note-ons may fire a voice.
 *
 * Move's SURFACE sends control presses as notes, not just its pads:
 *   notes 0-9    capacitive knob touch
 *   notes 16-31  the sixteen step buttons
 *   notes 68-99  the pad grid
 * Pressing a step button therefore FIRED THE SAMPLE instead of placing a trig
 * — reported from hardware, and the same wrong assumption that put the UI's
 * step handler in the CC branch where nothing ever reached it.
 *
 * Filtering on note NUMBER would be wrong: an external keyboard sends the
 * whole range, and note 60 is the sampler's own unity pitch. So the rule is by
 * SOURCE, matching how CCs are already handled — anything external plays,
 * while Move's own surface only plays from the PAD range. That keeps the chain
 * build's pads playing the sample and stops the step buttons doing it. */
static int note_may_trigger(int note, int source) {
    if (source == MOVE_MIDI_SOURCE_EXTERNAL ||
        source == MOVE_MIDI_SOURCE_FX_BROADCAST) return 1;
    return note >= 68 && note <= 99;
}

/* Which track a channel-voice message addresses.
 *
 * Track N listens on MIDI channel N: channel 1 drives track 1, channel 8
 * drives track 8, and channels above the track count are ignored rather than
 * folded onto anything. Decided 2026-07-29 over the alternative of addressing
 * tracks through NRPN — a channel per track is what every hardware sequencer
 * already speaks, so the whole CC 8..28 map works per track unchanged instead
 * of needing a second, parallel addressing scheme.
 *
 * Ignoring the spare channels rather than routing them to the selected track
 * is the deliberate half. A fallback would make the same message mean
 * different things depending on where the UI happened to be pointed, which is
 * exactly the sort of surprise that makes a rig unreproducible. Nothing is
 * released yet, so there is no map in the wild to keep working. */
static int midi_track(uint8_t status) {
    const int ch = status & 0x0F;
    return ch < WORK_TRACKS ? ch : -1;
}

void work_on_midi(work_t *w, const uint8_t *msg, int len, int source) {
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
            for (int n = 0; n < WORK_LFOS; ++n) TRK(w)->lfo_ph[n] = 0.0f;
            /* Restart the pattern from the top. 0xFB (continue) restarts too:
             * an FX pattern has no note state worth resuming mid-bar, and
             * landing on step 0 is what a performer expects. */
            w->seq_frame  = 0.0;
            seq_rearm(w);
            w->pass       = 0;
            w->pre_result = 0;
            break;
        case 0xFC:                       /* stop */
            w->clock_running = 0;
            break;
        default:
            /* External CC only: Move's own encoders arrive as INTERNAL CCs and
             * would fight the UI. A channel-matched chain slot can deliver one
             * external CC twice (channel dispatch + FX broadcast), so identical
             * messages inside ~2 blocks are dropped — the Mono convention. */
            if (len >= 3 && (msg[0] & 0xF0) == 0xB0 &&
                (source == MOVE_MIDI_SOURCE_EXTERNAL ||
                 source == MOVE_MIDI_SOURCE_FX_BROADCAST)) {
                const int t = midi_track(msg[0]);
                if (t < 0) break;
                /* The duplicate guard keeps the whole status byte, so the same
                 * CC on two channels is two messages and not one repeated. */
                int dup = (msg[0] == w->cc_last[0] && msg[1] == w->cc_last[1] &&
                           msg[2] == w->cc_last[2] &&
                           w->cc_frames - w->cc_last_frames <= 256);
                if (!dup) {
                    w->cc_last[0] = msg[0];
                    w->cc_last[1] = msg[1];
                    w->cc_last[2] = msg[2];
                    w->cc_last_frames = w->cc_frames;

                    /* cc_apply reaches the track through work_set_param, which
                     * addresses the SELECTED one — so the channel's track is
                     * selected for the duration of the write and put back.
                     *
                     * Deliberately reusing the parameter path rather than
                     * giving MIDI its own: the CC map, the NRPN assembly and
                     * the machine-family gate are all in there, and a second
                     * copy addressed at a track pointer is a second copy that
                     * drifts. Selection is UI state that nothing in
                     * work_set_param reads back, and the render path never
                     * looks at it at all. */
                    const uint8_t prev = w->sel_track;
                    w->sel_track = (uint8_t)t;
                    cc_apply(w, msg[1], msg[2]);
                    w->sel_track = prev;
                }
                break;
            }
            if (len >= 3 && (msg[0] & 0xF0) == 0x90 && msg[2] > 0 &&
                note_may_trigger(msg[1], source)) {
                /* Remember the note so a polyphonic SRC machine can pitch its
                 * voice by it. 60 is unity, matching the sampler convention. */
                const int t = midi_track(msg[0]);
                if (t < 0) break;
                w->trk[t].note_pending = 1;
                w->trk[t].note_num = msg[1];
                w->trk[t].note_vel = msg[2];
            } else if (len >= 3 && ((msg[0] & 0xF0) == 0x80 ||
                                    ((msg[0] & 0xF0) == 0x90 && msg[2] == 0))) {
                const int t = midi_track(msg[0]);
                if (t < 0) break;
                for (int i = 0; i < WORK_STAGES; ++i)
                    w->trk[t].slot[i].env_stage = 4.0f;
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
        st->lock_mask &= ~(1ull << idx);       /* -1 clears the lock */
    } else {
        st->lock_mask |= (1ull << idx);
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

/* "src" / "1" / "2" as a stage suffix -> stage index, else -1. Shared by the
 * labels and eff getters so their spelling cannot drift from parse_machine_key's. */
static int parse_stage_suffix(const char *sfx) {
    /* "_src" is the readable spelling ("labels_src", not "labelssrc"); bare
     * "src" is accepted too so the machine key and these stay one parser. */
    if (strcmp(sfx, "_src") == 0 || strcmp(sfx, "src") == 0) return WORK_STAGE_SRC;
    if (sfx[0] >= '1' && sfx[0] < '1' + WORK_INSERTS && sfx[1] == '\0')
        return WORK_STAGE_FX1 + (sfx[0] - '1');
    return -1;
}

/* Stage keys. The source stage is addressed as "src", the inserts as "fx1" and
 * "fx2" (with "machine1"/"machine2" kept as aliases for the inserts, which is
 * what those names already meant to every existing UI).
 *
 * Note the numbering: "fx1" is stage 1, not stage 0. The inserts are numbered
 * as the player sees them -- insert one and insert two -- and the source stage
 * is not an insert at all, so it is named rather than numbered. One parser
 * rather than a chain of strcmp, so adding a stage cannot leave one spelling
 * behind, which is exactly what left slot 3 unconfigurable before. */
static int parse_machine_key(const char *key) {
    if (strcmp(key, "src") == 0) return WORK_STAGE_SRC;
    int n = -1;
    if (strncmp(key, "machine", 7) == 0 && key[8] == '\0') n = key[7] - '1';
    else if (strncmp(key, "fx", 2) == 0 && key[3] == '\0') n = key[2] - '1';
    if (n < 0 || n >= WORK_INSERTS) return -1;
    return WORK_STAGE_FX1 + n;
}

static int parse_slot_param(const char *key, int *slot, int *idx) {
    int stage = -1, off = 0;
    if (strncmp(key, "src_p", 5) == 0) {
        stage = WORK_STAGE_SRC;
        off   = 5;
    } else if (strncmp(key, "fx", 2) == 0 &&
               key[2] >= '1' && key[2] < '1' + WORK_INSERTS &&
               strncmp(key + 3, "_p", 2) == 0) {
        stage = WORK_STAGE_FX1 + (key[2] - '1');
        off   = 5;
    } else {
        return 0;
    }
    int n = atoi(key + off);
    if (n < 1 || n > WORK_PARAMS) return 0;
    *slot = stage;
    *idx  = n - 1;
    return 1;
}

/* -------------------------------------------------- packed lane encoding
 *
 * A lane is 64 steps, each with six fields and up to WORK_LOCKABLE parameter
 * locks. v2 wrote them as "index=value" text inside a "stp" string, and one
 * maximally dense lane came to 14,817 bytes against the host's 16,384-byte
 * read buffer (schwung_host.c, js_host_module_get_param). Survivable at one
 * track. At eight it is roughly 114 KB — seven times over — and the failure
 * mode is silent truncation, so the preset loads and the pattern comes back
 * short with nothing to say it happened.
 *
 * So v3 packs each lane to binary and base64s it. One step record:
 *
 *     u8      step index
 *     u8      active
 *     u8      cond
 *     u8      micro + 64            biased; micro is -23..23
 *     u8      retrig
 *     u8      prob
 *     u8      trig type
 *     u8[5]   lock mask             WORK_LOCKABLE bits, little-endian
 *     u8 × n  lock values           ascending lock index, n = bits set
 *
 * Twelve bytes plus one per lock, against roughly twenty plus seven per lock
 * as text — about a quarter the size, and the dense lane lands near 4 KB. Only
 * steps that differ from empty get a record at all, so a sparse pattern still
 * costs almost nothing.
 *
 * The fields stay a byte each rather than bit-packing into the obvious spare
 * room. Every one of them is bounded by a WORK_* constant that has already
 * grown once, and a format that assumed cond fits in four bits would corrupt
 * every saved preset on the day a seventeenth condition is added. The mask is
 * read back by POPULATION COUNT over all 40 bits rather than by looping to
 * WORK_LOCKABLE, so a blob written by a future engine with more lockables
 * still parses: the extra values are consumed and ignored instead of
 * desynchronising the rest of the lane.
 */
#define LANE_MASK_BYTES ((WORK_LOCKABLE + 7) / 8)
#define LANE_REC_FIXED  (7 + LANE_MASK_BYTES)
#define LANE_MASK_BITS  (LANE_MASK_BYTES * 8)

/* The header sizes w->lane_buf from its own arithmetic, because a caller has
 * to know how big a lane can get without knowing how one is encoded. Tie the
 * two together here: if they ever disagree, lane_pack silently truncates the
 * densest patterns and lane_unpack reads a short blob as a complete one. */
_Static_assert(LANE_REC_FIXED + LANE_MASK_BITS <= WORK_LANE_STEP_MAX,
               "WORK_LANE_STEP_MAX is smaller than one packed step record");

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;                      /* '=' padding and any junk */
}

static const char B64_CHAR[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 into a bounded buffer. Emits whole quartets only: a truncated
 * encoding is still decodable up to where it stops, which matters because the
 * caller's cap is the host's read buffer and the alternative is a half-written
 * final group that decodes to garbage. */
static int b64_encode(const uint8_t *in, int len, char *out, int cap) {
    int n = 0;
    for (int i = 0; i < len; i += 3) {
        const int rem = len - i;
        const unsigned a = in[i];
        const unsigned b = rem > 1 ? in[i + 1] : 0u;
        const unsigned c = rem > 2 ? in[i + 2] : 0u;
        const unsigned v = (a << 16) | (b << 8) | c;
        if (n + 4 > cap) break;
        out[n++] = B64_CHAR[(v >> 18) & 63];
        out[n++] = B64_CHAR[(v >> 12) & 63];
        out[n++] = rem > 1 ? B64_CHAR[(v >> 6) & 63] : '=';
        out[n++] = rem > 2 ? B64_CHAR[v & 63]        : '=';
    }
    return n;
}

/* Decode until the closing quote of the JSON string, or the buffer fills.
 * `acc` is unsigned for the same reason sample_append_b64's is: it is a shift
 * register, and a signed left-shift past the sign bit is undefined. */
static int b64_decode(const char *in, uint8_t *out, int cap) {
    unsigned acc = 0;
    int bits = 0, n = 0;
    for (const char *c = in; *c && *c != '"'; ++c) {
        int v = b64_val(*c);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) break;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

static int lane_pack(const work_lane_t *ln, uint8_t *out, int cap) {
    int n = 0;
    for (int i = 0; i < WORK_STEPS; ++i) {
        const work_step_t *st = &ln->step[i];
        if (!st->active && !st->lock_mask && !st->cond && !st->micro &&
            !st->retrig && !st->trig_type && st->prob == 100)
            continue;

        int nlk = 0;
        for (int k = 0; k < WORK_LOCKABLE; ++k)
            if (st->lock_mask & (1ull << k)) nlk++;
        if (n + LANE_REC_FIXED + nlk > cap) break;

        out[n++] = (uint8_t)i;
        out[n++] = (uint8_t)(st->active ? 1 : 0);
        out[n++] = st->cond;
        out[n++] = (uint8_t)(st->micro + 64);
        out[n++] = st->retrig;
        out[n++] = st->prob;
        out[n++] = st->trig_type;
        for (int b = 0; b < LANE_MASK_BYTES; ++b)
            out[n++] = (uint8_t)((st->lock_mask >> (b * 8)) & 0xFF);
        for (int k = 0; k < WORK_LOCKABLE; ++k)
            if (st->lock_mask & (1ull << k)) out[n++] = st->lock[k];
    }
    return n;
}

static void lane_unpack(work_lane_t *ln, const uint8_t *in, int len) {
    memset(ln->step, 0, sizeof ln->step);
    for (int i = 0; i < WORK_STEPS; ++i) ln->step[i].prob = 100;

    int n = 0;
    while (n + LANE_REC_FIXED <= len) {
        const int idx    = in[n + 0];
        const int active = in[n + 1] ? 1 : 0;
        const int cond   = in[n + 2];
        const int micro  = (int)in[n + 3] - 64;
        const int retrig = in[n + 4];
        const int prob   = in[n + 5];
        const int ttype  = in[n + 6];
        uint64_t mask = 0;
        for (int b = 0; b < LANE_MASK_BYTES; ++b)
            mask |= (uint64_t)in[n + 7 + b] << (b * 8);
        n += LANE_REC_FIXED;

        /* Every set bit carries a value byte, whether or not this engine knows
         * what that lock index means. Counting only the ones below
         * WORK_LOCKABLE would leave a future engine's extra values in the
         * stream and read them as the next step's header. */
        int nlk = 0;
        for (int k = 0; k < LANE_MASK_BITS; ++k)
            if (mask & (1ull << k)) nlk++;
        if (n + nlk > len) break;         /* truncated blob: stop, don't guess */

        work_step_t *st = (idx >= 0 && idx < WORK_STEPS) ? &ln->step[idx] : NULL;
        if (st) {
            st->active    = (uint8_t)active;
            st->cond      = (uint8_t)iclamp(cond,   0, WORK_COND_COUNT - 1);
            st->micro     = (int8_t) iclamp(micro, -23, 23);
            st->retrig    = (uint8_t)iclamp(retrig, 0, WORK_RETRIG_COUNT - 1);
            st->prob      = (uint8_t)iclamp(prob,   1, 100);
            st->trig_type = (uint8_t)iclamp(ttype,  0, WORK_TRIG_TYPES - 1);
        }
        for (int k = 0; k < LANE_MASK_BITS; ++k) {
            if (!(mask & (1ull << k))) continue;
            if (st && k < WORK_LOCKABLE) step_set_lock(st, k, in[n]);
            n++;
        }
    }
}

/* Find the value of key "<pfx><name>" in a blob, returning the character just
 * past the colon. The prefix is how v3 keeps eight tracks in one flat object:
 * track 3's machine is "t3m1", not a nested field, because apply_state walks
 * the blob with strstr rather than parsing JSON and a nested "m1" would be
 * found by whichever track came first. */
static const char *jfind(const char *json, const char *pfx, const char *name) {
    char key[32];
    int n = snprintf(key, sizeof key, "\"%s%s\":", pfx, name);
    if (n <= 0 || n >= (int)sizeof key) return NULL;
    const char *q = strstr(json, key);
    return q ? q + n : NULL;
}

/* Restore from the blob written by get_param("state"). Keys absent from the
 * blob keep their current value, so a partial blob is a legal patch. */
/* Which lock map a blob's indices are written in.
 *
 * "v" alone is not enough. v1 covers TWO different maps: the one before SRC was
 * promoted out of the FX slots, where index 0..7 addressed the first FX slot,
 * and the one after, where it addresses the source stage. Both wrote "v":1 and
 * nothing in the lock data distinguishes them.
 *
 * The FLAT MIRROR key names do distinguish them, and by luck rather than
 * design: they were renumbered in the same change, from fp1/fp2/fp3 to
 * fp_src/fp1/fp2. apply_state ignores those fields, so they are pure metadata —
 * but they are metadata that happens to date the blob exactly. A blob with no
 * flat mirror at all (hand-written, or truncated) is read as the newer layout,
 * because that is what the engine has been writing most recently.
 *
 * Returns the number of stages the blob's index 0 was one stage behind by: 0
 * for a modern v1, 1 for a pre-promotion one. */
static int state_stage_shift(const char *json) {
    if (strstr(json, "\"fp_src\":")) return 0;   /* written after the promotion */
    if (strstr(json, "\"fp3\":"))    return 1;   /* written before it           */
    return 0;
}

/* One track's fields, from the keys carrying `pfx`. v3 prefixes every per-track
 * key with "t<N>"; v1 and v2 knew one track and used no prefix, so they come
 * through here with pfx = "" and land on track 0. */
static void apply_track_state(work_track_t *tr, const char *json, const char *pfx) {
    const char *q;

    if ((q = jfind(json, pfx, "lvl")) != NULL) tr->level = (uint8_t)iclamp(atoi(q), 0, 127);
    if ((q = jfind(json, pfx, "pan")) != NULL) tr->pan   = (uint8_t)iclamp(atoi(q), 0, 127);

    /* The sample path the patch expects. The engine does NOT load it — it has
     * no filesystem and work_set_param runs on the audio thread. It only
     * carries the string so the UI can see, after a preset load, that the
     * audio in memory is not the audio this patch wants, and reload it. */
    if ((q = jfind(json, pfx, "smp")) != NULL && *q == '"') {
        const char *c = q + 1;
        size_t i = 0;
        while (*c && *c != '"' && i < sizeof(tr->sample_path) - 1) {
            if (*c == '\\' && c[1]) c++;      /* unescape \" and \\ */
            tr->sample_path[i++] = *c++;
        }
        tr->sample_path[i] = '\0';
    }

    for (int s = 0; s < WORK_STAGES; ++s) {
        char name[8];
        snprintf(name, sizeof name, "m%d", s + 1);
        if ((q = jfind(json, pfx, name)) != NULL)
            tr->cfg[s].machine = (uint8_t)iclamp(atoi(q), 0, WORK_FX_COUNT - 1);

        snprintf(name, sizeof name, "p%d", s + 1);
        if ((q = jfind(json, pfx, name)) != NULL && *q == '[') {
            const char *c = q + 1;
            for (int i = 0; i < WORK_PARAMS && *c && *c != ']'; ++i) {
                tr->cfg[s].p[i] = (uint8_t)iclamp(atoi(c), 0, 127);
                while (*c && *c != ',' && *c != ']') c++;
                if (*c == ',') c++;
            }
        }
    }

    if ((q = jfind(json, pfx, "vf")) != NULL && *q == '[') {
        const char *c = q + 1;
        int v[7] = {0, 127, 0, 64, 0, 48, 0};
        for (int i = 0; i < 7 && *c && *c != ']'; ++i) {
            v[i] = iclamp(atoi(c), 0, 127);
            while (*c && *c != ',' && *c != ']') c++;
            if (*c == ',') c++;
        }
        tr->vfilt.base   = (uint8_t)v[0]; tr->vfilt.width = (uint8_t)v[1];
        tr->vfilt.reso   = (uint8_t)v[2]; tr->vfilt.env   = (uint8_t)v[3];
        tr->vfilt.attack = (uint8_t)v[4]; tr->vfilt.decay = (uint8_t)v[5];
        tr->vfilt.track  = (uint8_t)v[6];
    }

    for (int n = 0; n < WORK_LFOS; ++n) {
        char name[8];
        snprintf(name, sizeof name, "l%d", n + 1);
        if ((q = jfind(json, pfx, name)) != NULL && *q == '[') {
            const char *c = q + 1;
            int v[7] = {-1, 32, 64, 0, 64, 0, 0};
            for (int i = 0; i < 7 && *c && *c != ']'; ++i) {
                v[i] = atoi(c);
                while (*c && *c != ',' && *c != ']') c++;
                if (*c == ',') c++;
            }
            tr->lfo[n].dest  = (int8_t) iclamp(v[0], -1, WORK_STAGES * WORK_PARAMS - 1);
            tr->lfo[n].speed = (uint8_t)iclamp(v[1], 0, 127);
            tr->lfo[n].mult  = (uint8_t)iclamp(v[2], 0, 127);
            tr->lfo[n].wave  = (uint8_t)iclamp(v[3], 0, 127);
            tr->lfo[n].depth = (uint8_t)iclamp(v[4], 0, 127);
            tr->lfo[n].phase = (uint8_t)iclamp(v[5], 0, 127);
            tr->lfo[n].trig  = (uint8_t)iclamp(v[6], 0, 1);
        }
    }
}

static void apply_state(work_t *w, const char *json) {
    const char *q;

    /* Blob version. Absent means v1: the key was added when the format was
     * already in use, so an old blob simply has no "v". */
    int version = 1;
    if ((q = strstr(json, "\"v\":")) != NULL) version = atoi(q + 4);

    w->load_note[0] = '\0';

    if ((q = strstr(json, "\"mix\":")) != NULL) w->mix = (uint8_t)iclamp(atoi(q + 6), 0, 127);

    if (version >= 3) {
        /* Every track the blob names, and a reset for every track it does not.
         * Leaving an unnamed track alone would let the previous preset's track
         * 5 keep playing under this one — a patch is the whole instrument, not
         * a diff against whatever was loaded before. */
        for (int t = 0; t < WORK_TRACKS; ++t) {
            char pfx[8];
            snprintf(pfx, sizeof pfx, "t%d", t);
            if (!jfind(json, pfx, "m1")) {
                track_defaults(&w->trk[t], t);
                memset(LANE(w, t)->step, 0, sizeof LANE(w, t)->step);
                for (int i = 0; i < WORK_STEPS; ++i) LANE(w, t)->step[i].prob = 100;
                w->trk[t].held_mask = 0;
                continue;
            }
            apply_track_state(&w->trk[t], json, pfx);

            /* The lane, packed. A named track with no "ln" key is one whose
             * lane was empty at save time, so it still gets cleared — the
             * reset above only covers tracks the blob omits entirely. */
            int plen = 0;
            if ((q = jfind(json, pfx, "ln")) != NULL && *q == '"')
                plen = b64_decode(q + 1, w->lane_buf, (int)sizeof w->lane_buf);
            lane_unpack(LANE(w, t), w->lane_buf, plen);
            w->trk[t].held_mask = 0;
        }
        seq_rearm(w);
    } else {
        apply_track_state(TRK(w), json, "");
    }

    if ((q = strstr(json, "\"sq\":[")) != NULL) {
        const char *c = q + 6;
        w->seq_on = (uint8_t)(atoi(c) ? 1 : 0);
        if ((c = strchr(c, ',')) != NULL) {
            CURPAT(w)->len = (uint8_t)iclamp(atoi(c + 1), 1, WORK_STEPS);
            if ((c = strchr(c + 1, ',')) != NULL)
                CURPAT(w)->page_mask = (uint8_t)iclamp(atoi(c + 1), 1, 15);
        }
    }

    /* The pattern replaces whatever was loaded — a blob carrying "stp" is a
     * complete pattern, so a stale step from the previous patch must not
     * survive underneath it. */
    if ((q = strstr(json, "\"stp\":\"")) != NULL) {
        const int shift = version < 2 ? state_stage_shift(json) : 0;
        int dropped = 0;
        memset(CURLANE(w)->step, 0, sizeof(CURLANE(w)->step));
        for (int i = 0; i < WORK_STEPS; ++i) CURLANE(w)->step[i].prob = 100;
        for (int t = 0; t < WORK_TRACKS; ++t) w->trk[t].held_mask = 0;
        seq_rearm(w);

        const char *c = q + 7;
        while (*c && *c != '"') {
            int idx = atoi(c);
            work_step_t *st = (idx >= 0 && idx < WORK_STEPS) ? &CURLANE(w)->step[idx] : NULL;
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
                        else if (field == 5) st->prob = (uint8_t)iclamp(v, 1, 100);
                        else if (field == 6) st->trig_type = (uint8_t)iclamp(v, 0, WORK_TRIG_TYPES - 1);
                    }
                } else if (*c == '+' && st) {
                    int k = atoi(c + 1);
                    const char *eq = strchr(c, '=');
                    if (!eq || k < 0) { c++; continue; }
                    if (version < 2) {
                        /* Translate, and count what has no v2 home rather than
                         * dropping it quietly. A pre-promotion blob is shifted
                         * one stage first, so its "slot 1" lands on insert 1
                         * and not on the source stage. */
                        if (shift) k += shift * WORK_PARAMS;
                        int v2 = work_lock_migrate_v1(k);
                        if (v2 < 0) { dropped++; c++; continue; }
                        k = v2;
                    }
                    if (k < WORK_LOCKABLE) step_set_lock(st, k, atoi(eq + 1));
                }
                c++;
            }
            if (*c == '|') c++;
        }

        if (version < 2) {
            /* Say what happened. The global dry/wet was lockable in v1 and is
             * not per-track, so those locks have nowhere to go — see the lock
             * map in work_core.h. */
            snprintf(w->load_note, sizeof(w->load_note),
                     "v1 preset: locks moved%s%s",
                     shift ? ", stages shifted" : "",
                     dropped ? ", mix locks dropped" : "");
        }
    }
}

/* ------------------------------------------------------- sample transfer
 *
 * work_set_param runs on the SHIM'S AUDIO THREAD — shim_handle_param_bulk says
 * so in its own comment ("this runs on the audio thread ~44x/sec"). So the
 * engine must never open a file here. The UI reads the WAV with the host's
 * file bindings, converts to interleaved 16-bit, and pushes it through in
 * base64 chunks; all the engine does per chunk is a bounded decode into
 * already-allocated memory.
 *
 * Protocol:
 *   sample_begin  "<frames>[:<name>]"   reset the cursor, declare the length
 *   sample_chunk  "<base64>"            append; bounded by the allocation
 *   sample_end    anything              commit — sample_frames becomes visible
 *   sample_clear  anything              drop the sample
 *
 * Nothing the render path reads moves until sample_end, so an interrupted
 * transfer leaves the previous sample playing rather than half of a new one. */
/* Decode base64 straight into the sample buffer as little-endian int16.
 * Bounded by the remaining space, so a malformed or over-long chunk cannot
 * run past the allocation. */
static void sample_append_b64(work_t *w, const char *b64) {
    if (!TRK(w)->sample || !b64) return;

    const int cap_bytes = WORK_SAMPLE_FRAMES * 2 * (int)sizeof(int16_t);
    uint8_t  *dst = (uint8_t *)TRK(w)->sample;
    int       off = TRK(w)->sample_fill * 2 * (int)sizeof(int16_t);

    /* `acc` is unsigned because it is a shift register, not a number: a signed
     * int overflows its sign bit after a few characters and `acc << 6` is then
     * undefined. It happened to produce the right bytes on every compiler this
     * has met — the low bits survive either way — but UBSan flags it on every
     * sample transfer, and "works by luck" is not a transfer protocol. */
    unsigned acc = 0;
    int bits = 0;
    for (const char *c = b64; *c; ++c) {
        int v = b64_val(*c);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (off >= cap_bytes) break;
            dst[off++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    /* Only whole stereo frames count as filled. */
    TRK(w)->sample_fill = off / (2 * (int)sizeof(int16_t));
}

void work_set_param(work_t *w, const char *key, const char *val) {
    /* Every write bumps the revision. schwung's remote UI polls the cheap
     * "rui_poll" digest and only pays for a full state read when this moves,
     * so over-reporting costs one extra read and under-reporting loses an
     * edit. Bumping unconditionally is the safe direction. */
    if (w) w->rui_rev++;
    if (!w || !key || !val) return;

    if (strcmp(key, "sample_begin") == 0) {
        int frames = atoi(val);
        if (frames < 0) frames = 0;
        if (frames > WORK_SAMPLE_FRAMES) frames = WORK_SAMPLE_FRAMES;
        TRK(w)->sample_declared = frames;
        TRK(w)->sample_fill = 0;
        const char *colon = strchr(val, ':');
        if (colon) {
            size_t n = strlen(colon + 1);
            if (n >= sizeof(TRK(w)->sample_name)) n = sizeof(TRK(w)->sample_name) - 1;
            memcpy(TRK(w)->sample_name, colon + 1, n);
            TRK(w)->sample_name[n] = '\0';
        } else {
            TRK(w)->sample_name[0] = '\0';
        }
        return;
    }
    if (strcmp(key, "sample_chunk") == 0) { sample_append_b64(w, val); return; }
    if (strcmp(key, "sample_end") == 0) {
        int n = TRK(w)->sample_fill;
        if (TRK(w)->sample_declared > 0 && n > TRK(w)->sample_declared) n = TRK(w)->sample_declared;
        TRK(w)->sample_frames = n;
        return;
    }
    if (strcmp(key, "sample_path") == 0) {
        size_t n = strlen(val);
        if (n >= sizeof(TRK(w)->sample_path)) n = sizeof(TRK(w)->sample_path) - 1;
        memcpy(TRK(w)->sample_path, val, n);
        TRK(w)->sample_path[n] = '\0';
        return;
    }
    if (strcmp(key, "sample_clear") == 0) {
        TRK(w)->sample_frames = 0;
        TRK(w)->sample_fill = 0;
        TRK(w)->sample_declared = 0;
        TRK(w)->sample_name[0] = '\0';
        TRK(w)->sample_path[0] = '\0';
        return;
    }

    if (!w || !key || !val) return;

    int slot, idx;
    if (parse_slot_param(key, &slot, &idx)) {
        int v = iclamp(atoi(val), 0, 127);
        TRK(w)->cfg[slot].p[idx] = (uint8_t)v;
        /* With live record armed and the sequencer running, a knob move also
         * lays a lock on the step that is playing — the live-record gesture,
         * routed through the same path the UI and MIDI CC both use. */
        if (w->live_rec && w->seq_on && w->seq_pos >= 0 && w->seq_pos < WORK_STEPS) {
            work_step_t *st = &CURLANE(w)->step[w->seq_pos];
            st->active = 1;
            step_set_lock(st, slot * WORK_PARAMS + idx, v);
        }
        return;
    }

    /* Machine select. Named machine1/machine2, NOT fx1/fx2: schwung's own
     * component keys for chain FX slots are literally "fx1" and "fx2"
     * (shadow_ui.js), so a parameter of the same name collides in the
     * prefixed key space — `fx1:fx1` — and a visible_if condition pointed at
     * it failed to resolve. visible_if fails OPEN, so every machine's labels
     * showed at once with Bypass's "(no parameters)" first. fx1/fx2 stay as
     * aliases so nothing that already used them breaks. */
    {
        int s = parse_machine_key(key);
        if (s >= 0) {
        int mc = parse_machine(val);
        /* A machine the stage does not accept is REFUSED, not clamped to
         * something nearby. Silently substituting would leave a patch sounding
         * wrong with no way to tell why -- and the source stage refusing a
         * reverb is the whole reason the stages are typed. */
        if (mc >= 0 && !work_machine_fits_stage(s, mc)) return;
        if (mc >= 0 && mc != TRK(w)->cfg[s].machine) {
            TRK(w)->cfg[s].machine = (uint8_t)mc;
            /* Loading a machine installs its defaults */
            for (int i = 0; i < WORK_PARAMS; ++i)
                TRK(w)->cfg[s].p[i] = PARAM_DEFAULT[mc][i];
        }
        return;
        }
    }

    /* The browser editor writes EVERYTHING through this one key, as
      * "<key>:<value>". Not a convenience: the remote UI's writes cross the
      * same blocking param channel as everything else, and one well-known key
      * means the manager needs no list of ours. Same shape smack uses. */
    if (strcmp(key, "rui_set") == 0) {
        const char *colon = strchr(val, ':');
        if (!colon || colon == val) return;
        char k[32];
        size_t n = (size_t)(colon - val);
        if (n >= sizeof(k)) return;
        memcpy(k, val, n);
        k[n] = '\0';
        if (strcmp(k, "rui_set") == 0) return;        /* no recursion */
        work_set_param(w, k, colon + 1);
        return;
    }

    if (strncmp(key, "vf_", 3) == 0) {
        const char *f = key + 3;
        int v = iclamp(atoi(val), 0, 127);
        if      (strcmp(f, "base")  == 0) TRK(w)->vfilt.base   = (uint8_t)v;
        else if (strcmp(f, "width") == 0) TRK(w)->vfilt.width  = (uint8_t)v;
        else if (strcmp(f, "reso")  == 0) TRK(w)->vfilt.reso   = (uint8_t)v;
        else if (strcmp(f, "env")   == 0) TRK(w)->vfilt.env    = (uint8_t)v;
        else if (strcmp(f, "atk")   == 0) TRK(w)->vfilt.attack = (uint8_t)v;
        else if (strcmp(f, "dec")   == 0) TRK(w)->vfilt.decay  = (uint8_t)v;
        else if (strcmp(f, "track") == 0) TRK(w)->vfilt.track  = (uint8_t)v;
        return;
    }

    /* Track level and pan. Global `mix` stays global — see the lock map. */
    /* Which track every other parameter addresses. Out of range is REFUSED,
      * not clamped: a UI that asks for track 9 has a bug, and silently editing
      * track 8 instead would hide it behind edits that land somewhere real. */
    if (strcmp(key, "track") == 0) {
        int t = atoi(val);
        if (t >= 0 && t < WORK_TRACKS) w->sel_track = (uint8_t)t;
        return;
    }
    if (strcmp(key, "level") == 0) { TRK(w)->level = (uint8_t)iclamp(atoi(val), 0, 127); return; }
    if (strcmp(key, "pan")   == 0) { TRK(w)->pan   = (uint8_t)iclamp(atoi(val), 0, 127); return; }
    if (strcmp(key, "mix") == 0) { w->mix = (uint8_t)iclamp(atoi(val), 0, 127); return; }
    if (strcmp(key, "state") == 0) { if (val[0] == '{') apply_state(w, val); return; }

    /* ------------------------------------------------------- sequencer */
    if (strcmp(key, "seq_on") == 0) {
        int on = atoi(val) ? 1 : 0;
        if (on && !w->seq_on) {          /* restart cleanly when switched on */
            w->seq_frame = 0.0; seq_rearm(w); w->pass = 0; w->pre_result = 0;
        }
        w->seq_on = (uint8_t)on;
        return;
    }
    if (strcmp(key, "seq_len") == 0) {
        CURPAT(w)->len = (uint8_t)iclamp(atoi(val), 1, WORK_STEPS);
        /* A lane parked past the new end would never see an edge again. */
        for (int t = 0; t < WORK_TRACKS; ++t)
            if (w->trk[t].last_step >= CURPAT(w)->len) w->trk[t].last_step = -1;
        return;
    }
    if (strcmp(key, "fill") == 0) { w->fill = (uint8_t)(atoi(val) ? 1 : 0); return; }
    if (strcmp(key, "live_rec") == 0) { w->live_rec = (uint8_t)(atoi(val) ? 1 : 0); return; }
    if (strcmp(key, "monitor") == 0)  { w->monitor  = (uint8_t)(atoi(val) ? 1 : 0); return; }
    if (strcmp(key, "hw_input") == 0) { w->hw_input = (uint8_t)(atoi(val) ? 1 : 0); return; }

    /* ------------------------------------------------- bank, song, history */
    if (strcmp(key, "pattern") == 0) {
        int p = iclamp(atoi(val), 0, WORK_PATTERNS - 1);
        if (p != w->cur_pattern) { w->cur_pattern = (uint8_t)p; seq_rearm(w); }
        return;
    }
    if (strcmp(key, "page_mask") == 0) {
        CURPAT(w)->page_mask = (uint8_t)(iclamp(atoi(val), 0, 15));
        if (CURPAT(w)->page_mask == 0) CURPAT(w)->page_mask = 1;   /* never silence all */
        return;
    }
    if (strcmp(key, "song_on") == 0) {
        w->song_on = (uint8_t)(atoi(val) ? 1 : 0);
        if (w->song_on) { w->song_row = 0; w->song_rep = 0;
                          w->cur_pattern = (uint8_t)iclamp(w->song[0].pattern, 0, WORK_PATTERNS - 1); }
        return;
    }
    if (strcmp(key, "song_len") == 0) {
        w->song_len = (uint8_t)iclamp(atoi(val), 1, WORK_SONG_ROWS);
        if (w->song_row >= w->song_len) w->song_row = 0;
        return;
    }
    {
        int n = key_index(key, "song_row");
        if (n >= 0 && n < WORK_SONG_ROWS) {
            const char *c = val;                        /* "pattern:repeats:len" */
            w->song[n].pattern = (uint8_t)iclamp(atoi(c), 0, WORK_PATTERNS - 1);
            if ((c = strchr(c, ':')) != NULL) {
                w->song[n].repeats = (uint8_t)iclamp(atoi(++c), 1, 64);
                if ((c = strchr(c, ':')) != NULL)
                    w->song[n].len = (uint8_t)iclamp(atoi(++c), 0, WORK_STEPS);
            }
            return;
        }
    }
    {
        int n = key_index(key, "trigtype");
        if (n >= 0) {
            CURLANE(w)->step[n].trig_type =
                (uint8_t)iclamp(atoi(val), 0, WORK_TRIG_TYPES - 1);
            return;
        }
    }

    if (strcmp(key, "undo") == 0) {
        if (!w->undo_valid) return;
        w->redo_buf = *CURPAT(w); w->redo_valid = 1;
        *CURPAT(w) = w->undo_buf; w->undo_valid = 0;
        seq_rearm(w);
        return;
    }
    if (strcmp(key, "redo") == 0) {
        if (!w->redo_valid) return;
        w->undo_buf = *CURPAT(w); w->undo_valid = 1;
        *CURPAT(w) = w->redo_buf; w->redo_valid = 0;
        seq_rearm(w);
        return;
    }
    if (strcmp(key, "memorize") == 0) { w->memo_buf = *CURPAT(w); w->memo_valid = 1; return; }
    if (strcmp(key, "recall") == 0) {
        if (!w->memo_valid) return;
        push_undo(w);
        *CURPAT(w) = w->memo_buf;
        seq_rearm(w);
        return;
    }

    /* ------------------------------------------------ transform / quantize */
    if (strcmp(key, "transform") == 0) {
        /* Edits the SELECTED track's lane, like every other pattern edit:
         * these are surface operations on the lane you are looking at, not
         * on all eight at once. */
        work_lane_t *P = CURLANE(w);
        int len = CURPAT(w)->len ? CURPAT(w)->len : 1;   /* shared by the lanes */
        push_undo(w);
        if (strcmp(val, "reverse") == 0) {
            for (int i = 0; i < len / 2; ++i) {
                work_step_t t = P->step[i];
                P->step[i] = P->step[len - 1 - i];
                P->step[len - 1 - i] = t;
            }
        } else if (strcmp(val, "rotl") == 0 || strcmp(val, "rotr") == 0) {
            work_step_t tmp[WORK_STEPS];
            int dir = (val[3] == 'l') ? 1 : len - 1;
            for (int i = 0; i < len; ++i) tmp[i] = P->step[(i + dir) % len];
            for (int i = 0; i < len; ++i) P->step[i] = tmp[i];
        } else if (strcmp(val, "invert") == 0) {
            for (int i = 0; i < len; ++i) P->step[i].active = !P->step[i].active;
        } else if (strcmp(val, "random") == 0) {
            for (int i = 0; i < len; ++i)
                P->step[i].active = rnd_01(&w->cond_rng) < 0.5f ? 1 : 0;
        }
        seq_rearm(w);
        return;
    }
    if (strcmp(key, "quantize") == 0) {
        /* Pull micro-timing toward the grid; 127 lands everything exactly on it. */
        float amt = fclampf((float)atoi(val) / 127.0f, 0.0f, 1.0f);
        /* Edits the SELECTED track's lane, like every other pattern edit:
         * these are surface operations on the lane you are looking at, not
         * on all eight at once. */
        work_lane_t *P = CURLANE(w);
        push_undo(w);
        for (int i = 0; i < WORK_STEPS; ++i)
            P->step[i].micro = (int8_t)lrintf((float)P->step[i].micro * (1.0f - amt));
        return;
    }

    if (strncmp(key, "menv_", 5) == 0) {
        const char *f = key + 5;
        int v = atoi(val);
        if      (strcmp(f, "dest")  == 0) TRK(w)->menv.dest   = (int8_t)iclamp(v, -1, WORK_STAGES * WORK_PARAMS - 1);
        else if (strcmp(f, "atk")   == 0) TRK(w)->menv.attack = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "hold")  == 0) TRK(w)->menv.hold   = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "dec")   == 0) TRK(w)->menv.decay  = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "depth") == 0) TRK(w)->menv.depth  = (uint8_t)iclamp(v, 0, 127);
        return;
    }

    {
        int n = key_index(key, "prob");
        if (n >= 0) { CURLANE(w)->step[n].prob = (uint8_t)iclamp(atoi(val), 1, 100); return; }
    }

    if (strcmp(key, "seq_clear") == 0) {
        push_undo(w);
        memset(CURLANE(w)->step, 0, sizeof(CURLANE(w)->step));
        for (int i = 0; i < WORK_STEPS; ++i) CURLANE(w)->step[i].prob = 100;
        for (int t = 0; t < WORK_TRACKS; ++t) w->trk[t].held_mask = 0;
        seq_rearm(w);
        return;
    }

    {
        int stp, idx;
        if (parse_lock_key(key, &stp, &idx)) {
            step_set_lock(&CURLANE(w)->step[stp], idx, atoi(val));
            return;
        }
    }

    {
        int n = key_index(key, "locks");
        if (n >= 0) { step_set_locks(&CURLANE(w)->step[n], val); return; }
    }

    {
        int n = key_index(key, "step");
        if (n >= 0) {
            /* "active:cond:micro:retrig" — trailing fields may be omitted */
            work_step_t *st = &CURLANE(w)->step[n];
            const char *c = val;
            st->active = (uint8_t)(atoi(c) ? 1 : 0);
            if ((c = strchr(c, ':')) != NULL) {
                st->cond = (uint8_t)iclamp(atoi(++c), 0, WORK_COND_COUNT - 1);
                if ((c = strchr(c, ':')) != NULL) {
                    st->micro = (int8_t)iclamp(atoi(++c), -23, 23);
                    if ((c = strchr(c, ':')) != NULL) {
                        st->retrig = (uint8_t)iclamp(atoi(++c), 0, WORK_RETRIG_COUNT - 1);
                        if ((c = strchr(c, ':')) != NULL) {
                            st->prob = (uint8_t)iclamp(atoi(++c), 1, 100);
                            if ((c = strchr(c, ':')) != NULL)
                                st->trig_type = (uint8_t)iclamp(atoi(++c), 0, WORK_TRIG_TYPES - 1);
                        }
                    }
                }
            }
            seq_rearm(w);   /* re-evaluate: the edited step may be current */
            return;
        }
    }
    if (strcmp(key, "bpm") == 0) {
        float b = (float)atof(val);
        if (b > 20.0f && b < 400.0f) w->bpm = b;
        return;
    }

    if (strncmp(key, "lfo", 3) == 0 && key[3] >= '1' &&
        key[3] < '1' + WORK_LFOS && key[4] == '_') {
        work_lfo_cfg_t *L = &TRK(w)->lfo[key[3] - '1'];
        const char *f = key + 5;
        int v = atoi(val);
        if      (strcmp(f, "dest")  == 0) L->dest  = (int8_t)iclamp(v, -1, WORK_STAGES * WORK_PARAMS - 1);
        else if (strcmp(f, "spd")   == 0) L->speed = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "mult")  == 0) L->mult  = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "wave")  == 0) L->wave  = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "depth") == 0) L->depth = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "phase") == 0) L->phase = (uint8_t)iclamp(v, 0, 127);
        else if (strcmp(f, "trig")  == 0) L->trig  = (uint8_t)iclamp(v, 0, 1);
        return;
    }
}

/* A track that can make no sound and holds no pattern.
 *
 * Not a comparison against every default — that list would have to be kept in
 * step with track_defaults by hand, and a missed field means a preset silently
 * drops a setting. It is an INVARIANT instead: a track whose stages are all
 * Bypass and whose lane is empty has no voice to trigger and nothing to
 * process, so its level, pan, filter and LFOs cannot reach the output no
 * matter what they are set to. Such a track is left out of the blob, and the
 * loader gives it defaults on the way back in.
 *
 * Track 0 is written unconditionally by the caller: it is the one fed the live
 * input, so an all-Bypass chain there still passes audio. */
static int track_is_inaudible(const work_t *w, int t) {
    const work_track_t *tr = &w->trk[t];
    for (int s = 0; s < WORK_STAGES; ++s)
        if (tr->cfg[s].machine != WORK_FX_BYPASS) return 0;
    if (tr->sample_path[0]) return 0;

    const work_lane_t *ln = &w->pat[w->cur_pattern].lane[t];
    for (int i = 0; i < WORK_STEPS; ++i) {
        const work_step_t *st = &ln->step[i];
        if (st->active || st->lock_mask || st->cond || st->micro ||
            st->retrig || st->trig_type || st->prob != 100) return 0;
    }
    return 1;
}

/* Build the whole blob into w->state_buf, and return its length.
 *
 * Built whole and then served in WINDOWS by the "state" and "state@<offset>"
 * keys. v2 wrote straight into the caller's buffer and stopped when it filled,
 * which at one track was a 90%-full 16 KB and at eight is a preset that loses
 * most of its pattern and says nothing about it. A caller that reads "state"
 * and stops still gets a truncated blob; a caller that keeps asking for
 * "state@<n>" until the answer comes back short gets all of it. */
static int state_build(work_t *w) {
    char     *buf     = w->state_buf;
    const int buf_len = (int)sizeof w->state_buf;
    const int cap     = buf_len - 1;
    int       n       = 0;

    n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                            "{\"v\":3,\"mix\":%d,\"sel\":%d",
                            w->mix, w->sel_track), cap);

    for (int t = 0; t < WORK_TRACKS; ++t) {
        if (t != 0 && track_is_inaudible(w, t)) continue;

        char pfx[8];
        snprintf(pfx, sizeof pfx, "t%d", t);
        const work_track_t *tr = &w->trk[t];

        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                ",\"%slvl\":%d,\"%span\":%d",
                                pfx, tr->level, pfx, tr->pan), cap);

        /* The sample PATH, not the audio. A source-machine patch that restored
         * every parameter and then played silence read as a broken module, so
         * the blob records which file the patch expects and the UI reloads it.
         * Emitted only when set, so a patch with no sample is unchanged. */
        if (tr->sample_path[0]) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"%ssmp\":\"", pfx), cap);
            for (const char *c = tr->sample_path; *c && n < cap; ++c) {
                /* Only \" and \\ need escaping; anything below 0x20 would make
                 * the blob invalid JSON, and a control character in a path is
                 * corruption rather than a name, so it is dropped. */
                if ((unsigned char)*c < 0x20) continue;
                if (*c == '"' || *c == '\\')
                    n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                            "\\%c", *c), cap);
                else
                    n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                            "%c", *c), cap);
            }
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\""), cap);
        }

        for (int s = 0; s < WORK_STAGES; ++s) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"%sm%d\":%d,\"%sp%d\":[",
                                    pfx, s + 1, tr->cfg[s].machine, pfx, s + 1), cap);
            for (int i = 0; i < WORK_PARAMS; ++i)
                n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                        i ? "," : "", tr->cfg[s].p[i]), cap);
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "]"), cap);
        }

        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                ",\"%svf\":[%d,%d,%d,%d,%d,%d,%d]", pfx,
                                tr->vfilt.base, tr->vfilt.width, tr->vfilt.reso,
                                tr->vfilt.env, tr->vfilt.attack, tr->vfilt.decay,
                                tr->vfilt.track), cap);

        for (int l = 0; l < WORK_LFOS; ++l) {
            const work_lfo_cfg_t *L = &tr->lfo[l];
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"%sl%d\":[%d,%d,%d,%d,%d,%d,%d]", pfx, l + 1,
                                    L->dest, L->speed, L->mult, L->wave,
                                    L->depth, L->phase, L->trig), cap);
        }

        /* The lane, packed and base64'd. Omitted when empty, which is what
         * keeps a patch that only uses two tracks from carrying six empty
         * ones. */
        const int plen = lane_pack(&w->pat[w->cur_pattern].lane[t],
                                   w->lane_buf, (int)sizeof w->lane_buf);
        if (plen > 0) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                    ",\"%sln\":\"", pfx), cap);
            n = nclamp(n + b64_encode(w->lane_buf, plen, buf + n, cap - n), cap);
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\""), cap);
        }
    }

    n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                            ",\"sq\":[%d,%d,%d]",
                            w->seq_on, CURPAT(w)->len, CURPAT(w)->page_mask), cap);

    /* Flat mirrors of the arrays above, as comma-separated STRINGS.
     *
     * schwung's remote UI seeds a browser page by parsing this blob as a flat
     * object and keeping only scalar fields — a JSON array is dropped on the
     * floor. Rather than change the array form, which every saved preset
     * depends on, the same values go out again as strings under an "f" prefix.
     * apply_state ignores them, so they cost nothing on the way back in.
     *
     * SELECTED TRACK ONLY, and that is deliberate: the page shows one track's
     * chain at a time, and mirroring all eight would multiply the largest
     * fixed cost in the blob by eight to serve a view nothing renders.
     *
     * Keyed by the STAGE suffix — fp_src / fp1 / fp2 — the same spelling
     * labels and eff use. They used to be numbered from 1, which after the SRC
     * promotion would have made "fp1" the source stage while "labels1" meant
     * the first insert: two conventions in one contract, and the browser
     * editor would have had to know which key used which. */
    for (int sl = 0; sl < WORK_STAGES; ++sl) {
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                ",\"fp%s\":\"", STAGE_SFX[sl]), cap);
        for (int i = 0; i < WORK_PARAMS; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", TRK(w)->cfg[sl].p[i]), cap);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\""), cap);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                ",\"fe%s\":\"", STAGE_SFX[sl]), cap);
        for (int i = 0; i < WORK_PARAMS; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", TRK(w)->eff[sl][i]), cap);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "\""), cap);
        n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                                ",\"fn%s\":\"%s\"", STAGE_SFX[sl],
                                MACHINE_NAME[TRK(w)->cfg[sl].machine]), cap);
    }
    n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                            ",\"fvf\":\"%d,%d,%d,%d,%d,%d,%d\"",
                            TRK(w)->vfilt.base, TRK(w)->vfilt.width, TRK(w)->vfilt.reso,
                            TRK(w)->vfilt.env, TRK(w)->vfilt.attack, TRK(w)->vfilt.decay,
                            TRK(w)->vfilt.track), cap);
    n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n),
                            ",\"fseq\":\"%d,%d,%d,%d\"",
                            w->seq_on, CURPAT(w)->len, w->seq_pos,
                            w->cur_pattern), cap);

    n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "}"), cap);
    buf[n] = '\0';
    return n;
}

int work_get_param(work_t *w, const char *key, char *buf, int buf_len) {
    if (!w || !key || !buf || buf_len <= 1) return -1;
    int cap = buf_len - 1;

    int slot, idx;
    if (parse_slot_param(key, &slot, &idx))
        return nclamp(snprintf(buf, buf_len, "%d", TRK(w)->cfg[slot].p[idx]), cap);

    {
        int s = parse_machine_key(key);
        if (s >= 0)
            return nclamp(snprintf(buf, buf_len, "%d", TRK(w)->cfg[s].machine), cap);
    }

    if (strncmp(key, "vf_", 3) == 0) {
        const char *f = key + 3;
        int v = -1;
        if      (strcmp(f, "base")  == 0) v = TRK(w)->vfilt.base;
        else if (strcmp(f, "width") == 0) v = TRK(w)->vfilt.width;
        else if (strcmp(f, "reso")  == 0) v = TRK(w)->vfilt.reso;
        else if (strcmp(f, "env")   == 0) v = TRK(w)->vfilt.env;
        else if (strcmp(f, "atk")   == 0) v = TRK(w)->vfilt.attack;
        else if (strcmp(f, "dec")   == 0) v = TRK(w)->vfilt.decay;
        else if (strcmp(f, "track") == 0) v = TRK(w)->vfilt.track;
        if (v >= 0) return nclamp(snprintf(buf, buf_len, "%d", v), cap);
    }

    /* The digest schwung's remote UI polls: revision, transport, playhead and
     * tempo in one cheap read. It exists so the manager does NOT have to read
     * the whole state blob on every poll — that blob serialises every step and
     * lock, and the param channel it crosses is the one whose cost caused
     * every UI bug in this module. Order is rev:on:tick:bpm. */
    /* Transport only, for the browser's playhead: on:tick:bpm. Separate from
     * rui_poll because the manager pushes this one while the sequencer runs
     * without doing a full state read. */
    if (strcmp(key, "rui_play") == 0)
        return nclamp(snprintf(buf, buf_len, "%d:%d:%d",
                               w->seq_on ? 1 : 0, w->seq_pos,
                               (int)(w->bpm + 0.5f)), cap);

    if (strcmp(key, "rui_poll") == 0)
        return nclamp(snprintf(buf, buf_len, "%u:%d:%d:%d",
                               (unsigned)w->rui_rev, w->seq_on ? 1 : 0,
                               w->seq_pos, (int)(w->bpm + 0.5f)), cap);

    if (strcmp(key, "mix") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", w->mix), cap);
    /* What the last preset load had to translate, if anything. */
    if (strcmp(key, "load_note") == 0)
        return nclamp(snprintf(buf, buf_len, "%s", w->load_note), cap);
    if (strcmp(key, "track") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", w->sel_track), cap);
    if (strcmp(key, "tracks") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", WORK_TRACKS), cap);

    /* Everything the track strip draws, in ONE read: per track, the number of
     * active steps in the current pattern and the machine loaded in its source
     * stage, as "<trigs>:<machine>" separated by commas.
     *
     * One key rather than eight because a parameter read is a blocking SPI
     * round-trip of about 23 ms — eight of them is 184 ms of the audio thread's
     * param budget to redraw a strip, every time the selection moves. The same
     * reasoning already put the playhead and the effective values in a single
     * bulk read; this is that rule applied to the one view that is inherently
     * about all eight tracks at once. */
    if (strcmp(key, "track_map") == 0) {
        int n = 0;
        for (int t = 0; t < WORK_TRACKS; ++t) {
            const work_lane_t *ln = &w->pat[w->cur_pattern].lane[t];
            int trigs = 0;
            for (int i = 0; i < CURPAT(w)->len; ++i)
                if (ln->step[i].active) trigs++;
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d:%d",
                                    t ? "," : "", trigs,
                                    w->trk[t].cfg[WORK_STAGE_SRC].machine), cap);
        }
        return n;
    }
    if (strcmp(key, "level") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", TRK(w)->level), cap);
    if (strcmp(key, "pan") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", TRK(w)->pan), cap);

    /* The values actually reaching the DSP this block, after locks and LFOs.
     * The UI shows these live so a moving parameter reads as moving. */
    if (strncmp(key, "eff", 3) == 0 && parse_stage_suffix(key + 3) >= 0) {
        int s = parse_stage_suffix(key + 3);
        int n = 0;
        for (int i = 0; i < WORK_PARAMS; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    i ? "," : "", TRK(w)->eff[s][i]), cap);
        return n;
    }
    /* Every slot's effective machine, then the effective mix — the UI reads
     * this in one round-trip rather than one per slot. */
    if (strcmp(key, "effm") == 0) {
        int n = 0;
        for (int s = 0; s < WORK_STAGES; ++s)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%d,",
                                    TRK(w)->eff_machine[s]), cap);
        return nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%d",
                                   w->eff_mix), cap);
    }

    if (strcmp(key, "seq_on") == 0)  return nclamp(snprintf(buf, buf_len, "%d", w->seq_on), cap);
    if (strcmp(key, "seq_len") == 0) return nclamp(snprintf(buf, buf_len, "%d", CURPAT(w)->len), cap);
    if (strcmp(key, "fill") == 0)    return nclamp(snprintf(buf, buf_len, "%d", w->fill), cap);
    if (strcmp(key, "seq_pos") == 0)  return nclamp(snprintf(buf, buf_len, "%d", w->seq_pos), cap);
    if (strcmp(key, "live_rec") == 0) return nclamp(snprintf(buf, buf_len, "%d", w->live_rec), cap);
    if (strcmp(key, "monitor") == 0)  return nclamp(snprintf(buf, buf_len, "%d", w->monitor), cap);
    if (strcmp(key, "hw_input") == 0) return nclamp(snprintf(buf, buf_len, "%d", w->hw_input), cap);
    if (strcmp(key, "pattern") == 0)   return nclamp(snprintf(buf, buf_len, "%d", w->cur_pattern), cap);
    if (strcmp(key, "page_mask") == 0) return nclamp(snprintf(buf, buf_len, "%d", CURPAT(w)->page_mask), cap);
    if (strcmp(key, "song_on") == 0)   return nclamp(snprintf(buf, buf_len, "%d", w->song_on), cap);
    if (strcmp(key, "song_len") == 0)  return nclamp(snprintf(buf, buf_len, "%d", w->song_len), cap);
    if (strcmp(key, "song_pos") == 0)  return nclamp(snprintf(buf, buf_len, "%d:%d", w->song_row, w->song_rep), cap);
    if (strcmp(key, "undo_state") == 0)
        return nclamp(snprintf(buf, buf_len, "%d:%d:%d", w->undo_valid, w->redo_valid, w->memo_valid), cap);
    {
        int n = key_index(key, "song_row");
        if (n >= 0 && n < WORK_SONG_ROWS)
            return nclamp(snprintf(buf, buf_len, "%d:%d:%d",
                                   w->song[n].pattern, w->song[n].repeats, w->song[n].len), cap);
    }
    {
        int n = key_index(key, "trigtype");
        if (n >= 0) return nclamp(snprintf(buf, buf_len, "%d", CURLANE(w)->step[n].trig_type), cap);
    }

    if (strncmp(key, "menv_", 5) == 0) {
        const char *f = key + 5;
        int v;
        if      (strcmp(f, "dest")  == 0) v = TRK(w)->menv.dest;
        else if (strcmp(f, "atk")   == 0) v = TRK(w)->menv.attack;
        else if (strcmp(f, "hold")  == 0) v = TRK(w)->menv.hold;
        else if (strcmp(f, "dec")   == 0) v = TRK(w)->menv.decay;
        else if (strcmp(f, "depth") == 0) v = TRK(w)->menv.depth;
        else return -1;
        return nclamp(snprintf(buf, buf_len, "%d", v), cap);
    }

    {
        int n = key_index(key, "prob");
        if (n >= 0) return nclamp(snprintf(buf, buf_len, "%d", CURLANE(w)->step[n].prob), cap);
    }

    /* "a:c:m:r:nlocks" — one poll per step for the UI's grid */
    {
        int n = key_index(key, "step");
        if (n >= 0) {
            const work_step_t *st = &CURLANE(w)->step[n];
            int nl = 0;
            for (int i = 0; i < WORK_LOCKABLE; ++i)
                if (st->lock_mask & (1ull << i)) nl++;
            return nclamp(snprintf(buf, buf_len, "%d:%d:%d:%d:%d:%d:%d",
                                   st->active, st->cond, st->micro, st->retrig, nl,
                                   st->prob, st->trig_type), cap);
        }
    }

    /* A single lock: its value, or -1 when that parameter is not locked on
     * that step. The UI reads this to show "*value" while a step is held and
     * to nudge an existing lock rather than restarting from the base value —
     * without the getter it silently did the wrong thing in both places. */
    {
        int stp, idx;
        if (parse_lock_key(key, &stp, &idx)) {
            const work_step_t *st = &CURLANE(w)->step[stp];
            int v = (st->lock_mask & (1ull << idx)) ? st->lock[idx] : -1;
            return nclamp(snprintf(buf, buf_len, "%d", v), cap);
        }
    }

    {
        int n = key_index(key, "locks");
        if (n >= 0) {
            const work_step_t *st = &CURLANE(w)->step[n];
            int written = 0, out = 0;
            for (int i = 0; i < WORK_LOCKABLE; ++i) {
                if (!(st->lock_mask & (1ull << i))) continue;
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

    /* The machine CODES in each family, as the palette needs them. Codes, not
     * names or positions: a code is what a preset stores and what "machines"
     * indexes, so a UI that filtered by position would drift the moment a
     * machine was appended. The UI reads these once and maps pads to codes. */
    if (strcmp(key, "src_codes") == 0 || strcmp(key, "fx_codes") == 0) {
        const int stage = (key[0] == 's') ? WORK_STAGE_SRC : WORK_STAGE_FX1;
        int n = 0, written = 0;
        for (int mc = 0; mc < WORK_FX_COUNT; ++mc) {
            if (!work_machine_fits_stage(stage, mc)) continue;
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    written ? "," : "", mc), cap);
            written = 1;
        }
        if (!written) { buf[0] = '\0'; return 0; }
        return n;
    }

    if (strcmp(key, "conds") == 0) {
        int n = 0;
        for (int i = 0; i < WORK_COND_COUNT; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%s",
                                    i ? "," : "", COND_NAME[i]), cap);
        return n;
    }

    /* Compressor gain reduction, one field per INSERT. The source stage is not
     * reported because it cannot hold a compressor — that is an effect, and the
     * families are enforced — so a field for it would read 0 forever and invite
     * someone to wonder why their meter is dead. Derived from WORK_STAGES so a
     * third insert would appear here without being remembered. */
    if (strcmp(key, "meter") == 0) {
        int n = 0;
        for (int s = WORK_STAGE_FX1; s < WORK_STAGES; ++s)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%d",
                                    s > WORK_STAGE_FX1 ? ":" : "",
                                    (int)(-TRK(w)->slot[s].gr)), cap);
        return n;
    }

    /* The eight knob labels for whichever machine a slot currently holds.
     * The UI reads these rather than carrying its own copy of the table —
     * a second copy in JS is a copy that drifts. */
    if (strncmp(key, "labels", 6) == 0 && parse_stage_suffix(key + 6) >= 0) {
        int mc = TRK(w)->cfg[parse_stage_suffix(key + 6)].machine;
        int n = 0;
        for (int i = 0; i < WORK_PARAMS; ++i)
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%s",
                                    i ? "," : "", PARAM_NAME[mc][i]), cap);
        return n;
    }

    /* What the UI needs to show and gate on: whether a sample is loaded, how
     * long it is, and how far a transfer has got. */
    if (strcmp(key, "sample_frames") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", TRK(w)->sample_frames), cap);
    if (strcmp(key, "sample_fill") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", TRK(w)->sample_fill), cap);
    if (strcmp(key, "sample_max") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", WORK_SAMPLE_FRAMES), cap);
    if (strcmp(key, "sample_name") == 0)
        return nclamp(snprintf(buf, buf_len, "%s", TRK(w)->sample_name), cap);
    if (strcmp(key, "sample_path") == 0)
        return nclamp(snprintf(buf, buf_len, "%s", TRK(w)->sample_path), cap);

    if (strcmp(key, "machines") == 0) {
        int n = 0;
        for (int i = 0; i < WORK_FX_COUNT; ++i) {
            n = nclamp(n + snprintf(buf + n, (size_t)(buf_len - n), "%s%s",
                                    i ? "," : "", MACHINE_NAME[i]), cap);
        }
        return n;
    }

    if (strncmp(key, "lfo", 3) == 0 && key[3] >= '1' &&
        key[3] < '1' + WORK_LFOS && key[4] == '_') {
        work_lfo_cfg_t *L = &TRK(w)->lfo[key[3] - '1'];
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
    /* ui_hierarchy is DELIBERATELY NOT SERVED.
     *
     * shadow_ui.js enterComponentEdit():
     *
     *     const hierarchy = getComponentHierarchy(slotIndex, componentKey);
     *     if (hierarchy) { enterHierarchyEditor(...); return; }
     *     enterComponentEditFallback(...);        // -> loadModuleUi -> ui_chain.js
     *
     * So answering this key REPLACES our own chain UI with the generic
     * hierarchy editor — and that editor captures the hierarchy once, in
     * enterHierarchyEditorWith(), and never re-fetches it. That is what left
     * the settings page showing the previous machine until you backed out and
     * re-entered, and no amount of visible_if gating fixed it because the
     * whole editor was the wrong editor.
     *
     * src/ui_chain.js is strictly better here: it reads labels1/labels2 from
     * this engine on every machine change, so its labels always match what is
     * loaded. Staying quiet is what lets the host reach it.
     *
     * If Master FX pages ever need labels, the route is chain_params, NOT this
     * key — chain_params does not divert the component editor.
     */

    /* How long the whole blob is, so a caller can page it without having to
     * know the size of the host binding's buffer. Reading until a short answer
     * works too, but it makes the UI depend on a constant that lives in
     * schwung rather than here — and a host that grew its buffer would silently
     * turn a full read into a truncated one. */
    if (strcmp(key, "state_len") == 0)
        return nclamp(snprintf(buf, buf_len, "%d", state_build(w)), cap);

    /* The preset blob, or a window into it.
     *
     *     state            from byte 0
     *     state@<offset>   from byte <offset>
     *
     * Both fill the caller's buffer and return how much they wrote. Read
     * "state_len" first, then "state" and "state@<so far>" until that many
     * bytes have arrived. A short answer also means the end, so a caller that
     * knows the buffer size can skip the length read.
     *
     * The blob is rebuilt for each window rather than cached between them. It
     * costs a few passes over 40 KB on a save, and the alternative is a cache
     * that goes stale between two reads and hands out a preset assembled from
     * two different moments. */
    if (strcmp(key, "state") == 0 || strncmp(key, "state@", 6) == 0) {
        const int total = state_build(w);
        int off = (key[5] == '@') ? atoi(key + 6) : 0;
        if (off < 0) off = 0;
        if (off > total) off = total;
        int n = total - off;
        if (n > cap) n = cap;
        memcpy(buf, w->state_buf + off, (size_t)n);
        buf[n] = '\0';
        return n;
    }

    return -1;
}
