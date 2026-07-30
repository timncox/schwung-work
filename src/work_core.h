/*
 * Work — a clean-room FX engine for the Ableton Move.
 *
 * Every machine is an original DSP implementation, written from published
 * descriptions of the effect it is named for rather than from anyone's
 * source. They aim for the described character, not for bit-exact copies.
 * See CLAUDE.md for the reference material and the clean-room boundary.
 *
 * Shared engine used by both builds:
 *   - work_fx.c        audio_fx_api_v2 wrapper (chain slots + Master FX slots)
 *   - work_overtake.c  plugin_api_v2 wrapper (full-surface overtake)  [phase 2]
 *
 * Architecture: a SOURCE stage followed by TWO insert FX, in series, each
 * loaded with one of 26 machines and each exposing up to 8 parameters — one
 * per Move knob, a 1:1 map. Three FX LFOs and a modulation envelope reach any
 * stage parameter.
 *
 * Realtime rules (schwung docs/REALTIME_SAFETY.md): the render path never
 * allocates, never blocks, never logs. All buffers come from work_create().
 */
#ifndef WORK_CORE_H
#define WORK_CORE_H

#include <stdint.h>
#include "plugin_api_v1.h"

#define WORK_SR          44100
/* A track is a chain of STAGES in series:
 *
 *     input -> [0] SRC -> [1] insert FX 1 -> [2] insert FX 2 -> mix -> out
 *
 * Stage 0 is the source stage and takes only SRC-family machines; the inserts
 * take only effect-family machines. That split is the point: loading a sampler
 * no longer costs an FX slot, which is what it did when all three stages were
 * interchangeable. It also makes each palette fit the pad grid without a Shift
 * bank -- 21 effects and 6 sources, against 21 free palette pads.
 *
 * TWO inserts, matching the reference device. Do not grow this without reading
 * the CC-space note further down: the map is full. */
#define WORK_INSERTS     2
#define WORK_STAGES      (1 + WORK_INSERTS)
#define WORK_STAGE_SRC   0      /* stage index of the source stage      */
#define WORK_STAGE_FX1   1      /* inserts run from here to WORK_STAGES */

/* Tracks. The reference device has eight, and this is the axis the engine
 * grows along — NOT the stage count.
 *
 * Eight since v0.9.0. The engine had been structurally ready for a while (the
 * render path names its track, a pattern holds a lane per track, the lock map
 * is per track); what held the number at 1 was the PRESET FORMAT, because over
 * the host's read buffer a blob does not fail, it truncates. That is fixed —
 * lanes are packed and the blob is served in windows — so this is now just a
 * count. See DESIGN-8TRACK.md. */
#define WORK_TRACKS      8
#define WORK_PARAMS      8      /* knob A-H, one per Move encoder */
#define WORK_LFOS        3      /* FX LFO 1, 2 and 3 (3 added in v0.3.0) */

/* Longest delay line any machine can ask for: 2 s covers a 1/2 note at 60 BPM
 * and a whole note at 120 BPM. Drive Delay clamps its division to this. */
#define WORK_DLY_LEN     (WORK_SR * 2)

/* The three reverbs are three DIFFERENT algorithms, not one tank with three
 * parameter sets — which is what they were until v0.2.0, and the single
 * biggest sonic simplification in the project:
 *
 *   Roomtone  early reflections + a Schroeder comb/allpass tank  (large room)
 *   Iron Room  Dattorro figure-of-eight plate, modulated          (plate)
 *   Voidspace  Householder feedback delay network, shelved loop   (room->huge)
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

/* Granulator: concurrent grains in flight. Work is monotimbral, so this is
 * the whole grain budget rather than a per-voice allowance. */
#define WORK_GRAINS      8

