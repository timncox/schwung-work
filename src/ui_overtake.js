/*
 * Overwork — full-surface UI.
 *
 * Move's sixteen dedicated STEP buttons stand in for a hardware sequencer's
 * trig keys,
 * the eight knobs for its eight encoders, and the jog for LEVEL/DATA. That is
 * a real 1:1 map, not an approximation — which is why the hold-step gesture
 * survives intact: HOLD A STEP AND TURN A KNOB TO PARAMETER-LOCK IT.
 *
 * Surface (pad rows numbered from the TOP, matching the docs convention):
 *
 *   Steps 1-16      the pattern page's trigs
 *                     tap                  toggle a trig
 *                     hold + knob          parameter-lock that knob
 *                     hold + SHIFT + knob  edit the base value instead
 *                     hold + jog           set the attribute the active mode
 *                                          selects (condition / micro / retrig)
 *                     hold + pad 83        clear that step's locks
 *
 *   Pads row 1-3    machine palette for the FOCUSED STAGE — the six sources on
 *   (76-99)           SRC, the twenty-one effects on FX 1 and FX 2. Tap loads
 *                     one. Each family fits the rows outright, so there is no
 *                     longer a SHIFT bank to reach the rest of the list.
 *
 *   Pad row 4       68 play/stop   69 fill     70 stage focus 71 pattern page
 *   (68-75)         72 edit page   73 copy     74 paste       75 clear
 *                   SHIFT + 71/72/73/74/75 = probability / condition / micro /
 *                   retrig mode, and clear-locks-on-the-held-step.
 *                   SHIFT + 68 arms LIVE RECORD: knob moves then write locks
 *                   onto whichever step is playing.
 *
 *   Shift+step      select that pattern from the 16-pattern bank
 *   Pads 81/82/83   undo (shift = redo), memorize/recall, song mode
 *   SHIFT + 69      monitor: mute or unmute the live input. Auto-muted
 *                   whenever the speakers are live with nothing in the
 *                   input jack, because that is a feedback loop.
 *   Shift+jog click open and close the preset browser
 *   Knobs 1-8       the current edit page's eight parameters
 *   Jog             pattern length, or the held step's attribute
 *
 * QuickJS loads this as an ES module: STRICT MODE. Every identifier must be
 * declared — an assigned-but-undeclared variable throws on the first pad
 * release and the host treats a handler exception as fatal, which looks
 * exactly like a crash (the Smack v0.8.6 incident). Audit before shipping.
 */

import {
    MoveKnob1, MoveShift, MoveMainButton, MoveMainKnob,
    Black, White, LightGrey, DarkGrey, Red, BrightRed, Blue, Green, BrightGreen,
    Cyan, Purple, SkyBlue, Lime, OrangeRed, BurntOrange, YellowGreen, TealGreen, Rose
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, setLED, shouldFilterMessage }
    from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    announce, announceParameter, announceView
} from '/data/UserData/schwung/shared/screen_reader.mjs';

import * as os from 'os';

/* Presets live OUTSIDE the module directory, so they survive a reinstall.
 * That also means "I reinstalled and my patterns are still there" is expected,
 * not evidence the reinstall failed. */
const PRESET_DIR = '/data/UserData/schwung/presets/overwork';

/* ------------------------------------------------------------- constants */

/* Move's step buttons are NOTES 16-31, not CCs — schwung's hardware notes say
 * so ("Pads notes 68-99. Steps notes 16-31.") and the device confirms it:
 *   status=144 d1=20 d2=127
 * This handler lived in the CC branch, so nothing ever reached it and the
 * sequencer could not be played from the surface at all. Worse, the note fell
 * through to the engine, which fired the sample — pressing a step TRIGGERED
 * THE SOUND instead of placing a trig. The harness encoded the same wrong
 * assumption, which is why every UI check passed against it. */
const STEP_FIRST = 16;          /* step buttons are NOTES 16-31 */
const STEP_COUNT = 16;
const PAGE_STEPS = 16;
const MAX_STEPS  = 64;
/* The machine count comes from the ENGINE's own list, never a constant. A
 * hardcoded 19 here is what made Granulator unreachable when the 21st machine
 * landed, and a hardcoded 21 did the same to One Shot. The fallback only
 * covers the window before the first fetchAll answers. */
function nMachines() { return machineList.length || 21; }
/* palette slots past the last machine are free for functions */
const PAD_UNDO = 81;
const PAD_MEMO = 82;
const PAD_SONG = 83;

/* Machine palette occupies pad rows 1-3 in reading order: 92-99, 84-91, 76-79 */
const PALETTE_PADS = [
    92, 93, 94, 95, 96, 97, 98, 99,
    84, 85, 86, 87, 88, 89, 90, 91,
    76, 77, 78, 79, 80, 81, 82, 83
];

/* The step-attribute modes moved to SHIFT + row 4 in v0.2.0, when a 21st
 * machine (Granulator) needed the whole of row 3 for the palette. */
const PAD_MODE_COND   = 72;   /* shift + edit-page  */
const PAD_MODE_MICRO  = 73;   /* shift + copy       */
const PAD_MODE_RETRIG = 74;   /* shift + paste      */
const PAD_LOCK_CLEAR  = 75;   /* shift + clear      */
const PAD_MODE_PROB   = 71;   /* shift + pattern page */
const PAD_LIVE_REC    = 68;   /* shift + play         */
const PAD_MONITOR     = 69;   /* shift + fill         */

/* Row 4: transport and navigation */
const PAD_PLAY  = 68;
const PAD_FILL  = 69;
const PAD_STAGE = 70;
const PAD_PPAGE = 71;
const PAD_EPAGE = 72;
const PAD_COPY  = 73;
const PAD_PASTE = 74;
const PAD_CLEAR = 75;

/* Edit pages the knobs address */
const EDIT_FX     = 0;
const EDIT_LFO1   = 1;
const EDIT_LFO2   = 2;
const EDIT_LFO3   = 3;
const EDIT_MENV   = 4;
/* The voice filter is its own page rather than knobs on the sample machines:
 * all five source machines already spend all eight, and a filter belongs to
 * the voice rather than to the machine reading the sample. */
const EDIT_VFILT  = 5;
const EDIT_GLOBAL = 6;
const EDIT_COUNT  = 7;
const EDIT_NAME   = ['FX', 'LFO 1', 'LFO 2', 'LFO 3', 'MOD ENV', 'VOICE FLT',
                     'GLOBAL'];

/* Step-attribute modes the jog edits while a step is held */
const MODE_NONE   = 0;
const MODE_COND   = 1;
const MODE_MICRO  = 2;
const MODE_RETRIG = 3;
const MODE_PROB   = 4;
const MODE_NAME   = ['-', 'COND', 'MICRO', 'RTRG', 'PROB'];

const HOLD_MS = 600;

/* Machine colours, grouped by family the way Smack groups its effects, so the
 * palette reads as a map rather than twenty arbitrary lights. */
/* A hand-maintained table indexed by machine, which is the same shape as the
 * N_MACHINES constant that made two machines unreachable — a UI value mirroring
 * a table the engine owns, with nothing linking them. It cannot be derived
 * (a colour is a design choice, not engine data), so instead: the lookup below
 * falls back rather than handing `undefined` to setLED, and a test asserts this
 * table is the same length as the engine's machine list. */
const MACHINE_COLOR = [
    DarkGrey,      /* 0  Bypass            */
    Purple,        /* 1  Clock Pitch      pitch family */
    Green,         /* 2  Comb Filter   filter family */
    Blue,          /* 3  Compressor        dynamics */
    BurntOrange,   /* 4  Chain Delay       delay family */
    Lime,          /* 5  Decimator          destruction */
    Lime,          /* 6  Gritshaper        destruction */
    Green,         /* 7  Fold Filter     filter */
    Green,         /* 8  Filterbank        filter */
    Purple,        /* 9  Spectrum Bender  pitch */
    Cyan,          /* 10 Endless Flanger  modulation */
    Green,         /* 11 Low-Pass Filter   filter */
    Green,         /* 12 Multimode Filter  filter */
    Cyan,          /* 13 Wide Chorus  modulation */
    Cyan,          /* 14 Phase Array          modulation */
    SkyBlue,       /* 15 Roomtone Reverb  space */
    BurntOrange,   /* 16 Drive Delay   delay */
    SkyBlue,       /* 17 Iron Room Reverb  space */
    SkyBlue,       /* 18 Voidspace Reverb  space */
    TealGreen,     /* 19 Flutter            tape */
    Rose,          /* 20 Granulator           granular */
    White,         /* 21 One Shot     source */
    White,         /* 22 Polysample      source */
    White,         /* 23 Slicer         source */
    YellowGreen,   /* 24 Wavescan        source, wavetable */
    Blue           /* 25 Tilt              an effect — shelving EQ    */
];


/* The palette occupies pad rows 1-3, but three of those pads are undo, memo
 * and song. The LED painter skipped them and the press handler did NOT, which
 * was harmless only while the machine count stayed below the index of the
 * first function pad — at 21 machines nothing reached pad 81. Adding the
 * Phase 3 machines pushed the count past it, and pressing Undo would have
 * loaded a machine instead. So both paths go through this one function.
 *
 * The palette shows the FOCUSED STAGE's family, not the whole machine list:
 * six sources on SRC, twenty-one effects on an insert. Two things fall out of
 * that and both are worth having. The SHIFT bank is retired — each family fits
 * the 21 free pads outright — which in turn kills one of the latched-Shift
 * symptoms, where a palette pad loaded machine+21 and Compressor arrived as
 * Wavescan. And a pad can no longer offer a machine the stage would refuse.
 *
 * 21 effects against 21 pads is exact. Adding an effect without freeing a pad
 * puts it out of reach of the surface entirely; that has happened twice. */
const PALETTE_SLOTS = PALETTE_PADS.filter(
    (pad) => pad !== PAD_UNDO && pad !== PAD_MEMO && pad !== PAD_SONG);

