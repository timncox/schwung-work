/*
 * Work — a clean-room FX engine for the Ableton Move, inspired by the
 * Elektron Tonverk's FX machines.
 *
 * Clean-room: every machine here is written from the published Tonverk user
 * manual's parameter descriptions (OS 1.3.3, docs/tonverk-manual-1.3.3.txt).
 * No Elektron code or factory content is used. These are our own DSP
 * implementations that aim for the described character, not bit-exact clones.
 *
 * Shared engine used by both builds:
 *   - work_fx.c        audio_fx_api_v2 wrapper (chain slots + Master FX slots)
 *   - work_overtake.c  plugin_api_v2 wrapper (full-surface overtake)  [phase 2]
 *
 * Architecture mirrors a Tonverk track's FX section: TWO insert FX slots in
 * series, each loaded with one of 20 machines, each machine exposing up to 8
 * parameters (Tonverk has 8 encoders per page; Move has 8 knobs — a 1:1 map).
 * Two FX LFOs modulate any FX-slot parameter, as on Tonverk's MOD pages 4/5.
 *
 * Realtime rules (schwung docs/REALTIME_SAFETY.md): the render path never
 * allocates, never blocks, never logs. All buffers come from work_create().
 */
#ifndef WORK_CORE_H
#define WORK_CORE_H

#include <stdint.h>
#include "plugin_api_v1.h"

#define WORK_SR          44100
#define WORK_SLOTS       2      /* insert FX 1 and 2, in series */
#define WORK_PARAMS      8      /* Tonverk encoders A-H */
#define WORK_LFOS        3      /* FX LFO 1, 2 and 3 (3 added in v0.3.0) */

/* Longest delay line any machine can ask for: 2 s covers a 1/2 note at 60 BPM
 * and a whole note at 120 BPM. Saturator Delay clamps its division to this. */
#define WORK_DLY_LEN     (WORK_SR * 2)

/* The three reverbs are three DIFFERENT algorithms, not one tank with three
 * parameter sets — which is what they were until v0.2.0, and the single
 * biggest sonic simplification in the project:
 *
 *   Rumsklang  early reflections + a Schroeder comb/allpass tank  (large room)
 *   Steel Box  Dattorro figure-of-eight plate, modulated          (plate)
 *   Supervoid  Householder feedback delay network, shelved loop   (room->huge)
 *
 * Predelay is shared and capped at 500 ms.
 */
#define WORK_PRE_LEN     (WORK_SR / 2)
#define WORK_TANK_COMBS  8
#define WORK_TANK_APS    4

/* Dattorro plate: 4 input diffusers, then two branches of allpass + delay */
#define WORK_PLATE_DIFF  4
#define WORK_PLATE_DLEN  1024      /* input diffuser lines            */
#define WORK_PLATE_APS   4         /* two per branch                  */
#define WORK_PLATE_APLEN 4096
#define WORK_PLATE_DELS  4         /* two per branch                  */
#define WORK_PLATE_DELEN 8192

/* Feedback delay network: 8 mutually-prime lines, Householder mixing */
#define WORK_FDN_LINES   8
#define WORK_FDN_LEN     8192

/* Grainer: concurrent grains in flight. Tonverk allows 8 per voice across 8
 * voices; Work is monotimbral, so 8 is the whole budget. */
#define WORK_GRAINS      8
/* Longest comb line: the Freeverb-derived tuning table tops out at 1617
 * frames, doubled by SIZE at max, so 4096 leaves headroom without paying
 * 500 kB per slot for space we can never address. */
#define WORK_TANK_LEN    4096

/* Machine codes. Order is the catalog order and is part of the preset format:
 * APPEND ONLY — inserting a machine renumbers saved presets. */