/* ------------------------------------------------------------- sample RAM
 *
 * Tier B's SRC machines need somewhere to put audio. The realtime path never
 * allocates, so the buffer is sized once at work_create() and never grows:
 * eight seconds of stereo at 44.1 kHz stored as int16, which is 1.35 MB per
 * instance. Two instances of Work in a chain is 2.7 MB — affordable next to
 * the A53's headroom, and int16 rather than float is what halves it.
 *
 * The transfer route matters more than the size. work_set_param runs on the
 * SHIM'S AUDIO THREAD (shim_handle_param_bulk's own comment: "this runs on the
 * audio thread ~44x/sec"), so a sample must NOT be read from disk here — file
 * I/O in set_param would block the audio callback. Instead the UI reads the
 * WAV with the host's file bindings and pushes it through in chunks, and the
 * engine does nothing but a bounded memcpy per chunk. */
/* Polyphony. Eight voices is cheap here because a voice is a read cursor and
 * an envelope, not a synth. Voices live in the slot struct, so they are
 * allocated with it and the render path still never allocates. */
#define WORK_VOICES 8

#define WORK_SAMPLE_SECONDS 8
#define WORK_SAMPLE_FRAMES  (WORK_SAMPLE_SECONDS * 44100)
/* Longest comb line: the Freeverb-derived tuning table tops out at 1617
 * frames, doubled by SIZE at max, so 4096 leaves headroom without paying
 * 500 kB per slot for space we can never address. */
#define WORK_TANK_LEN    4096

/* Machine codes. Order is the catalog order and is part of the preset format:
 * APPEND ONLY — inserting a machine renumbers saved presets. */
typedef enum {
    WORK_FX_BYPASS = 0,
    WORK_FX_CLOCK,      /* Clock Pitch       granular pitch shift + fb + LFO */
    WORK_FX_COMB,       /* Comb Filter       bipolar tuned comb, tempo LFO */
    WORK_FX_COMP,       /* Compressor        8 ratios, sidechain src + filter */
    WORK_FX_CHAIN,      /* Chain Delay       drive/width/skew/tilt delay */
    WORK_FX_DECIMATOR,  /* Decimator         bits/SRR/dropouts/ringmod/freeze */
    WORK_FX_GRIT,       /* Gritshaper        drive + rectify + noise ringmod */
    WORK_FX_FOLD,       /* Fold Filter       HP > wavefolder > multimode > dist */
    WORK_FX_FBANK,      /* Filterbank        8 fixed bands, 90 Hz - 4 kHz */
    WORK_FX_BENDER,     /* Spectrum Bender   frequency shifter + sideband */
    WORK_FX_FLANGER,    /* Endless Flanger   barber-pole, never reverses */
    WORK_FX_LPF,        /* Low-Pass Filter   4-pole 24 dB/oct + LFO + spread */
    WORK_FX_MMF,        /* Multimode Filter  LP-BP-HP morph + ADSR envelope */
    WORK_FX_CHORUS,     /* Wide Chorus       widening chorus w/ HP + width */
    WORK_FX_PHASEARRAY, /* Phase Array       4-to-6 stage blendable phaser */
    WORK_FX_ROOMTONE,   /* Roomtone Reverb   large-space reverb, early refl. */
    WORK_FX_DRIVEDELAY, /* Drive Delay       128th-note grid, ping-pong */
    WORK_FX_IRONROOM,   /* Iron Room Reverb  90s plate character */
    WORK_FX_VOIDSPACE,  /* Voidspace Reverb  shelved-feedback room-to-huge */
    WORK_FX_FLUTTER,    /* Flutter           tape pitch warble + noise */
    /* v0.2.0. With no sample loaded this granulates the ROLLING INPUT BUFFER,
     * which is what lets it granulate live audio. One 8-knob page holds the
     * eight parameters that most change the sound; the reference design spends
     * 24 across three pages, so no direction/mode/pan controls and a fixed
     * Hann window in place of a fade/shape pair. */
    WORK_FX_GRANULATOR, /* Granulator        live granular */
    WORK_FX_ONESHOT,    /* One Shot          one-shot sample voice */
    WORK_FX_POLYSAMPLE, /* Polysample        polyphonic sample voice */
    WORK_FX_SLICER,     /* Slicer            play modes + loop points */
    WORK_FX_WAVESCAN,   /* Wavescan          two wavetable oscillators */
    WORK_FX_TILT,       /* Tilt              shelving EQ + width, no source */
    WORK_FX_COUNT
} work_fx_t;