function paletteMachine(pad) {
    const i = PALETTE_SLOTS.indexOf(pad);
    if (i < 0) return -1;
    const fam = familyFor(focusStage);
    return i < fam.length ? fam[i] : -1;
}


/* Whether SHIFT is down.
 *
 * Tracking it locally from CC 49 is correct — shadow_ui.js notes that the
 * shim's own shift tracking does NOT work in overtake mode, which is why it
 * tracks locally too — but it LATCHES. See a shift-ON, miss the release, and
 * the module believes Shift is held forever.
 *
 * That is not hypothetical: measured on the device, the JS side reported Shift
 * held across 495 consecutive events while the shim's control block said 0.
 * And latched Shift does not look like one bug, it looks like several
 * unrelated ones — a knob edits the base value instead of writing a parameter
 * lock, a palette pad loads machine+21 so Compressor arrives as Wavescan,
 * and the stage pad opens the sample browser rather than switching stages. Every
 * one of those was reported separately from hardware and chased separately.
 *
 * The origin is the launch gesture. Overwork is opened with Shift+Vol+jog
 * click; shadow_ui does not forward MIDI to a module until init() has
 * returned, and init was measured taking about six seconds. The release lands
 * inside that window and is discarded, so the module starts life latched.
 *
 * So: ignore Shift for a short settling window after init. The user cannot
 * still be holding the launch combo a second later, and any genuine press
 * after that is tracked normally. */
const SHIFT_SETTLE_TICKS = 44;      /* about a second at ~44 ticks/sec */

function shiftDown() {
    return shiftHeld;
}

/* Called every tick. Masking reads during the settling window is not enough:
 * the stale flag simply takes effect the moment the window closes. It has to
 * be CLEARED while the window is open, so that whatever arrived from the
 * launch combo is gone by the time the window shuts and any genuine press
 * afterwards tracks normally. */
function settleShift() {
    if (tickCount < SHIFT_SETTLE_TICKS) shiftHeld = false;
}

/* ----------------------------------------------------------------- state */

let editPage   = EDIT_FX;
let attrMode   = MODE_NONE;
let focusStage = 0;          /* which stage the palette and knobs address   */
let patPage    = 0;          /* 0-3, the visible sixteen of up to 64 steps  */
let shiftHeld  = false;
let needsRedraw = true;
let tickCount  = 0;
let resumeRepaints = 0;

let sampleMode  = false;     /* the sample browser is open                  */
let samples     = [];
let sampleIndex = 0;
let sampleLoaded = '';       /* name the ENGINE reports holding              */
let sampleFrames = 0;
let sampleStatus = '';       /* last transfer result, shown on screen        */
let samplePath   = '';       /* file the ENGINE says this patch wants        */

let presetMode  = false;
let presetIndex = 0;         /* 0 = "Save new", 1..n = a stored preset      */
let presets     = [];
let undoState   = [0, 0, 0];   /* undo / redo / memo availability */

let heldStep   = -1;         /* absolute step index held down, or -1        */
let heldUsed   = false;      /* a lock or attribute edit happened this hold */
let clearAt    = 0;          /* PAD_CLEAR press time, for the hold gesture  */
let fillAt     = 0;
let fillLatched = false;
let liveRec     = 0;
let songOn      = 0;
let curPattern  = 0;
let memoAt      = 0;

/* Feedback protection. schwung's own guard walks chain SLOTS only, so it never
 * sees an overtake module at all — Smack had to grow its own for exactly this
 * reason and Overwork is in the same position: it reads the mic and puts the
 * result on the speakers.
 *
 *   monitor      what the engine is actually doing right now
 *   monitorUser  the user's explicit choice, or null while the guard is in
 *                charge. A manual override survives until the risk state
 *                itself changes, so the guard never fights the user. */
let monitor     = 1;
let monitorUser = null;
let atRisk      = false;
let hwInput     = 0;
let copyBuf    = null;

/* mirrored DSP state */
let machineName = ['Bypass', 'Bypass', 'Bypass'];
let machineList = [];
let condList    = [];
let fxLabels    = [[], [], []];
let effVals     = [[], [], []];

/* Which machines each stage accepts, asked of the ENGINE rather than derived
 * here. A local copy of a table the engine owns is a copy that drifts, and
 * this one decides what the pads can even load. */
let familyCodes = [[], [], []];

function familyFor(stage) { return familyCodes[stage] || []; }

/* The machine knobs mirror a POSITION in the family; the engine speaks machine
 * codes. Sweeping raw codes would stall on every code the stage refuses, which
 * on hardware reads as a dead encoder rather than as a rule being enforced. */
function codeToPos(stage, code) {
    const i = familyFor(stage).indexOf(code);
    return i < 0 ? 0 : i;
}
function posToCode(stage, pos) {
    const fam = familyFor(stage);
    if (!fam.length) return 0;
    return fam[Math.max(0, Math.min(fam.length - 1, pos))];
}
let cfg         = {};        /* key -> value for everything the knobs edit  */
let steps       = [];        /* per step: {active, cond, micro, retrig, nlocks} */
let seqPos      = 0;
let seqLen      = 16;
let seqOn       = 1;

for (let i = 0; i < MAX_STEPS; i++) {
    steps.push({ active: 0, cond: 0, micro: 0, retrig: 0, nlocks: 0, prob: 100 });
}

/* ------------------------------------------------------------ DSP access */

/* EVERY read here is a blocking round-trip to the shim, serviced once per SPI
 * frame (~23 ms) and abandoned after 100 ms — schwung's own comment above
 * js_shadow_get_param calls it "the where-does-the-tick-time-go measurement".
 * fetchAll() wants ~58 keys, which one at a time is over a second of stall.
 * schwung provides a bulk path for exactly this (host_module_get_params, the
 * BULK_GET request the shim answers in a single frame), so use it. */
function getParam(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? '' : `${v}`;
}

function getNum(key) {
    const n = parseInt(getParam(key), 10);
    return Number.isFinite(n) ? n : 0;
}

/* Wire format, from shim_handle_param_bulk: "<count>\n" then count records of
 * "<len>\n<bytes>". The response has the same shape carrying values. The shim
 * caps a request at 64 items, so callers chunk. */
const BULK_MAX = 48;

function encodeBulk(items) {
    let s = `${items.length}\n`;
    for (const it of items) s += `${it.length}\n${it}`;
    return s;
}

function decodeBulk(blob, expected) {
    const out = new Array(expected).fill('');
    let p = 0;
    const readLen = () => {
        let n = 0, any = false;
        while (p < blob.length && blob[p] >= '0' && blob[p] <= '9') {
            n = n * 10 + (blob.charCodeAt(p) - 48); p++; any = true;
        }
        if (!any || blob[p] !== '\n') return -1;
        p++;
        return n;
    };
    const count = readLen();
    if (count < 0) return null;
    for (let i = 0; i < count && i < expected; i++) {
        const len = readLen();
        if (len < 0) return null;
        out[i] = blob.slice(p, p + len);
        p += len;
    }
    return out;
}

/* Read many keys in one round-trip. Falls back to individual reads when the
 * host predates the bulk binding, so this is safe on any schwung. */
function getParams(keys) {
    const out = {};
    if (typeof host_module_get_params !== 'function') {
        for (const k of keys) out[k] = getParam(k);
        return out;
    }
    for (let base = 0; base < keys.length; base += BULK_MAX) {
        const chunk = keys.slice(base, base + BULK_MAX);
        const blob = host_module_get_params(encodeBulk(chunk));
        const vals = blob === null || blob === undefined ? null : decodeBulk(`${blob}`, chunk.length);
        if (!vals) {
            for (const k of chunk) out[k] = getParam(k);
            continue;
        }
        for (let i = 0; i < chunk.length; i++) out[chunk[i]] = vals[i];
    }
    return out;
}

function setNum(key, v) {
    cfg[key] = v;
    host_module_set_param(key, `${v}`);
}

/* Stages, mirroring work_core.h: stage 0 is SRC and the two inserts follow it.
 * The engine keys them "src" / "fx1" / "fx2", so fx1 is stage 1 — off by one
 * from the array index on purpose, because that is what the engine answers to.
 * Parameters, labels and effective values all key off the same suffix. */
const N_INSERTS     = 2;
const N_STAGES      = 1 + N_INSERTS;
const STAGE_SRC     = 0;
const STAGE_KEY     = ['src', 'fx1', 'fx2'];
const STAGE_LABEL   = ['SRC', 'FX 1', 'FX 2'];

function paramKey(stage, knob) { return `${STAGE_KEY[stage]}_p${knob + 1}`; }
function labelsKey(stage) { return stage === STAGE_SRC ? 'labels_src' : `labels${stage}`; }
function effKey(stage)    { return stage === STAGE_SRC ? 'eff_src'    : `eff${stage}`; }

/* Lock indices, mirroring the map in work_core.h. The map is APPEND-ONLY, so
 * stage 2's parameters sit ABOVE the machine and mix entries rather than after
 * stage 1's — computing stage * 8 + knob gives the wrong index for the last
 * stage and would write a lock onto the SRC MACHINE select. */
const LOCK_MACH0    = 16;
const LOCK_MIX      = 18;
const LOCK_S2P0     = 19;
const LOCK_MACH2    = 27;
const DEST_MAX      = N_STAGES * 8 - 1;

function lockForParam(stage, knob) {
    return stage < 2 ? stage * 8 + knob : LOCK_S2P0 + knob;
}
function isMachineKey(key) {
    return STAGE_KEY.indexOf(key) >= 0;
}
function lockForMachine(stage) {
    return stage < 2 ? LOCK_MACH0 + stage : LOCK_MACH2;
}

/* The knob descriptors for the current edit page. Stage labels come from the
 * DSP so they always match whichever machine the focused stage holds. */