typedef enum {
    WORK_FX_BYPASS = 0,
    WORK_FX_CHRONO,      /* Chrono Pitch      granular pitch shift + fb + LFO  */
    WORK_FX_COMB,        /* Comb +/- Filter   bipolar tuned comb, tempo LFO    */
    WORK_FX_COMP,        /* Compressor        8 ratios, sidechain src + filter */
    WORK_FX_DAISY,       /* Daisy Delay       drive/width/skew/tilt delay      */
    WORK_FX_DEGRADER,    /* Degrader          bits/SRR/dropouts/ringmod/freeze */
    WORK_FX_DIRT,        /* Dirtshaper        drive + rectify + noise ringmod  */
    WORK_FX_FOLDER,      /* Filter Folder     HP > wavefolder > multimode > dist */
    WORK_FX_FBANK,       /* Filterbank        8 fixed bands, 90 Hz - 4 kHz     */
    WORK_FX_WARPER,      /* Frequency Warper  frequency shifter + sideband     */
    WORK_FX_FLANGER,     /* Infinite Flanger  barber-pole, never reverses      */
    WORK_FX_LPF,         /* Low-Pass Filter   4-pole 24 dB/oct + LFO + spread  */
    WORK_FX_MMF,         /* Multimode Filter  LP-BP-HP morph + ADSR envelope   */
    WORK_FX_CHORUS,      /* Panoramic Chorus  widening chorus w/ HP + width    */
    WORK_FX_PHASE98,     /* Phase 98          4-to-6 stage blendable phaser    */
    WORK_FX_RUMSKLANG,   /* Rumsklang Reverb  large-space reverb, early refl.  */
    WORK_FX_SATDELAY,    /* Saturator Delay   128th-note grid, ping-pong       */
    WORK_FX_STEELBOX,    /* Steel Box Reverb  90s plate character              */
    WORK_FX_SUPERVOID,   /* Supervoid Reverb  shelved-feedback room-to-huge    */
    WORK_FX_WARBLE,      /* Warble            tape pitch warble + noise        */
    /* v0.2.0. Tonverk's Grainer is an SRC machine that granulates a SAMPLE,
     * across 24 parameters on three pages. Work has no sample loading and one
     * 8-knob page per machine, so this granulates the ROLLING INPUT BUFFER
     * instead, with the eight parameters that most change the sound. What that
     * costs, stated plainly: no sample slot, no AMNT/DIR/MODE/PAN pages, and a
     * fixed Hann window in place of FADE + SHAPE. */
    WORK_FX_GRAINER,     /* Grainer           live granular                   */
    WORK_FX_COUNT
} work_fx_t;

/* Sidechain sources the Compressor can analyse. On Tonverk this spans
 * MAIN/TRK1-8/BUS1-4/IN A/IN B/IN AB; a Move FX slot only ever sees its own
 * input and the host mix, so we expose the reachable subset. */
typedef enum {
    WORK_SC_INPUT = 0,   /* this slot's own input (classic insert comp)  */
    WORK_SC_MAIN,        /* the signal arriving at the slot pre-FX1      */
    WORK_SC_SIDE_L,      /* left channel only  — pumping from a kick     */
    WORK_SC_SIDE_R,      /* right channel only                            */
    WORK_SC_COUNT
} work_sc_t;

/* ------------------------------------------------------------- sequencer */

#define WORK_STEPS      64     /* four 16-step pages */
#define WORK_PAGE_STEPS 16
/* Lockable parameters, in lock-index order:
 *   0..7   slot 1 parameters A-H
 *   8..15  slot 2 parameters A-H
 *   16     slot 1 machine
 *   17     slot 2 machine
 *   18     global dry/wet
 * Append only — the index is part of the pattern format. */
#define WORK_LOCKABLE   19

/* Trig conditions, following Elektron's set. NEI (neighbour track) has no
 * meaning for a single FX chain and is deliberately absent. */
typedef enum {
    WORK_COND_OFF = 0,
    WORK_COND_FILL,      WORK_COND_NOT_FILL,
    WORK_COND_PRE,       WORK_COND_NOT_PRE,
    WORK_COND_FIRST,     WORK_COND_NOT_FIRST,
    WORK_COND_P25,       WORK_COND_P50,      WORK_COND_P75,
    WORK_COND_1_2,       WORK_COND_2_2,
    WORK_COND_1_3,       WORK_COND_2_3,      WORK_COND_3_3,
    WORK_COND_1_4,       WORK_COND_2_4,      WORK_COND_3_4,     WORK_COND_4_4,
    WORK_COND_COUNT
} work_cond_t;

/* Retrig rates, in 16th-note steps per repeat */
typedef enum {
    WORK_RETRIG_OFF = 0,
    WORK_RETRIG_4,       /* 1/4  */
    WORK_RETRIG_8,       /* 1/8  */
    WORK_RETRIG_16,      /* 1/16 */
    WORK_RETRIG_32,      /* 1/32 */
    WORK_RETRIG_COUNT
} work_retrig_t;

typedef struct {
    uint8_t  active;                  /* a trig is placed on this step      */
    uint8_t  cond;                    /* work_cond_t                        */
    int8_t   micro;                   /* -23..+23, in 1/24ths of a step     */
    uint8_t  retrig;                  /* work_retrig_t                      */
    /* Elektron's PROB is a per-step parameter separate from the trig
     * condition, so it is one here too: 1..100, and 100 means always. Both
     * gates must pass for the trig to fire. */
    uint8_t  prob;
    uint32_t lock_mask;               /* bit i set = parameter i is locked  */
    uint8_t  lock[WORK_LOCKABLE];
} work_step_t;