/* Sidechain sources the Compressor can analyse. A Move FX slot only ever sees
 * its own input and the host mix, so those are the two that can be offered —
 * per-track and per-bus taps have nothing to read here. */
typedef enum {
    WORK_SC_INPUT = 0,   /* this slot's own input (classic insert comp)  */
    WORK_SC_MAIN,        /* the signal arriving at the slot pre-FX1      */
    WORK_SC_SIDE_L,      /* left channel only  — pumping from a kick     */
    WORK_SC_SIDE_R,      /* right channel only                            */
    WORK_SC_COUNT
} work_sc_t;

/* ------------------------------------------------------------- sequencer */

#define WORK_STEPS      64     /* four 16-step pages */
#define WORK_PATTERNS   16     /* a bank; song mode chains them  */
#define WORK_SONG_ROWS  32     /* rows in the song               */
#define WORK_PAGE_STEPS 16
/* Lockable parameters, in lock-index order. The map is PER TRACK: index 12
 * means the same thing on every track, and a pattern holds one lane of these
 * per track.
 *
 *    0..7   stage 0 (SRC)  parameters A-H
 *    8..15  stage 1 (FX 1) parameters A-H
 *   16..23  stage 2 (FX 2) parameters A-H
 *   24      SRC machine
 *   25      FX 1 machine
 *   26      FX 2 machine
 *   27      track level
 *   28      track pan
 *   29..35  voice filter: base, width, reso, env, attack, decay, track
 *   ------
 *   36 lockable
 *
 * This map was REBUILT ONCE, in v0.9.0, and it is append-only again from here.
 * Add new lockables at 36 and up.
 *
 * The rebuild was allowed because the eight-lane pattern format broke anyway,
 * and it bought two things the old map could not have: the stage parameters are
 * now contiguous, so an index is `stage * WORK_PARAMS + knob` rather than a
 * special case for whichever stage was bolted on last; and the map describes a
 * TRACK rather than a global chain, which is what makes eight lanes possible.
 *
 * The global dry/wet is deliberately NOT here. It used to be lockable at index
 * 18, and it is not per-track: with eight tracks, "track 5 step 3 changes the
 * global mix" is exactly the kind of cross-track surprise that makes a pattern
 * unpredictable. Per-track LEVEL at 27 is the control that replaces it. A v1
 * mix lock is dropped on load and the load reports it — see "load_note".
 *
 * See DESIGN-8TRACK.md. */
#define WORK_LOCK_MACH0     24     /* stage N's machine is WORK_LOCK_MACH0 + N */
#define WORK_LOCK_LEVEL     27
#define WORK_LOCK_PAN       28
#define WORK_LOCK_VF0       29     /* the seven voice-filter fields run here   */
#define WORK_LOCK_VF_COUNT  7
#define WORK_LOCKABLE       36

/* How big a preset can get.
 *
 * WORK_LANE_MAX is one lane packed to binary at its worst: every one of 64
 * steps carrying a record, and every record carrying a value for every bit the
 * mask can express. The mask is sized in whole bytes, so it addresses 40 lock
 * indices even though only WORK_LOCKABLE of them mean anything — the spare
 * bits are counted here so that raising WORK_LOCKABLE within the same byte
 * count cannot overflow the buffer.
 *
 * WORK_STATE_MAX is the whole blob: eight tracks of that lane in base64 (four
 * characters per three bytes), plus each track's machines, parameters, LFOs,
 * filter and sample path, plus the globals and the flat mirrors. Measured at
 * about 40 KB in the worst case; 48 leaves room for a field or two without
 * another format change. The blob is never read whole through a single host
 * call — see "state@<offset>" — so this bound is about the engine's own
 * scratch, not about what the host can carry. */