function pageKnobs() {
    if (editPage === EDIT_FX) {
        const out = [];
        for (let i = 0; i < 8; i++) {
            const lab = fxLabels[focusStage][i];
            out.push({
                key: paramKey(focusStage, i),
                label: lab && lab.length ? lab : '',
                lock: lockForParam(focusStage, i),
                min: 0, max: 127
            });
        }
        return out;
    }
    if (editPage === EDIT_VFILT) {
        return [
            { key: 'vf_base',  label: 'BASE',  lock: -1, min: 0, max: 127 },
            { key: 'vf_width', label: 'WDTH',  lock: -1, min: 0, max: 127 },
            { key: 'vf_reso',  label: 'RESO',  lock: -1, min: 0, max: 127 },
            { key: 'vf_env',   label: 'ENV',   lock: -1, min: 0, max: 127 },
            { key: 'vf_atk',   label: 'ATK',   lock: -1, min: 0, max: 127 },
            { key: 'vf_dec',   label: 'DEC',   lock: -1, min: 0, max: 127 },
            { key: 'vf_track', label: 'KEY',   lock: -1, min: 0, max: 127 },
            { key: '', label: '', lock: -1, min: 0, max: 0 }
        ];
    }
    if (editPage === EDIT_MENV) {
        return [
            { key: 'menv_dest',  label: 'DEST',  lock: -1, min: -1, max: DEST_MAX },
            { key: 'menv_atk',   label: 'ATK',   lock: -1, min: 0, max: 127 },
            { key: 'menv_hold',  label: 'HOLD',  lock: -1, min: 0, max: 127 },
            { key: 'menv_dec',   label: 'DEC',   lock: -1, min: 0, max: 127 },
            { key: 'menv_depth', label: 'DEP',   lock: -1, min: 0, max: 127 },
            { key: '', label: '', lock: -1, min: 0, max: 0 },
            { key: '', label: '', lock: -1, min: 0, max: 0 },
            { key: '', label: '', lock: -1, min: 0, max: 0 }
        ];
    }
    if (editPage === EDIT_LFO1 || editPage === EDIT_LFO2 || editPage === EDIT_LFO3) {
        const n = editPage - EDIT_LFO1 + 1;
        return [
            { key: `lfo${n}_dest`,  label: 'DEST',  lock: -1, min: -1, max: DEST_MAX },
            { key: `lfo${n}_spd`,   label: 'SPD',   lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_mult`,  label: 'MULT',  lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_wave`,  label: 'WAVE',  lock: -1, min: 0, max: 6 },
            { key: `lfo${n}_depth`, label: 'DEP',   lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_phase`, label: 'SPH',   lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_trig`,  label: 'TRIG',  lock: -1, min: 0, max: 1 },
            { key: '', label: '', lock: -1, min: 0, max: 0 }
        ];
    }
    /* EDIT_GLOBAL. The machine knobs keep CODE bounds rather than family
     * bounds, because `min`/`max` here also clamp a machine parameter-LOCK and
     * a lock stores a machine code. Stepping within the family is the knob
     * handler's job — see adjustKnob. Built from N_STAGES so a stage cannot be
     * left off the page the way one was left out of the label refresh. */
    const out = [];
    for (let s = 0; s < N_STAGES; s++) {
        out.push({ key: STAGE_KEY[s], label: STAGE_LABEL[s], stage: s,
                   lock: lockForMachine(s), min: 0, max: nMachines() - 1 });
    }
    out.push({ key: 'mix',     label: 'MIX', lock: LOCK_MIX, min: 0, max: 127 });
    out.push({ key: 'seq_len', label: 'LEN', lock: -1, min: 1, max: MAX_STEPS });
    while (out.length < 8) out.push({ key: '', label: '', lock: -1, min: 0, max: 0 });
    return out;
}

/* The scalars fetchAll mirrors, in one place so the batch and the unpack
 * cannot drift apart. */
const SCALAR_KEYS = [
    'mix', 'seq_len', 'seq_on', 'fill', 'live_rec', 'song_on', 'pattern',
    'monitor', 'hw_input'
];

/* Everything the screen and LEDs show, pulled in one pass — two bulk
 * round-trips rather than the ~58 single reads this used to cost. That
 * mattered: fetchAll runs from the machine-select handler, and at ~23 ms a key
 * the old version stalled the UI for over a second per detent. */
function fetchAll() {
    const keys = [];
    /* Static tables — asked for once, then never again. The families are
     * compiled into the engine and cannot change while it runs. */
    if (machineList.length === 0) keys.push('machines');
    if (condList.length === 0) keys.push('conds');
    if (familyFor(STAGE_SRC).length === 0) keys.push('src_codes', 'fx_codes');

    for (let s = 0; s < N_STAGES; s++) {
        keys.push(STAGE_KEY[s], labelsKey(s), effKey(s));
        for (let i = 0; i < 8; i++) keys.push(paramKey(s, i));
    }
    for (const k of pageKnobs()) if (k.key && keys.indexOf(k.key) < 0) keys.push(k.key);
    for (const k of SCALAR_KEYS) if (keys.indexOf(k) < 0) keys.push(k);
    keys.push('undo_state');

    const stepKeys = stepKeysForPage();
    const v = getParams(keys.concat(stepKeys));

    const num = (key, dflt) => {
        const n = parseInt(v[key], 10);
        return Number.isFinite(n) ? n : (dflt || 0);
    };

    if (v.machines) machineList = v.machines.split(',');
    if (v.conds) condList = v.conds.split(',');
    if (v.src_codes && v.fx_codes) {
        const s2n = (t) => t.split(',').map((x) => parseInt(x, 10)).filter(Number.isFinite);
        familyCodes = [s2n(v.src_codes)];
        for (let i = 1; i < N_STAGES; i++) familyCodes.push(s2n(v.fx_codes));
    }

    for (let s = 0; s < N_STAGES; s++) {
        const code = num(STAGE_KEY[s]);
        cfg[STAGE_KEY[s]] = code;
        machineName[s] = machineList[code] || `#${code}`;
        const lab = v[labelsKey(s)];
        fxLabels[s] = lab ? lab.split(',') : [];
        const ev = v[effKey(s)];
        effVals[s] = ev ? ev.split(',').map((x) => parseInt(x, 10)) : [];
        for (let i = 0; i < 8; i++) cfg[paramKey(s, i)] = num(paramKey(s, i));
    }

    for (const k of pageKnobs()) if (k.key) cfg[k.key] = num(k.key);
    for (const k of SCALAR_KEYS) cfg[k] = num(k);

    const us = v.undo_state;
    undoState = us ? us.split(':').map((x) => parseInt(x, 10) || 0) : [0, 0, 0];
    seqLen = num('seq_len') || 16;
    seqOn  = num('seq_on');
    fillLatched = num('fill') !== 0;
    liveRec = num('live_rec');
    songOn = num('song_on');
    curPattern = num('pattern');
    monitor = num('monitor');
    hwInput = num('hw_input');

    unpackSteps(v, stepKeys);
    needsRedraw = true;
}

/* Step metadata for the visible page only — sixteen keys, not sixty-four. */
function stepKeysForPage() {
    const base = patPage * PAGE_STEPS;
    const keys = [];
    for (let i = 0; i < PAGE_STEPS; i++) {
        const idx = base + i;
        if (idx >= MAX_STEPS) break;
        keys.push(`step${idx}`);
    }
    return keys;
}

function unpackSteps(v, keys) {
    for (const key of keys) {
        const idx = parseInt(key.slice(4), 10);
        const raw = v[key];
        const f = raw ? raw.split(':') : [];
        steps[idx] = {
            active: parseInt(f[0], 10) || 0,
            cond:   parseInt(f[1], 10) || 0,
            micro:  parseInt(f[2], 10) || 0,
            retrig: parseInt(f[3], 10) || 0,
            nlocks: parseInt(f[4], 10) || 0,
            prob:   parseInt(f[5], 10) || 100
        };
    }
}

function fetchSteps() {
    const keys = stepKeysForPage();
    unpackSteps(getParams(keys), keys);
}


/* ------------------------------------------------------------ sample load
 *
 * Tier B's SRC machines need audio in the engine, and the constraint that
 * shapes the whole route is that work_set_param runs on the SHIM'S AUDIO
 * THREAD (shim_handle_param_bulk's own comment: "this runs on the audio thread
 * ~44x/sec"). The DSP therefore must not open files. The UI reads the WAV
 * instead — schwung's host_read_file_base64 is the binary-safe binding, since
 * host_read_file hands back a C string and would stop at the first NUL, which
 * in a WAV is usually inside the header.
 *
 * The engine wants interleaved 16-bit stereo, so anything else is converted
 * here: 8/24/32-bit and float PCM are scaled, mono is duplicated across both
 * channels, and extra channels past the second are dropped. Sample RATE is NOT
 * converted — playback pitch follows the file, which is what a sampler does
 * and what TUNE is for.
 *
 * Transfer is chunked because each set_param crosses the same shared-memory
 * channel as everything else (SHADOW_PARAM_VALUE_LEN is 64 KB) and is serviced
 * once per SPI frame. CHUNK_FRAMES keeps each message inside that and spreads
 * the cost over several frames rather than one enormous blocking write.
 */
/* VERIFIED ON THE DEVICE, 2026-07-29 — these are not guesses. The Move keeps
 * its library under UserLibrary, not at the top of UserData, and Schwung's own
 * resampler and skipback write into DATED SUBDIRECTORIES
 * (Samples/Schwung/Skipback/2026-07-28/...), so a flat scan of the base finds
 * nothing at all. Hence the recursive walk below. */
const SAMPLE_DIRS = [
    '/data/UserData/UserLibrary/Samples',
    '/data/UserData/UserLibrary/Recordings'
];
const SAMPLE_SCAN_DEPTH = 4;     /* Samples/Schwung/Skipback/<date>/x.wav */
const SAMPLE_LIMIT = 200;        /* a browser, not an archive */

/* Reading and transferring a sample blocks for a noticeable moment, so the
 * "Reading..." frame has to reach the screen BEFORE that starts, not after. */
function flushDisplay() {
    if (typeof host_flush_display === 'function') host_flush_display();
}
const CHUNK_FRAMES = 8192;              /* 32 KB raw -> ~43 KB of base64 */