/* Modulation envelope. Tonverk gives a track two voice LFOs, a mod envelope
 * and two FX LFOs; Work had only the FX LFOs until v0.3.0. AHD rather than
 * ADSR because there is no note to sustain against — it fires on each trig. */
typedef struct {
    int8_t  dest;                     /* -1 = off, else a slot parameter    */
    uint8_t attack, hold, decay;
    uint8_t depth;                    /* bipolar around 64                  */
} work_modenv_cfg_t;

/* One insert slot: machine code + 8 parameters, each 0..127 like Elektron. */
typedef struct {
    uint8_t machine;
    uint8_t p[WORK_PARAMS];
} work_slot_cfg_t;

/* FX LFO. Destination addresses a slot parameter as slot*8 + param, or -1 for
 * off. Tonverk's FX LFOs only reach FX-page parameters; same restriction here. */
typedef struct {
    int8_t  dest;        /* -1 = off, else 0..(WORK_SLOTS*WORK_PARAMS-1) */
    uint8_t speed;
    uint8_t mult;
    uint8_t wave;        /* tri, sine, square, saw, ramp, exp, random     */
    uint8_t depth;       /* 64 = zero; bipolar                            */
    uint8_t phase;       /* start phase                                   */
    uint8_t trig;        /* 0 free-run, 1 retrig on note                  */
} work_lfo_cfg_t;

typedef struct work work_t;

/* Lifecycle. work_create() performs every allocation the engine will ever
 * need and returns NULL if any of them fail. */
work_t *work_create(const host_api_v1_t *host);
void    work_destroy(work_t *w);

/* Process stereo interleaved int16. in and out may alias (audio_fx passes the
 * same buffer for both). */
void    work_process(work_t *w, const int16_t *in, int16_t *out, int frames);

/* Parameter access. Keys:
 *   fx1, fx2                  machine code (int or name)
 *   fx1_p1..fx1_p8, fx2_p*    machine parameters, 0..127
 *   lfo1_dest/spd/mult/wave/depth/phase/trig, lfo2_*
 *   mix                       global wet/dry at the end of the chain
 *   state                     whole-engine JSON blob (preset contract)
 *   meter                     "gr1:gr2" compressor gain reduction, read-only
 *   machines                  comma-separated machine names, read-only
 *   labels1, labels2          the current machine's 8 knob labels, read-only
 *
 * Sequencer keys:
 *   seq_on                    0 = the chain is static, 1 = the pattern runs
 *   seq_len                   pattern length, 1..64 steps
 *   fill                      FILL mode, gates the FILL trig conditions
 *   step<N>                   "active:cond:micro:retrig", N is 0-based
 *   lock<N>_<P>               parameter P locked on step N, or -1 to clear
 *   locks<N>                  "p=v,p=v,..." every lock on step N, read/write
 *   seq_pos                   current step, read-only
 *   prob<N>                   step N's probability, 1..100
 *   live_rec                  1 = knob moves record locks onto the playing step
 *   menv_dest/atk/hold/dec/depth   the modulation envelope
 *
 * MIDI CC, external only (Move's own encoders arrive as internal CCs):
 *   CC 8..15   FX 1 parameters A..H     CC 16..23  FX 2 parameters A..H
 *   CC 24/25   FX 1 / FX 2 machine      CC 26      global dry/wet
 *   CC 32..38  FX LFO 1                 CC 40..46  FX LFO 2
 *   CC 48..54  FX LFO 3                 CC 56..60  modulation envelope
 *   CC 64      sequencer on/off         CC 65      fill
 *   CC 66      live record
 */
void    work_set_param(work_t *w, const char *key, const char *val);
int     work_get_param(work_t *w, const char *key, char *buf, int buf_len);

/* MIDI in. Clock (0xF8/0xFA/0xFB) drives tempo-synced machines; note-on
 * retriggers LFOs whose trig mode says so, and the Multimode Filter envelope. */
void    work_on_midi(work_t *w, const uint8_t *msg, int len, int source);

/* Human-readable machine name for a code, or "?" if out of range. */
const char *work_machine_name(int code);

/* Short label for a trig condition ("OFF", "FILL", "1:4", "50%"...). */
const char *work_cond_name(int cond);

/* Label for a lockable parameter index, e.g. "1:TUNE" for slot 1 knob A when
 * slot 1 holds Chrono Pitch. Machine-dependent, so it takes the engine. */
int work_lock_label(work_t *w, int index, char *buf, int buf_len);

/* Parameter labels for a machine, index 0..7. Returns "" for unused knobs
 * (Bypass has none, Infinite Flanger has five). */
const char *work_param_name(int machine, int idx);

#endif /* WORK_CORE_H */