#define WORK_LANE_STEP_MAX  (12 + 40)      /* fixed record + one value per mask bit */
#define WORK_LANE_MAX       (WORK_STEPS * WORK_LANE_STEP_MAX)
#define WORK_STATE_MAX      49152

/* The previous map, kept only so a v1 blob can be translated on load. Nothing
 * outside migrate_v1_lock() may use these. */
#define WORK_LOCK_V1_MACH0  16     /* stage 0 and 1's machines           */
#define WORK_LOCK_V1_MIX    18
#define WORK_LOCK_V1_S2P0   19     /* stage 2 parameters A-H             */
#define WORK_LOCK_V1_MACH2  27
#define WORK_LOCK_V1_COUNT  28

/* (stage, knob) -> lock index, and back. Both return -1 for anything out of
 * range so a caller cannot quietly address the wrong parameter. */
static inline int work_lock_param_index(int stage, int knob) {
    if (stage < 0 || stage >= WORK_STAGES || knob < 0 || knob >= WORK_PARAMS)
        return -1;
    return stage * WORK_PARAMS + knob;
}

static inline int work_lock_machine_index(int stage) {
    if (stage < 0 || stage >= WORK_STAGES) return -1;
    return WORK_LOCK_MACH0 + stage;
}

/* Decode a lock index to the stage parameter it addresses. Returns the stage
 * and writes the knob, or -1 when the index addresses anything else. */
static inline int work_lock_decode(int index, int *knob) {
    if (index < 0 || index >= WORK_STAGES * WORK_PARAMS) return -1;
    if (knob) *knob = index % WORK_PARAMS;
    return index / WORK_PARAMS;
}

/* Decode a lock index to the stage whose MACHINE it selects, or -1. Separate
 * from work_lock_decode because a machine lock carries a machine code rather
 * than a 0..127 parameter value. */
static inline int work_lock_decode_machine(int index) {
    if (index < WORK_LOCK_MACH0 || index >= WORK_LOCK_MACH0 + WORK_STAGES)
        return -1;
    return index - WORK_LOCK_MACH0;
}

/* Decode a lock index to a voice-filter field (0..6), or -1. */
static inline int work_lock_decode_vfilt(int index) {
    if (index < WORK_LOCK_VF0 || index >= WORK_LOCK_VF0 + WORK_LOCK_VF_COUNT)
        return -1;
    return index - WORK_LOCK_VF0;
}

/* v1 lock index -> v2, or -1 for one that has no v2 home.
 *
 * v1 is the map as it stood AFTER the SRC promotion: 0..7 SRC, 8..15 FX 1,
 * 16/17 those two machines, 18 the global mix, 19..26 FX 2, 27 FX 2's machine.
 * A blob written BEFORE that promotion carries the same numbers meaning one
 * stage earlier, and nothing in the blob distinguishes the two — see
 * apply_state, which uses the flat-mirror key names to tell them apart. */
static inline int work_lock_migrate_v1(int index) {
    if (index >= 0 && index < 2 * WORK_PARAMS) return index;   /* SRC, FX 1 */
    if (index == WORK_LOCK_V1_MACH0)     return WORK_LOCK_MACH0;
    if (index == WORK_LOCK_V1_MACH0 + 1) return WORK_LOCK_MACH0 + 1;
    if (index == WORK_LOCK_V1_MIX)       return -1;            /* dropped   */
    if (index >= WORK_LOCK_V1_S2P0 && index < WORK_LOCK_V1_S2P0 + WORK_PARAMS)
        return 2 * WORK_PARAMS + (index - WORK_LOCK_V1_S2P0);
    if (index == WORK_LOCK_V1_MACH2)     return WORK_LOCK_MACH0 + 2;
    return -1;
}

/* Trig conditions — the fill/previous/first/ratio set that step sequencers
 * have converged on. A neighbour-track condition has no meaning for a single
 * FX chain and is deliberately absent. */
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