const B64_CHARS =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64_LOOKUP = (() => {
    const t = new Array(256).fill(-1);
    for (let i = 0; i < B64_CHARS.length; i++) t[B64_CHARS.charCodeAt(i)] = i;
    return t;
})();

function b64ToBytes(b64) {
    const out = [];
    let acc = 0, bits = 0;
    for (let i = 0; i < b64.length; i++) {
        const v = B64_LOOKUP[b64.charCodeAt(i)];
        if (v < 0) continue;                        /* '=' padding, newlines */
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push((acc >> bits) & 0xFF);
        }
    }
    return out;
}

function bytesToB64(bytes, from, count) {
    let out = '';
    const end = from + count;
    for (let i = from; i < end; i += 3) {
        const b0 = bytes[i];
        const b1 = i + 1 < end ? bytes[i + 1] : 0;
        const b2 = i + 2 < end ? bytes[i + 2] : 0;
        const v = (b0 << 16) | (b1 << 8) | b2;
        out += B64_CHARS[(v >> 18) & 63];
        out += B64_CHARS[(v >> 12) & 63];
        out += (i + 1 < end) ? B64_CHARS[(v >> 6) & 63] : '=';
        out += (i + 2 < end) ? B64_CHARS[v & 63] : '=';
    }
    return out;
}

function u32(b, at) {
    return (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
}
function u16(b, at) { return b[at] | (b[at + 1] << 8); }

/* Walk the RIFF chunk list rather than assuming fmt/data sit at fixed offsets —
 * plenty of WAVs carry LIST/fact/cue chunks first, and a fixed offset reads
 * metadata as audio. Returns interleaved stereo Int16 bytes, or null. */
function parseWav(bytes) {
    if (bytes.length < 44) return null;
    if (String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) !== 'RIFF') return null;
    if (String.fromCharCode(bytes[8], bytes[9], bytes[10], bytes[11]) !== 'WAVE') return null;

    let at = 12, fmt = null, dataAt = -1, dataLen = 0;
    while (at + 8 <= bytes.length) {
        const id = String.fromCharCode(bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]);
        const size = u32(bytes, at + 4);
        const body = at + 8;
        if (id === 'fmt ' && size >= 16) {
            fmt = {
                format: u16(bytes, body),
                channels: u16(bytes, body + 2),
                rate: u32(bytes, body + 4),
                bits: u16(bytes, body + 14)
            };
            /* WAVE_FORMAT_EXTENSIBLE hides the real format in the GUID's
             * first two bytes. */
            if (fmt.format === 0xFFFE && size >= 40) fmt.format = u16(bytes, body + 24);
        } else if (id === 'data') {
            dataAt = body;
            dataLen = Math.min(size, bytes.length - body);
        }
        at = body + size + (size & 1);              /* chunks are word-aligned */
    }
    if (!fmt || dataAt < 0 || !fmt.channels) return null;

    const bytesPerSample = fmt.bits >> 3;
    if (!bytesPerSample) return null;
    const frames = Math.floor(dataLen / (bytesPerSample * fmt.channels));
    if (frames <= 0) return null;

    const read = (at2) => {
        if (fmt.format === 3) {                     /* IEEE float */
            if (fmt.bits === 32) {
                const v = u32(bytes, at2);
                const sign = (v >>> 31) ? -1 : 1;
                const exp = (v >>> 23) & 0xFF;
                const man = v & 0x7FFFFF;
                if (exp === 0) return 0;
                const f = sign * (1 + man / 0x800000) * Math.pow(2, exp - 127);
                return Math.max(-32768, Math.min(32767, Math.round(f * 32767)));
            }
            return 0;
        }
        if (fmt.bits === 8) return (bytes[at2] - 128) << 8;      /* 8-bit is unsigned */
        if (fmt.bits === 16) {
            const v = u16(bytes, at2);
            return v >= 0x8000 ? v - 0x10000 : v;
        }
        if (fmt.bits === 24) {
            let v = bytes[at2] | (bytes[at2 + 1] << 8) | (bytes[at2 + 2] << 16);
            if (v >= 0x800000) v -= 0x1000000;
            return v >> 8;
        }
        if (fmt.bits === 32) {
            const v = u32(bytes, at2) | 0;
            return v >> 16;
        }
        return 0;
    };

    const out = new Array(frames * 4);              /* 2 channels x int16 LE */
    for (let f = 0; f < frames; f++) {
        const base = dataAt + f * bytesPerSample * fmt.channels;
        const l = read(base);
        const r = fmt.channels > 1 ? read(base + bytesPerSample) : l;
        const o = f * 4;
        out[o]     = l & 0xFF;
        out[o + 1] = (l >> 8) & 0xFF;
        out[o + 2] = r & 0xFF;
        out[o + 3] = (r >> 8) & 0xFF;
    }
    return { bytes: out, frames, rate: fmt.rate, channels: fmt.channels };
}

/* Push a parsed WAV into the engine. Returns the number of frames sent. */
function sendSample(name, wav, maxFrames) {
    let frames = wav.frames;
    if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;

    host_module_set_param('sample_begin', `${frames}:${name.slice(0, 24)}`);
    for (let at = 0; at < frames; at += CHUNK_FRAMES) {
        const n = Math.min(CHUNK_FRAMES, frames - at);
        host_module_set_param('sample_chunk', bytesToB64(wav.bytes, at * 4, n * 4));
    }
    host_module_set_param('sample_end', '1');
    return frames;
}

/* Ask the ENGINE what it is holding. Reporting success from our own side of
 * the transfer is worth nothing — the sample crosses a shared-memory channel
 * to another process, and only the far end knows whether it arrived. */
function refreshSampleState() {
    sampleFrames = parseInt(getParam('sample_frames'), 10) || 0;
    sampleLoaded = sampleFrames > 0 ? getParam('sample_name') : '';
    samplePath   = getParam('sample_path') || '';
}

/* `quiet` is set when a PRESET is doing the loading rather than the user. The
 * browser is not open then, so painting it would overwrite whatever view the
 * preset landed on, and closing it on success would be closing nothing. */
function loadSampleFile(path, quiet) {
    const name = path.slice(path.lastIndexOf('/') + 1).replace(/\.wav$/i, '');

    /* Every failure below sets a status the SCREEN shows. announce() alone
     * reaches the screen reader only, so with it off a failed load and a
     * successful one looked exactly the same: nothing happened. */
    sampleStatus = `Reading ${name}...`;
    needsRedraw = true;
    if (!quiet) { drawSampleBrowser(); flushDisplay(); }

    const b64 = typeof host_read_file_base64 === 'function'
        ? host_read_file_base64(path) : null;
    if (!b64) {
        sampleStatus = 'Could not read that file';
        announce(sampleStatus); needsRedraw = true; return false;
    }

    const wav = parseWav(b64ToBytes(`${b64}`));
    if (!wav) {
        sampleStatus = 'Not a WAV this can read';
        announce(sampleStatus); needsRedraw = true; return false;
    }

    const maxFrames = parseInt(getParam('sample_max'), 10) || 0;
    const sent = sendSample(name, wav, maxFrames);

    /* The engine is the authority on what landed. */
    refreshSampleState();
    if (sampleFrames <= 0) {
        sampleStatus = 'Transfer failed';
        announce(sampleStatus); needsRedraw = true; return false;
    }

    /* Record WHERE it came from, so saving this patch saves a patch that can
     * find its audio again. The engine only carries the string — it has no
     * filesystem, and work_set_param runs on the audio thread. */
    host_module_set_param('sample_path', path);
    samplePath = path;

    const secs = (sampleFrames / 44100).toFixed(1);
    sampleStatus = sent < wav.frames
        ? `Loaded ${name}, cut to ${secs}s`
        : `Loaded ${name}, ${secs}s`;
    announce(sampleStatus);
    if (quiet) return true;

    /* Close on success. Leaving the browser open after a load left every pad
     * inert with no sign why — you had picked your sample and the surface
     * still would not respond, which reads as "the machine palette is
     * broken". A FAILED load keeps the browser open, because there the next
     * thing you want is another file. */
    closeSampleBrowser();
    return true;
}

/* Every .wav under the sample directories, walking subdirectories.
 *
 * os.readdir returns a [names, errno] TUPLE and the listing includes "." and
 * ".." — treating it as a flat array of filenames is the bug that broke Mono's
 * preset browser. There is no stat binding to ask "is this a directory", so
 * the walk uses readdir itself: a successful listing means a directory, and
 * ENOTDIR on a file is simply how the recursion terminates. */
function scanSampleDir(dir, depth, found) {
    if (depth <= 0 || found.length >= SAMPLE_LIMIT) return;
    let result;
    try { result = os.readdir(dir); } catch (e) { return; }
    if (!result || result[1] !== 0) return;

    const subdirs = [];
    for (const f of result[0]) {
        if (f === '.' || f === '..') continue;
        const full = `${dir}/${f}`;
        if (/\.wav$/i.test(f)) {
            if (found.length < SAMPLE_LIMIT) found.push(full);
        } else if (f.indexOf('.') < 0) {
            subdirs.push(full);         /* no extension — probably a directory */
        }
    }
    /* Files first, then descend, so the shallowest samples head the list. */
    for (const sub of subdirs) scanSampleDir(sub, depth - 1, found);
}

function listSamples() {
    const found = [];
    for (const dir of SAMPLE_DIRS) scanSampleDir(dir, SAMPLE_SCAN_DEPTH, found);
    return found;
}

/* Open the browser and scan for files. The scan is filesystem work, not param
 * round-trips, so it is cheap — but it happens on the gesture rather than every
 * tick so a directory with hundreds of files never touches the audio path. */
function openSampleBrowser() {
    samples = listSamples();
    sampleIndex = 0;
    sampleStatus = '';
    refreshSampleState();
    /* start on whatever is already loaded, if it is still in the list */
    if (sampleLoaded) {
        const at = samples.findIndex((f) =>
            f.slice(f.lastIndexOf('/') + 1).replace(/\.wav$/i, '') === sampleLoaded);
        if (at >= 0) sampleIndex = at;
    }
    sampleMode = true;
    needsRedraw = true;
    announceView(samples.length
        ? `Sample browser, ${samples.length} file${samples.length === 1 ? '' : 's'}`
        : 'Sample browser, no WAV files found');
}

function closeSampleBrowser() {
    sampleMode = false;
    needsRedraw = true;
    announceView(sampleLoaded ? `Overwork, sample ${sampleLoaded}` : 'Overwork');
}

function drawSampleBrowser() {
    clear_screen();
    print(0, 1, 'SAMPLES', 1);
    const count = `${samples.length}`;
    print(128 - text_width(count), 1, count, 1);
    fill_rect(0, 9, 128, 1, 1);

    if (!samples.length) {
        print(2, 20, 'No .wav found in', 1);
        print(2, 30, 'UserData/Samples', 1);
    } else {
        const rows = 4;
        const top = Math.max(0, Math.min(sampleIndex - 1, samples.length - rows));
        for (let r = 0; r < rows; r++) {
            const idx = top + r;
            if (idx >= samples.length) break;
            const y = 13 + r * 10;
            const path = samples[idx];
            let label = path.slice(path.lastIndexOf('/') + 1);
            /* a dot marks the one the engine is actually holding */
            const isLoaded = sampleLoaded &&
                label.replace(/\.wav$/i, '') === sampleLoaded;
            label = (isLoaded ? '\u2022' : ' ') + label;
            if (idx === sampleIndex) fill_rect(0, y - 1, 128, 9, 1);
            print(2, y, label.length > 24 ? label.slice(0, 24) : label,
                  idx === sampleIndex ? 0 : 1);
        }
    }

    fill_rect(0, 55, 128, 1, 1);
    /* The status of the last attempt beats a static hint: a load that failed
     * and a load that worked used to look identical on screen. NOT
     * "Back:exit" — in overtake mode Back belongs to the host and leaves the
     * module altogether. */
    const foot = sampleStatus || 'Click:load  Sh+Stage:close';
    print(0, 57, foot.length > 25 ? foot.slice(0, 25) : foot, 1);
}

/* --------------------------------------------------------------- editing */

/* decodeDelta reports ACCUMULATED encoder movement — the shim batches ticks
 * per SPI frame, so one brisk turn arrives as a single event carrying 20+.
 * Applying that raw to a 21-entry machine select lands on an end stop every
 * time ("fx 2 only does warble or bypass"); clamping it to one step per event
 * made crossing the list take twenty separate clicks ("super slow"). Cap the
 * magnitude at a quarter of the range instead: a single detent still moves
 * exactly one, and the fastest possible spin crosses the whole list in four. */
function scaledMove(k, delta) {
    const span = k.max - k.min;
    const cap  = span > 0 ? Math.ceil(span / 4) : 1;
    const mag  = Math.min(Math.abs(delta), Math.max(1, cap));
    return delta > 0 ? mag : -mag;
}

/* The same rule applied to a machine select, whose range is its stage's FAMILY
 * rather than the descriptor's code bounds. Scaling by the code range instead
 * would let one flick cross the whole six-machine source family. */
function familyMove(stage, delta) {
    return scaledMove({ min: 0, max: Math.max(0, familyFor(stage).length - 1) }, delta);
}

function toggleStep(idx) {
    const st = steps[idx];
    st.active = st.active ? 0 : 1;
    host_module_set_param(`step${idx}`, `${st.active}:${st.cond}:${st.micro}:${st.retrig}`);
    if (!st.active) {
        host_module_set_param(`locks${idx}`, '');
        st.nlocks = 0;
    }
    announce(`Step ${idx + 1} ${st.active ? 'on' : 'off'}`);
    needsRedraw = true;
}

function writeStepAttrs(idx) {
    const st = steps[idx];
    host_module_set_param(`step${idx}`, `${st.active}:${st.cond}:${st.micro}:${st.retrig}`);
}

/* Hold a step and turn a knob: that knob's parameter is locked on that step.
 * The lock value starts from whatever the parameter currently is, so nudging
 * a knob one detent locks the value you can already hear. */
function lockKnob(idx, knob, delta) {
    const k = pageKnobs()[knob];
    if (!k || k.lock < 0) {
        announce('Not lockable');
        return;
    }
    const st = steps[idx];
    if (!st.active) {                    /* locking implies a trig */
        st.active = 1;
        writeStepAttrs(idx);
    }

    const cur = getParam(`lock${idx}_${k.lock}`);
    let base = parseInt(cur, 10);
    if (!Number.isFinite(base) || cur === '') base = cfg[k.key] | 0;

    let v;
    if (isMachineKey(k.key)) {
        /* A machine lock stores a machine CODE, and the engine ignores one its
         * stage will not accept. Stepping through the family means the value
         * written is always one that will actually take — otherwise the screen
         * would show a lock that does nothing on playback. */
        const stage = STAGE_KEY.indexOf(k.key);
        v = posToCode(stage, codeToPos(stage, base) + familyMove(stage, delta));
    } else {
        v = base + scaledMove(k, delta);
        if (v < k.min) v = k.min;
        if (v > k.max) v = k.max;
    }
    host_module_set_param(`lock${idx}_${k.lock}`, `${v}`);

    heldUsed = true;
    fetchSteps();
    announceParameter(`${k.label} step ${idx + 1}`, `${v}`);
    needsRedraw = true;
}

function clearStepLocks(idx) {
    host_module_set_param(`locks${idx}`, '');
    steps[idx].nlocks = 0;
    heldUsed = true;
    announce(`Step ${idx + 1} locks cleared`);
    needsRedraw = true;
}

/* Jog while a step is held edits whichever attribute the mode pads selected. */
function adjustHeldAttr(delta) {
    if (heldStep < 0) return;
    const st = steps[heldStep];

    if (attrMode === MODE_COND) {
        st.cond = Math.max(0, Math.min(condList.length - 1 || 18, st.cond + delta));
        writeStepAttrs(heldStep);
        announceParameter(`Step ${heldStep + 1} condition`, condList[st.cond] || `${st.cond}`);
    } else if (attrMode === MODE_MICRO) {
        st.micro = Math.max(-23, Math.min(23, st.micro + delta));
        writeStepAttrs(heldStep);
        announceParameter(`Step ${heldStep + 1} micro`, `${st.micro}`);
    } else if (attrMode === MODE_RETRIG) {
        st.retrig = Math.max(0, Math.min(4, st.retrig + delta));
        writeStepAttrs(heldStep);
        announceParameter(`Step ${heldStep + 1} retrig`, `${st.retrig}`);
    } else if (attrMode === MODE_PROB) {
        st.prob = Math.max(1, Math.min(100, (st.prob || 100) + delta));
        host_module_set_param(`prob${heldStep}`, `${st.prob}`);
        announceParameter(`Step ${heldStep + 1} probability`, `${st.prob}%`);
    } else {
        announce('Pick a mode: cond, micro, retrig or prob');
        return;
    }
    heldUsed = true;
    needsRedraw = true;
}

function adjustKnob(knob, delta) {
    const k = pageKnobs()[knob];
    if (!k || !k.key) return;

    /* A machine knob steps through its stage's FAMILY. Stepping raw machine
     * codes would stop dead on every code the stage refuses — the encoder
     * would look broken rather than the rule looking enforced. */
    if (isMachineKey(k.key)) {
        const stage = STAGE_KEY.indexOf(k.key);
        const cur = cfg[k.key] | 0;
        const code = posToCode(stage, codeToPos(stage, cur) + familyMove(stage, delta));
        if (code === cur) return;
        setNum(k.key, code);
        fetchAll();
        announceParameter(k.label, knobText(knob));
        needsRedraw = true;
        return;
    }

    let v = (cfg[k.key] | 0) + scaledMove(k, delta);
    if (v < k.min) v = k.min;
    if (v > k.max) v = k.max;
    if (v === cfg[k.key]) return;

    setNum(k.key, v);
    if (k.key === 'seq_len') seqLen = v;

    announceParameter(k.label, knobText(knob));
    needsRedraw = true;
}

function loadMachine(code) {
    sampleStatus = '';
    setNum(STAGE_KEY[focusStage], code);
    fetchAll();
    announce(`${machineList[code] || code} in ${STAGE_LABEL[focusStage]}`);
}

function copyPage() {
    const base = patPage * PAGE_STEPS;
    copyBuf = [];
    for (let i = 0; i < PAGE_STEPS; i++) {
        const idx = base + i;
        copyBuf.push({
            attrs: `${steps[idx].active}:${steps[idx].cond}:${steps[idx].micro}:${steps[idx].retrig}`,
            locks: getParam(`locks${idx}`)
        });
    }
    announce(`Page ${patPage + 1} copied`);
}

function pastePage() {
    if (!copyBuf) { announce('Nothing copied'); return; }
    const base = patPage * PAGE_STEPS;
    for (let i = 0; i < PAGE_STEPS; i++) {
        const idx = base + i;
        host_module_set_param(`step${idx}`, copyBuf[i].attrs);
        host_module_set_param(`locks${idx}`, copyBuf[i].locks);
    }
    fetchSteps();
    announce(`Pasted to page ${patPage + 1}`);
    needsRedraw = true;
}

function clearPage() {
    const base = patPage * PAGE_STEPS;
    for (let i = 0; i < PAGE_STEPS; i++) {
        host_module_set_param(`step${base + i}`, '0:0:0:0');
        host_module_set_param(`locks${base + i}`, '');
    }
    fetchSteps();
    announce(`Page ${patPage + 1} cleared`);
    needsRedraw = true;
}

/* ------------------------------------------------------- feedback guard */

/* Speakers live and no cable in the input jack means the mic is hearing the
 * speakers: mute the input until that changes. Only armed for builds that
 * actually read the hardware input. */