/* Trig types. With no voices to place a note for, the distinction that
 * survives is whether the trig also RESTARTS the modulators. A LOCK trig
 * applies its parameter locks and stops there. */
typedef enum {
    WORK_TRIG_FULL = 0,   /* applies locks AND restarts envelope + LFOs */
    WORK_TRIG_LOCK,       /* applies locks only                         */
    WORK_TRIG_TYPES
} work_trigtype_t;

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
    /* Probability is a per-step parameter separate from the trig condition:
     * 1..100, where 100 means always. Both gates must pass to fire. */
    uint8_t  prob;
    uint8_t  trig_type;               /* work_trigtype_t                    */
    /* bit i set = parameter i is locked. 64 bits because the map is 36 wide;
     * a uint32_t is what capped the old map at 28 and forced the last stage
     * to be bolted on above the machine selects. */
    uint64_t lock_mask;
    uint8_t  lock[WORK_LOCKABLE];
} work_step_t;

/* One pattern: the steps plus the settings that belong to it rather than to
 * the engine. Length and the page play/loop mask move here so switching
 * pattern switches those too, as it does on the hardware. */
/* One track's steps within a pattern. */
typedef struct {
    work_step_t step[WORK_STEPS];
} work_lane_t;

/* A pattern holds one LANE PER TRACK. Length and the page mask are shared by
 * every lane: one transport, one playhead, all lanes at the same step.
 *
 * Per-lane length (polymeter) is deliberately NOT here. It is a real feature
 * and a separate one — it changes what "the current step" means, which every
 * conditional trig and the whole song mode are written against. */
typedef struct {
    work_lane_t lane[WORK_TRACKS];
    uint8_t     len;                  /* 1..64                              */
    uint8_t     page_mask;            /* bit p set = page p plays           */
} work_pattern_t;

/* A song row: play `pattern` for `repeats` passes, optionally overriding the
 * pattern's own length. */
typedef struct {
    uint8_t pattern;
    uint8_t repeats;                  /* 1..64                              */
    uint8_t len;                      /* 0 = use the pattern's own length   */
} work_song_row_t;

/* Modulation envelope, added in v0.3.0 alongside LFO 3. AHD rather than ADSR
 * because there is no note to sustain against — it fires on each trig. */
typedef struct {
    int8_t  dest;                     /* -1 = off, else a slot parameter    */
    uint8_t attack, hold, decay;
    uint8_t depth;                    /* bipolar around 64                  */
} work_modenv_cfg_t;

/* Filter state and envelope belonging to ONE voice. Sharing it across voices
 * would make each new note's envelope sweep every note still sounding, which
 * is audible the moment two trigs overlap. Two filters, because BASE is the
 * high-pass edge and WDTH is the distance up to the low-pass edge: the pair
 * is a band whose width you set directly. */
typedef struct {
    float hp1[2], hp2[2];        /* SVF integrator state, per channel      */
    float lp1[2], lp2[2];
    float env;
    int   stage;                 /* 0 idle, 1 attack, 2 decay              */
} work_vfilt_t;

/* One sample voice. Polysample and Slicer are polyphonic, so a voice owns
 * everything that differs per note: where it is reading, how fast, its envelope
 * and its vibrato phase. */
typedef struct {
    double pos;          /* fractional read cursor, frames                */
    float  env;
    int    stage;        /* 0 idle, 1 attack, 2 decay                     */
    float  rate;         /* playback rate, including note pitch           */
    float  vib_ph;
    float  vib_fade;     /* 0..1, vibrato fades in over FADE              */
    float  gain;         /* note velocity                                 */
    int    note;         /* -1 when free                                  */
    int    dir;          /* +1 forward, -1 reverse                        */
    uint32_t age;        /* for voice stealing: oldest goes first         */
    work_vfilt_t filt;   /* this voice's own filter and filter envelope    */
} work_voice_t;