function pollFeedbackGuard() {
    if (!hwInput) return;
    if (typeof host_speaker_active !== 'function' ||
        typeof host_line_in_connected !== 'function') return;

    let risk;
    try { risk = host_speaker_active() && !host_line_in_connected(); }
    catch (e) { return; }

    if (risk === atRisk) return;          /* nothing changed */
    atRisk = risk;
    monitorUser = null;                   /* a new situation clears the override */

    const want = risk ? 0 : 1;
    if (want !== monitor) {
        monitor = want;
        host_module_set_param('monitor', `${monitor}`);
        announce(risk ? 'Feedback risk, input muted' : 'Input restored');
        needsRedraw = true;
    }
}

function toggleMonitor() {
    monitor = monitor ? 0 : 1;
    monitorUser = monitor;
    host_module_set_param('monitor', `${monitor}`);
    announce(monitor
        ? (atRisk ? 'Input on, feedback risk' : 'Input on')
        : 'Input muted');
    needsRedraw = true;
}

/* --------------------------------------------------------------- presets */

/* QuickJS `os.readdir` answers with a [names, errno] TUPLE, and the raw
 * listing includes "." and "..". Treating it as a flat filename array is
 * silently wrong: /\.json$/.test(namesArray) tests the *stringified* array, so
 * the whole listing is kept or dropped as one blob depending on which entry
 * the filesystem happened to return last — and when it is kept, the later
 * .replace runs against an Array and throws. That is precisely how Mono
 * shipped a preset browser that never listed a file. Unwrap the tuple. */
function readPresetDir() {
    let result;
    try { result = os.readdir(PRESET_DIR); } catch (e) { return []; }
    if (!Array.isArray(result) || !Array.isArray(result[0])) return [];
    return result[0].filter((e) =>
        typeof e === 'string' && e !== '.' && e !== '..');
}

function loadPresetList() {
    const found = [];
    for (const file of readPresetDir()) {
        if (!/\.json$/i.test(file)) continue;
        let name = file.replace(/\.json$/i, '');
        try {
            const parsed = JSON.parse(host_read_file(`${PRESET_DIR}/${file}`) || '{}');
            if (parsed.name) name = String(parsed.name);
        } catch (e) { /* a corrupt preset still lists under its filename */ }
        found.push({ name, file });
    }
    found.sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
    presets = found;
}

function safeStem(raw) {
    const s = String(raw).replace(/[^A-Za-z0-9 _-]/g, '').trim();
    return (s || 'pattern').slice(0, 48);
}

function nextPresetName() {
    let n = 1;
    const used = new Set(presets.map((p) => p.name.toLowerCase()));
    while (used.has(`pattern ${n}`)) n++;
    return `Pattern ${n}`;
}

/* `os.rename` reports failure with a NEGATIVE RETURN CODE rather than
 * throwing, so try/catch around it calls every failure a success and strands
 * the payload in the .tmp file. Check the code, and fall back to writing the
 * destination directly so a rename refusal still saves. */
function writePresetAtomically(file, payload) {
    if (typeof host_write_file !== 'function') return false;
    const path = `${PRESET_DIR}/${file}`;
    const temp = `${path}.tmp`;
    if (!host_write_file(temp, payload)) return false;
    let renamed = -1;
    try { renamed = os.rename(temp, path); } catch (e) { renamed = -1; }
    if (renamed === 0) return true;
    const direct = host_write_file(path, payload);
    try { os.remove(temp); } catch (e) {}
    return !!direct;
}

function savePreset() {
    if (typeof host_ensure_dir === 'function') host_ensure_dir(PRESET_DIR);
    else { try { os.mkdir(PRESET_DIR); } catch (e) {} }

    const name = nextPresetName();
    const file = `${safeStem(name)}.json`;
    const payload = JSON.stringify({
        v: 1,
        name: name,
        module: 'overwork',
        state: getParam('state')
    });

    if (!writePresetAtomically(file, payload)) {
        announce('Save failed');
        return;
    }
    loadPresetList();
    presetIndex = Math.max(1, presets.findIndex((p) => p.file === file) + 1);
    announce(`Saved ${name}`);
    needsRedraw = true;
}

function loadPreset(entry) {
    if (!entry || typeof host_read_file !== 'function') return;
    let payload;
    try { payload = JSON.parse(host_read_file(`${PRESET_DIR}/${entry.file}`) || '{}'); }
    catch (e) { announce('Preset unreadable'); return; }
    if (!payload.state) { announce('Preset is empty'); return; }

    host_module_set_param('state', payload.state);
    fetchAll();
    restoreSample();
    announce(`Loaded ${entry.name}`);
    needsRedraw = true;
}

/* Bring back the audio a just-loaded preset expects.
 *
 * A preset stores the sample PATH, never the samples — a few seconds of stereo
 * audio does not belong in a patch file, and the blob crosses a 64 KiB channel.
 * So a source-machine preset restored every parameter, LFO, pattern and lock
 * and then played silence, which reads as a broken module rather than as a
 * patch whose audio has not been fetched yet.
 *
 * A missing file is reported and nothing else: the previous sample stays
 * loaded, because dropping it would turn "this preset's file moved" into
 * "everything is silent now". */
function restoreSample() {
    const want = getParam('sample_path') || '';
    if (!want) { samplePath = ''; return; }
    /* Ask what the ENGINE is holding rather than trusting our own mirror. The
     * path alone is not enough: clear the sample and load a preset naming that
     * same file and the mirror still says we have it, so nothing reloads and
     * the patch plays silence — the exact bug this function exists to fix. */
    const held = parseInt(getParam('sample_frames'), 10) || 0;
    if (want === samplePath && held > 0) return;
    if (!loadSampleFile(want, true)) {
        const file = want.slice(want.lastIndexOf('/') + 1);
        sampleStatus = `Missing ${file}`;
        announce(sampleStatus);
    }
}

function deletePreset(entry) {
    if (!entry) return;
    /* os.remove returns 0 or -errno and never throws — a bare try/catch here
     * would report every failure as a success. */
    let rc = -1;
    try { rc = os.remove(`${PRESET_DIR}/${entry.file}`); } catch (e) { rc = -1; }
    if (rc !== 0) { announce('Delete failed'); return; }
    loadPresetList();
    presetIndex = Math.min(presetIndex, presets.length);
    announce(`Deleted ${entry.name}`);
    needsRedraw = true;
}

function enterPresetMode() {
    loadPresetList();
    presetMode = true;
    presetIndex = presets.length ? 1 : 0;
    announceView(`Presets, ${presets.length} saved`);
    needsRedraw = true;
}

function drawPresets() {
    clear_screen();
    print(0, 1, 'PRESETS', 1);
    const count = `${presets.length}`;
    print(128 - text_width(count), 1, count, 1);
    fill_rect(0, 9, 128, 1, 1);

    /* a four-row window around the selection */
    const rows = 4;
    let top = Math.max(0, Math.min(presetIndex - 1, presets.length + 1 - rows));
    for (let r = 0; r < rows; r++) {
        const idx = top + r;
        if (idx > presets.length) break;
        const y = 13 + r * 10;
        const label = idx === 0 ? '+ Save new' : presets[idx - 1].name;
        if (idx === presetIndex) fill_rect(0, y - 1, 128, 9, 1);
        print(2, y, label.length > 24 ? label.slice(0, 24) : label,
              idx === presetIndex ? 0 : 1);
    }

    fill_rect(0, 55, 128, 1, 1);
    print(0, 57, 'Click:load  Shift+click:exit', 1);
}

/* --------------------------------------------------------------- display */

function knobText(i) {
    const k = pageKnobs()[i];
    if (!k || !k.key) return '';
    const v = cfg[k.key] | 0;

    if (isMachineKey(k.key)) {
        const nm = machineList[v] || `#${v}`;
        return nm.length > 6 ? nm.slice(0, 6) : nm;
    }
    if (k.key.endsWith('_wave')) return ['Tri', 'Sin', 'Sqr', 'Saw', 'Ramp', 'Exp', 'Rnd'][v % 7];
    if (k.key.endsWith('_trig')) return v ? 'Rtrg' : 'Free';
    if (k.key.endsWith('_dest')) {
        if (v < 0) return 'Off';
        /* A destination addresses a stage and a knob within it, so it names the
         * stage rather than counting FX slots — "SRCa" and "FX 2c", not "FX1a"
         * for something that is now the source. */
        const stage = v >> 3;
        const tag = STAGE_LABEL[stage] || `?${stage}`;
        return `${tag}${String.fromCharCode(97 + (v & 7))}`;
    }
    return `${v}`;
}

function drawUI() {
    clear_screen();

    /* Header: edit page + focused machine, then transport on the right */
    let title = EDIT_NAME[editPage];
    if (editPage === EDIT_FX) title = `${STAGE_LABEL[focusStage]} ${machineName[focusStage]}`;
    if (title.length > 20) title = title.slice(0, 20);
    print(0, 1, title, 1);

    const right = `P${curPattern + 1}${songOn ? 'S' : ''} ${seqOn ? '>' : '||'}${seqPos + 1}/${seqLen}`;
    print(128 - text_width(right), 1, right, 1);
    fill_rect(0, 9, 128, 1, 1);

    /* Four columns, two rows of knobs. A held step turns the row into the
     * lock editor, so the values shown are that step's locks. */
    const knobs = pageKnobs();
    for (let i = 0; i < 8; i++) {
        if (!knobs[i] || !knobs[i].label) continue;
        const x = (i % 4) * 32;
        const y = 13 + (i < 4 ? 0 : 1) * 20;
        const lab = knobs[i].label.length > 6 ? knobs[i].label.slice(0, 6) : knobs[i].label;
        print(x, y, lab, 1);

        let val = knobText(i);
        if (heldStep >= 0 && knobs[i].lock >= 0) {
            const lv = getParam(`lock${heldStep}_${knobs[i].lock}`);
            if (lv !== '' && parseInt(lv, 10) >= 0) val = `*${lv}`;
        } else if (editPage === EDIT_FX && effVals[focusStage].length === 8) {
            /* show the value actually reaching the DSP when it differs */
            const e = effVals[focusStage][i];
            if (Number.isFinite(e) && e !== (cfg[knobs[i].key] | 0)) val = `${e}~`;
        }
        print(x, y + 9, val, 1);
    }

    if (!monitor) {
        /* This is the difference between "broken" and "protecting you", so it
         * gets the footer outright rather than a corner glyph. */
        fill_rect(0, 46, 128, 9, 1);
        print(2, 47, atRisk ? 'INPUT MUTED - FEEDBACK' : 'INPUT MUTED', 0);
    }

    fill_rect(0, 55, 128, 1, 1);
    let foot;
    if (heldStep >= 0) {
        const st = steps[heldStep];
        foot = `S${heldStep + 1} ${condList[st.cond] || ''} u${st.micro} L${st.nlocks} ${st.prob}%`;
    } else if (sampleStatus) {
        /* The result of the last load, on the MAIN screen — the browser closes
         * on success, so without this the confirmation would vanish with it and
         * a load would once again leave no trace. Cleared by the next edit. */
        foot = sampleStatus;
    } else {
        foot = `Pg${patPage + 1} ${MODE_NAME[attrMode]}${fillLatched ? ' FILL' : ''}${liveRec ? ' REC' : ''}`;
        if (sampleLoaded) foot += ` ${sampleLoaded}`;
    }
    print(0, 57, foot.length > 24 ? foot.slice(0, 24) : foot, 1);
}