/* The voice filter, shared by every sample machine in the slot.
 *
 * It is NOT one of the machine's eight knobs: all five source machines
 * already spend all eight, and a filter is a property of the voice rather
 * than of the machine reading the sample. It gets its own edit page.
 *
 * Defaults are wide open, so every patch saved before v0.8.0 sounds exactly
 * as it did. */
typedef struct {
    uint8_t base;        /* high-pass edge, 0 = open                       */
    uint8_t width;       /* octaves from base up to the low-pass edge      */
    uint8_t reso;        /* resonance, both edges                          */
    uint8_t env;         /* envelope amount, bipolar around 64             */
    uint8_t attack;
    uint8_t decay;
    uint8_t track;       /* key tracking, 0 = none, 127 = one-for-one      */
} work_vfilt_cfg_t;

/* One insert slot: machine code + 8 parameters, each 0..127. */
typedef struct {
    uint8_t machine;
    uint8_t p[WORK_PARAMS];
} work_slot_cfg_t;

/* FX LFO. Destination addresses a slot parameter as slot*8 + param, or -1 for
 * off. FX LFOs reach FX-slot parameters only. */
typedef struct {
    int8_t  dest;        /* -1 = off, else 0..(WORK_STAGES*WORK_PARAMS-1) */
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
 *   monitor                   1 = live input reaches the engine, 0 = muted.
 *                             NEVER preset-saved: a patch must not be able to
 *                             re-open a feedback path when it loads.
 *   hw_input                  1 = this build reads the hardware mailbox, so a
 *                             UI should arm the feedback guard. Set by the
 *                             overtake and sound_generator wrappers only —
 *                             the audio_fx build processes upstream chain
 *                             audio and must never auto-mute.
 *   menv_dest/atk/hold/dec/depth   the modulation envelope
 *   pattern                   selected pattern, 0..15
 *   page_mask                 bit p set = page p plays
 *   trigtype<N>               step N's trig type: 0 full, 1 lock-only
 *   undo / redo / memorize / recall     pattern-level edit history
 *   transform                 "reverse" | "rotl" | "rotr" | "invert" | "random"
 *   quantize                  strength 0..127, pulls micro-timing to the grid
 *   song_on / song_len / song_row<N> ("pattern:repeats:len") / song_pos
 *
 * MIDI CC, external only (Move's own encoders arrive as internal CCs):
 *   CC 8..15   insert FX 1 params A..H  CC 16..23  insert FX 2 params A..H
 *   CC 24/25   insert 1 / 2 machine     CC 26      global dry/wet
 *   CC 27      track level              CC 28      track pan
 *   CC 27      track level              CC 28      track pan
 *   CC 32..38  FX LFO 1                 CC 40..46  FX LFO 2
 *   CC 48..54  FX LFO 3                 CC 56..60  modulation envelope
 *   CC 64      sequencer on/off         CC 65      fill
 *   CC 66      live record
 *   CC 80..87  SRC stage params A..H    CC 88      SRC stage machine
 *
 * The source stage sits at 80 rather than continuing at 27, because 27..31 is
 * not eight controls wide -- and because 8..26 was published meaning the two
 * inserts and the dry/wet, which is still exactly what it means. Promoting SRC
 * out of the slots moved nothing anyone had already assigned. NRPN mirrors
 * these numbers exactly, so CC 80 and NRPN 80 reach the same parameter.
 */
/* Whether a stage will accept a machine. The source stage takes the SRC family
 * (Bypass, Granulator and the four voice machines); the inserts take the effect
 * family. A refused machine is not substituted -- see work_set_param. */
int     work_machine_fits_stage(int stage, int machine);

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
 * slot 1 holds Clock Pitch. Machine-dependent, so it takes the engine. */
int work_lock_label(work_t *w, int index, char *buf, int buf_len);

/* Parameter labels for a machine, index 0..7. Returns "" for unused knobs
 * (Bypass has none, Endless Flanger has five). */
const char *work_param_name(int machine, int idx);

#endif /* WORK_CORE_H */