/* ------------------------------------------------------------------ LEDs */

function paintSteps(force) {
    const base = patPage * PAGE_STEPS;
    for (let i = 0; i < STEP_COUNT; i++) {
        const idx = base + i;
        let color = Black;
        if (idx < seqLen) {
            const st = steps[idx];
            if (st.active) {
                if (st.nlocks > 0) color = Green;        /* trig with locks */
                else color = White;                      /* plain trig      */
                if (st.cond !== 0) color = Cyan;         /* conditional     */
            } else {
                color = 0x08;                            /* in-length, empty */
            }
            if (idx === heldStep) color = BrightGreen;
            if (idx === seqPos && seqOn) color = BrightRed;
        }
        setLED(STEP_FIRST + i, color, force);
    }
}

function paintPalette(force) {
    for (let i = 0; i < PALETTE_PADS.length; i++) {
        const pad = PALETTE_PADS[i];
        const code = paletteMachine(pad);
        /* A pad past the end of this stage's family is DARKENED rather than
         * left alone: the source family is six long and the effect family
         * twenty-one, so switching stages must not leave fifteen pads still
         * lit with effects that the source stage would refuse. */
        if (code < 0) {
            if (pad !== PAD_UNDO && pad !== PAD_MEMO && pad !== PAD_SONG)
                setLED(pad, Black, force);
            continue;
        }
        /* A machine added to the engine without a colour here would otherwise
         * hand `undefined` to setLED and light nothing. */
        let color = MACHINE_COLOR[code] !== undefined ? MACHINE_COLOR[code] : LightGrey;
        /* the machine loaded in the focused stage burns brighter */
        if (code === (cfg[STAGE_KEY[focusStage]] | 0)) color = White;
        setLED(pad, color, force);
    }
}

function paintFunctions(force) {
    setLED(PAD_UNDO, undoState[0] ? BurntOrange : 0x08, force);
    setLED(PAD_MEMO, undoState[2] ? TealGreen : 0x08, force);
    setLED(PAD_SONG, songOn ? Lime : 0x0A, force);
}

function paintTransport(force) {
    setLED(PAD_PLAY,  liveRec ? Red : (seqOn ? BrightGreen : DarkGrey), force);
    setLED(PAD_FILL,  fillLatched ? BrightRed : (!monitor ? OrangeRed : 0x0C), force);
    /* One colour per stage, so the pad says which one the knobs address
     * without looking at the screen. Two colours for three stages would leave
     * FX 2 indistinguishable from FX 1. */
    setLED(PAD_STAGE, [SkyBlue, YellowGreen, BurntOrange][focusStage] || SkyBlue, force);
    setLED(PAD_PPAGE, [Blue, Cyan, Purple, OrangeRed][patPage % 4], force);
    if (shiftDown()) {
        /* while shift is held row 4 IS the step-attribute mode row */
        setLED(PAD_MODE_COND,   attrMode === MODE_COND   ? Cyan   : 0x0A, force);
        setLED(PAD_MODE_MICRO,  attrMode === MODE_MICRO  ? Purple : 0x0A, force);
        setLED(PAD_MODE_RETRIG, attrMode === MODE_RETRIG ? Blue   : 0x0A, force);
        setLED(PAD_LOCK_CLEAR,  heldStep >= 0 ? OrangeRed : 0x08, force);
        setLED(PAD_MODE_PROB,   attrMode === MODE_PROB ? YellowGreen : 0x0A, force);
        setLED(PAD_LIVE_REC,    liveRec ? Red : 0x0A, force);
        setLED(PAD_MONITOR,     monitor ? (atRisk ? BrightRed : BrightGreen) : DarkGrey, force);
    } else {
        setLED(PAD_EPAGE, [White, Cyan, Cyan, Cyan, Purple, LightGrey][editPage], force);
        setLED(PAD_COPY,  copyBuf ? TealGreen : 0x0A, force);
        setLED(PAD_PASTE, copyBuf ? Lime : 0x06, force);
        setLED(PAD_CLEAR, Red, force);
    }
}

function paintAll(force) {
    paintSteps(force);
    paintPalette(force);
    paintFunctions(force);
    paintTransport(force);
}

/* ----------------------------------------------------------------- input */

function handlePadPress(note) {
    /* While the preset browser is open only CLEAR (delete) is live; the rest
     * of the surface would otherwise edit a pattern you cannot see. */
    if (sampleMode) {
        /* The closing gesture MUST get through this guard. Swallowing every
         * pad while a modal is open is how a user ends up stuck in it with no
         * way back — the browser opens on SHIFT + stage pad and has to close
         * the same way. */
        if (shiftDown() && note === PAD_STAGE) { closeSampleBrowser(); return; }
        announce('Sample browser open. Shift and stage pad to close.');
        return;
    }
    if (presetMode && note !== PAD_CLEAR) {
        announce('Preset browser open');
        return;
    }

    /* Machine palette for the focused stage. A pad that is undo, memo or song
     * is never a palette pad, so those keep working. Shift is deliberately not
     * consulted: each family fits the pads outright, so a shift-latched palette
     * press now loads exactly what the pad shows. */
    const pi = paletteMachine(note);
    if (pi >= 0) { loadMachine(pi); return; }

    /* SHIFT + row 4 selects the step-attribute mode the jog edits. */
    if (shiftDown()) {
        switch (note) {
            case PAD_MODE_COND:
                attrMode = attrMode === MODE_COND ? MODE_NONE : MODE_COND;
                announce(attrMode === MODE_COND ? 'Condition mode' : 'Mode off');
                needsRedraw = true;
                return;
            case PAD_MODE_MICRO:
                attrMode = attrMode === MODE_MICRO ? MODE_NONE : MODE_MICRO;
                announce(attrMode === MODE_MICRO ? 'Micro timing mode' : 'Mode off');
                needsRedraw = true;
                return;
            case PAD_MODE_RETRIG:
                attrMode = attrMode === MODE_RETRIG ? MODE_NONE : MODE_RETRIG;
                announce(attrMode === MODE_RETRIG ? 'Retrig mode' : 'Mode off');
                needsRedraw = true;
                return;
            case PAD_LOCK_CLEAR:
                if (heldStep >= 0) clearStepLocks(heldStep);
                else announce('Hold a step first');
                return;
            case PAD_MODE_PROB:
                attrMode = attrMode === MODE_PROB ? MODE_NONE : MODE_PROB;
                announce(attrMode === MODE_PROB ? 'Probability mode' : 'Mode off');
                needsRedraw = true;
                return;
            case PAD_STAGE:
                /* SHIFT + stage pad opens the sample browser. Without a gesture
                 * the whole SRC machine set is unreachable on hardware, which
                 * is exactly how Mono shipped a preset browser that never
                 * listed a file. */
                if (sampleMode) closeSampleBrowser();
                else openSampleBrowser();
                return;
            case PAD_MONITOR:
                toggleMonitor();
                return;
            case PAD_LIVE_REC:
                liveRec = liveRec ? 0 : 1;
                host_module_set_param('live_rec', `${liveRec}`);
                announce(liveRec ? 'Live record armed' : 'Live record off');
                needsRedraw = true;
                return;
            default:
                break;
        }
    }

    switch (note) {
        case PAD_UNDO:
            host_module_set_param(shiftDown() ? 'redo' : 'undo', '1');
            fetchAll();
            announce(shiftDown() ? 'Redo' : 'Undo');
            return;
        case PAD_MEMO:
            memoAt = Date.now();
            return;
        case PAD_SONG:
            songOn = songOn ? 0 : 1;
            host_module_set_param('song_on', `${songOn}`);
            announce(songOn ? 'Song mode' : 'Pattern mode');
            needsRedraw = true;
            return;
        case PAD_PLAY:
            seqOn = seqOn ? 0 : 1;
            host_module_set_param('seq_on', `${seqOn}`);
            announce(seqOn ? 'Play' : 'Stop');
            needsRedraw = true;
            return;
        case PAD_FILL:
            fillAt = Date.now();
            fillLatched = !fillLatched;
            host_module_set_param('fill', fillLatched ? '1' : '0');
            announce(fillLatched ? 'Fill on' : 'Fill off');
            needsRedraw = true;
            return;
        case PAD_STAGE:
            focusStage = (focusStage + 1) % N_STAGES;
            fetchAll();
            /* Repaint at once rather than waiting for the periodic pass: the
             * palette under the pads just changed to a different family, and
             * for a moment the surface would otherwise offer machines this
             * stage refuses. */
            paintPalette(true);
            announceView(`${STAGE_LABEL[focusStage]}, ${machineName[focusStage]}`);
            return;
        case PAD_PPAGE:
            patPage = (patPage + 1) % Math.max(1, Math.ceil(seqLen / PAGE_STEPS));
            fetchSteps();
            announceView(`Page ${patPage + 1}`);
            needsRedraw = true;
            return;
        case PAD_EPAGE:
            editPage = (editPage + 1) % EDIT_COUNT;
            fetchAll();
            announceView(EDIT_NAME[editPage]);
            return;
        case PAD_COPY:
            copyPage();
            needsRedraw = true;
            return;
        case PAD_PASTE:
            pastePage();
            return;
        case PAD_CLEAR:
            if (presetMode) {
                if (presetIndex > 0) deletePreset(presets[presetIndex - 1]);
                else announce('Nothing selected');
                return;
            }
            clearAt = Date.now();
            return;
        default:
            return;
    }
}

function handlePadRelease(note) {
    if (note === PAD_MEMO) {
        const held = Date.now() - memoAt;
        host_module_set_param(held >= HOLD_MS ? 'memorize' : 'recall', '1');
        if (held < HOLD_MS) fetchAll();
        announce(held >= HOLD_MS ? 'Memorized' : 'Recalled');
        memoAt = 0;
        needsRedraw = true;
        return;
    }
    if (note === PAD_CLEAR) {
        const held = Date.now() - clearAt;
        if (held >= HOLD_MS) {
            host_module_set_param('seq_clear', '1');
            fetchSteps();
            announce('Pattern cleared');
        } else {
            clearPage();
        }
        clearAt = 0;
        needsRedraw = true;
        return;
    }
    if (note === PAD_FILL && Date.now() - fillAt >= HOLD_MS) {
        /* a long press was momentary — drop fill again on release */
        fillLatched = false;
        host_module_set_param('fill', '0');
        announce('Fill off');
        needsRedraw = true;
    }
}


/* Step buttons. Press latches the hold; release either commits the hold's
 * edits or, if nothing happened, toggles the trig. Notes, not CCs — see
 * STEP_FIRST. */
function handleStepNote(note, velocity) {
    /* SHIFT + a step selects that pattern from the bank. */
    if (shiftDown() && velocity > 0) {
        const p = note - STEP_FIRST;
        host_module_set_param('pattern', `${p}`);
        curPattern = p;
        fetchAll();
        announceView(`Pattern ${p + 1}`);
        return;
    }
    const idx = patPage * PAGE_STEPS + (note - STEP_FIRST);
    if (velocity > 0) {
        heldStep = idx;
        heldUsed = false;
        const st = steps[idx];
        announce(`Step ${idx + 1}${st.active ? '' : ' empty'}${st.nlocks ? `, ${st.nlocks} locks` : ''}`);
        needsRedraw = true;
    } else if (heldStep === idx) {
        if (!heldUsed) toggleStep(idx);
        heldStep = -1;
        heldUsed = false;
        needsRedraw = true;
    }
}

function onMidiMessageInternal(data) {
    /* Drop what is not ours before touching any state.
     *
     * This module ran without the house filter, and hardware showed why: a
     * sysex dump streaming through the MIDI_IN ring arrives as a burst of
     * bytes reinterpreted as three-byte messages — status 240, then 29, 58,
     * 178, then 247 — and once the ring misaligns the stream degrades to an
     * endless run of status=0 d1=0 d2=0. Thousands of them reached this
     * handler.
     *
     * shouldFilterMessage() drops clock, sysex, aftertouch and the
     * capacitive knob-touch notes. It does NOT catch the garbage BETWEEN the
     * sysex markers, so the validity check is separate: a real MIDI status
     * byte always has its high bit set, and anything without one is not a
     * message at all. */
    if (!data || data.length < 3) return;
    if ((data[0] & 0x80) === 0) return;
    if (shouldFilterMessage(data)) return;


    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xB0) {
        if (d1 === MoveShift) {
            shiftHeld = d2 >= 64;
            paintTransport(false);      /* row 4 swaps to the mode layer */
            return;
        }

        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta === 0) return;
            if (sampleMode) {
                if (!samples.length) return;
                let v = sampleIndex + (delta > 0 ? 1 : -1);
                if (v < 0) v = 0;
                if (v >= samples.length) v = samples.length - 1;
                if (v !== sampleIndex) {
                    sampleIndex = v;
                    const pth = samples[sampleIndex];
                    announce(pth.slice(pth.lastIndexOf('/') + 1));
                    needsRedraw = true;
                }
                return;
            }
            if (presetMode) {
                let v = presetIndex + (delta > 0 ? 1 : -1);
                if (v < 0) v = 0;
                if (v > presets.length) v = presets.length;
                if (v !== presetIndex) {
                    presetIndex = v;
                    announce(presetIndex === 0 ? 'Save new'
                                               : presets[presetIndex - 1].name);
                    needsRedraw = true;
                }
                return;
            }
            if (heldStep >= 0) {
                adjustHeldAttr(delta);
            } else {
                /* free jog sets pattern length */
                let v = seqLen + delta;
                if (v < 1) v = 1;
                if (v > MAX_STEPS) v = MAX_STEPS;
                if (v !== seqLen) {
                    seqLen = v;
                    host_module_set_param('seq_len', `${v}`);
                    announceParameter('Length', `${v}`);
                    needsRedraw = true;
                }
            }
            return;
        }

        if (d1 === MoveMainButton && d2 > 0) {
            /* Shift + jog click toggles the preset browser — the same gesture
             * the sibling modules use, so it is where a user will look. */
            if (shiftDown()) {
                if (presetMode) {
                    presetMode = false;
                    announceView(EDIT_NAME[editPage]);
                } else {
                    enterPresetMode();
                }
                needsRedraw = true;
                return;
            }
            if (sampleMode) {
                if (samples.length) loadSampleFile(samples[sampleIndex]);
                else announce('No WAV files found');
                return;
            }
            if (presetMode) {
                if (presetIndex === 0) savePreset();
                else loadPreset(presets[presetIndex - 1]);
                return;
            }
            editPage = (editPage + 1) % EDIT_COUNT;
            fetchAll();
            announceView(EDIT_NAME[editPage]);
            return;
        }

        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
            const delta = decodeDelta(d2);
            if (delta === 0) return;
            const knob = d1 - MoveKnob1;
            /* THE gesture: hold a step, turn a knob, that parameter is locked
             * on that step. Without a step held the knob edits the base value.
             * Shift escapes the gesture — it edits the base even while a step
             * is held, for when you are inspecting a step's locks and want to
             * move the underlying value instead. */
            if (heldStep >= 0 && !shiftDown()) {
                lockKnob(heldStep, knob, delta);
            } else {
                if (heldStep >= 0) heldUsed = true;   /* don't also toggle the trig */
                adjustKnob(knob, delta);
            }
            return;
        }
        return;
    }

    /* Steps come first: they share the note channel with the pads and sit
     * below them (16-31 against 68-99). */
    if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
        if (status === 0x90) { handleStepNote(d1, d2); return; }
        if (status === 0x80) { handleStepNote(d1, 0); return; }
        return;
    }
    if (status === 0x90 && d2 > 0) { handlePadPress(d1); return; }
    if (status === 0x80 || (status === 0x90 && d2 === 0)) { handlePadRelease(d1); return; }
}

/* ------------------------------------------------------------- lifecycle */

globalThis.init = function () {
    editPage = EDIT_FX;
    attrMode = MODE_NONE;
    focusStage = 0;
    patPage = 0;
    heldStep = -1;
    heldUsed = false;
    shiftHeld = false;
    fetchAll();
    paintAll(true);
    announceView('Overwork');
    needsRedraw = true;
    drawUI();
};

/* A suspended overtake session comes back with its LEDs cleared, and a single
 * paintAll is ~60 writes in one tick — the shim's queue drops some and pads
 * stay dark, which a diff-gated refresh never heals. Force several spaced
 * repaints instead (the Smack v0.9.1 fix). */
globalThis.onResume = function () {
    resumeRepaints = 3;
    fetchAll();
    paintAll(true);
    needsRedraw = true;
};

globalThis.tick = function () {
    tickCount++;
    settleShift();

    /* The playhead and the live effective values in ONE bulk round-trip every
     * other tick (~22/sec). A read costs a whole SPI frame, so the old
     * one-read-per-tick playhead poll was claiming every frame the param
     * channel had — which is what made other reads time out and return
     * nothing. Twenty-two updates a second is still smoother than the eye. */
    if (tickCount % 2 === 0) {
        const keys = ['seq_pos'];
        for (let s = 0; s < N_STAGES; s++) keys.push(effKey(s));
        const v = getParams(keys);
        const pos = parseInt(v.seq_pos, 10);
        if (Number.isFinite(pos) && pos !== seqPos) {
            seqPos = pos;
            paintSteps(false);
            needsRedraw = true;
        }
        for (let s = 0; s < N_STAGES; s++) {
            const ev = v[effKey(s)];
            if (ev) effVals[s] = ev.split(',').map((x) => parseInt(x, 10));
        }
    }

    if (tickCount % 24 === 0) pollFeedbackGuard();

    if (tickCount % 12 === 0) {
        fetchSteps();
        paintAll(false);
        needsRedraw = true;
    }

    if (resumeRepaints > 0 && tickCount % 8 === 0) {
        paintAll(true);
        resumeRepaints--;
    }

    if (needsRedraw) {
        if (sampleMode) drawSampleBrowser();
        else if (presetMode) drawPresets();
        else drawUI();
        needsRedraw = false;
    }
};

globalThis.onUnload = function () {
    /* The host clears LEDs and unloads the DSP; nothing to release here. */
};

/* Exposed for the harness: sample loading is pure data handling with no input
 * gesture attached yet, so this is the only way to drive it. */
/* Exposed so the harness can check this table against the engine's list. */
globalThis.__machineColorCount = () => MACHINE_COLOR.length;

globalThis.loadSampleFile = loadSampleFile;
globalThis.listSamples = listSamples;

globalThis.onMidiMessageInternal = onMidiMessageInternal;

export { onMidiMessageInternal };
