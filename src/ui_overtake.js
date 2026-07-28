/*
 * Overwork — full-surface UI.
 *
 * Move's sixteen dedicated STEP buttons stand in for Tonverk's [TRIG] keys,
 * the eight knobs for its eight encoders, and the jog for LEVEL/DATA. That is
 * a real 1:1 map, not an approximation — which is why the Elektron gesture
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
 *   Pads row 1-3    machine palette, 21 machines. Tap loads one into the
 *   (76-99)           focused slot.
 *
 *   Pad row 4       68 play/stop   69 fill     70 slot focus  71 pattern page
 *   (68-75)         72 edit page   73 copy     74 paste       75 clear
 *                   SHIFT + 71/72/73/74/75 = probability / condition / micro /
 *                   retrig mode, and clear-locks-on-the-held-step.
 *                   SHIFT + 68 arms LIVE RECORD: knob moves then write locks
 *                   onto whichever step is playing.
 *
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

import { decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    announce, announceParameter, announceView
} from '/data/UserData/schwung/shared/screen_reader.mjs';

import * as os from 'os';

/* Presets live OUTSIDE the module directory, so they survive a reinstall.
 * That also means "I reinstalled and my patterns are still there" is expected,
 * not evidence the reinstall failed. */
const PRESET_DIR = '/data/UserData/schwung/presets/overwork';

/* ------------------------------------------------------------- constants */

const STEP_FIRST = 16;          /* step buttons are CC 16-31 */
const STEP_COUNT = 16;
const PAGE_STEPS = 16;
const MAX_STEPS  = 64;
const N_MACHINES = 21;

/* Machine palette occupies pad rows 1-3 in reading order: 92-99, 84-91, 76-79 */
const PALETTE_PADS = [
    92, 93, 94, 95, 96, 97, 98, 99,
    84, 85, 86, 87, 88, 89, 90, 91,
    76, 77, 78, 79, 80, 81, 82, 83
];

/* The step-attribute modes moved to SHIFT + row 4 in v0.2.0, when a 21st
 * machine (Grainer) needed the whole of row 3 for the palette. */
const PAD_MODE_COND   = 72;   /* shift + edit-page  */
const PAD_MODE_MICRO  = 73;   /* shift + copy       */
const PAD_MODE_RETRIG = 74;   /* shift + paste      */
const PAD_LOCK_CLEAR  = 75;   /* shift + clear      */
const PAD_MODE_PROB   = 71;   /* shift + pattern page */
const PAD_LIVE_REC    = 68;   /* shift + play         */

/* Row 4: transport and navigation */
const PAD_PLAY  = 68;
const PAD_FILL  = 69;
const PAD_SLOT  = 70;
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
const EDIT_GLOBAL = 5;
const EDIT_COUNT  = 6;
const EDIT_NAME   = ['FX', 'LFO 1', 'LFO 2', 'LFO 3', 'MOD ENV', 'GLOBAL'];

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
const MACHINE_COLOR = [
    DarkGrey,      /* 0  Bypass            */
    Purple,        /* 1  Chrono Pitch      pitch family */
    Green,         /* 2  Comb +/- Filter   filter family */
    Blue,          /* 3  Compressor        dynamics */
    BurntOrange,   /* 4  Daisy Delay       delay family */
    Lime,          /* 5  Degrader          destruction */
    Lime,          /* 6  Dirtshaper        destruction */
    Green,         /* 7  Filter Folder     filter */
    Green,         /* 8  Filterbank        filter */
    Purple,        /* 9  Frequency Warper  pitch */
    Cyan,          /* 10 Infinite Flanger  modulation */
    Green,         /* 11 Low-Pass Filter   filter */
    Green,         /* 12 Multimode Filter  filter */
    Cyan,          /* 13 Panoramic Chorus  modulation */
    Cyan,          /* 14 Phase 98          modulation */
    SkyBlue,       /* 15 Rumsklang Reverb  space */
    BurntOrange,   /* 16 Saturator Delay   delay */
    SkyBlue,       /* 17 Steel Box Reverb  space */
    SkyBlue,       /* 18 Supervoid Reverb  space */
    TealGreen,     /* 19 Warble            tape */
    Rose           /* 20 Grainer           granular */
];

/* ----------------------------------------------------------------- state */

let editPage   = EDIT_FX;
let attrMode   = MODE_NONE;
let focusSlot  = 0;          /* which FX slot the palette and knobs address */
let patPage    = 0;          /* 0-3, the visible sixteen of up to 64 steps  */
let shiftHeld  = false;
let needsRedraw = true;
let tickCount  = 0;
let resumeRepaints = 0;

let presetMode  = false;
let presetIndex = 0;         /* 0 = "Save new", 1..n = a stored preset      */
let presets     = [];

let heldStep   = -1;         /* absolute step index held down, or -1        */
let heldUsed   = false;      /* a lock or attribute edit happened this hold */
let clearAt    = 0;          /* PAD_CLEAR press time, for the hold gesture  */
let fillAt     = 0;
let fillLatched = false;
let liveRec     = 0;
let copyBuf    = null;

/* mirrored DSP state */
let machineName = ['Bypass', 'Bypass'];
let machineList = [];
let condList    = [];
let fxLabels    = [[], []];
let effVals     = [[], []];
let cfg         = {};        /* key -> value for everything the knobs edit  */
let steps       = [];        /* per step: {active, cond, micro, retrig, nlocks} */
let seqPos      = 0;
let seqLen      = 16;
let seqOn       = 1;

for (let i = 0; i < MAX_STEPS; i++) {
    steps.push({ active: 0, cond: 0, micro: 0, retrig: 0, nlocks: 0, prob: 100 });
}

/* ------------------------------------------------------------ DSP access */

function getParam(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? '' : `${v}`;
}

function getNum(key) {
    const n = parseInt(getParam(key), 10);
    return Number.isFinite(n) ? n : 0;
}

function setNum(key, v) {
    cfg[key] = v;
    host_module_set_param(key, `${v}`);
}

/* The knob descriptors for the current edit page. FX labels come from the DSP
 * so they always match whichever machine the focused slot holds. */
function pageKnobs() {
    if (editPage === EDIT_FX) {
        const out = [];
        for (let i = 0; i < 8; i++) {
            const lab = fxLabels[focusSlot][i];
            out.push({
                key: `fx${focusSlot + 1}_p${i + 1}`,
                label: lab && lab.length ? lab : '',
                lock: focusSlot * 8 + i,
                min: 0, max: 127
            });
        }
        return out;
    }
    if (editPage === EDIT_MENV) {
        return [
            { key: 'menv_dest',  label: 'DEST',  lock: -1, min: -1, max: 15 },
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
            { key: `lfo${n}_dest`,  label: 'DEST',  lock: -1, min: -1, max: 15 },
            { key: `lfo${n}_spd`,   label: 'SPD',   lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_mult`,  label: 'MULT',  lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_wave`,  label: 'WAVE',  lock: -1, min: 0, max: 6 },
            { key: `lfo${n}_depth`, label: 'DEP',   lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_phase`, label: 'SPH',   lock: -1, min: 0, max: 127 },
            { key: `lfo${n}_trig`,  label: 'TRIG',  lock: -1, min: 0, max: 1 },
            { key: '', label: '', lock: -1, min: 0, max: 0 }
        ];
    }
    /* EDIT_GLOBAL */
    return [
        { key: 'fx1',     label: 'FX1',  lock: 16, min: 0, max: N_MACHINES - 1 },
        { key: 'fx2',     label: 'FX2',  lock: 17, min: 0, max: N_MACHINES - 1 },
        { key: 'mix',     label: 'MIX',  lock: 18, min: 0, max: 127 },
        { key: 'seq_len', label: 'LEN',  lock: -1, min: 1, max: MAX_STEPS },
        { key: '', label: '', lock: -1, min: 0, max: 0 },
        { key: '', label: '', lock: -1, min: 0, max: 0 },
        { key: '', label: '', lock: -1, min: 0, max: 0 },
        { key: '', label: '', lock: -1, min: 0, max: 0 }
    ];
}

/* Everything the screen and LEDs show, pulled in one pass. */
function fetchAll() {
    const list = getParam('machines');
    if (list) machineList = list.split(',');
    const conds = getParam('conds');
    if (conds) condList = conds.split(',');

    for (let s = 0; s < 2; s++) {
        const code = getNum(`fx${s + 1}`);
        cfg[`fx${s + 1}`] = code;
        machineName[s] = machineList[code] || `#${code}`;
        const lab = getParam(`labels${s + 1}`);
        fxLabels[s] = lab ? lab.split(',') : [];
        const ev = getParam(`eff${s + 1}`);
        effVals[s] = ev ? ev.split(',').map((x) => parseInt(x, 10)) : [];
        for (let i = 0; i < 8; i++) cfg[`fx${s + 1}_p${i + 1}`] = getNum(`fx${s + 1}_p${i + 1}`);
    }

    for (const k of pageKnobs()) if (k.key) cfg[k.key] = getNum(k.key);
    cfg.mix = getNum('mix');

    seqLen = getNum('seq_len') || 16;
    seqOn  = getNum('seq_on');
    fillLatched = getNum('fill') !== 0;
    liveRec = getNum('live_rec');

    fetchSteps();
    needsRedraw = true;
}

/* Step metadata for the visible page only — sixteen reads, not sixty-four. */
function fetchSteps() {
    const base = patPage * PAGE_STEPS;
    for (let i = 0; i < PAGE_STEPS; i++) {
        const idx = base + i;
        if (idx >= MAX_STEPS) break;
        const raw = getParam(`step${idx}`);
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

/* --------------------------------------------------------------- editing */

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

    let v = base + delta;
    if (v < k.min) v = k.min;
    if (v > k.max) v = k.max;
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

    let v = (cfg[k.key] | 0) + delta;
    if (v < k.min) v = k.min;
    if (v > k.max) v = k.max;
    if (v === cfg[k.key]) return;

    setNum(k.key, v);
    if (k.key === 'fx1' || k.key === 'fx2') fetchAll();
    if (k.key === 'seq_len') seqLen = v;

    announceParameter(k.label, knobText(knob));
    needsRedraw = true;
}

function loadMachine(code) {
    setNum(`fx${focusSlot + 1}`, code);
    fetchAll();
    announce(`${machineList[code] || code} in FX ${focusSlot + 1}`);
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
    announce(`Loaded ${entry.name}`);
    needsRedraw = true;
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

    if (k.key === 'fx1' || k.key === 'fx2') {
        const nm = machineList[v] || `#${v}`;
        return nm.length > 6 ? nm.slice(0, 6) : nm;
    }
    if (k.key.endsWith('_wave')) return ['Tri', 'Sin', 'Sqr', 'Saw', 'Ramp', 'Exp', 'Rnd'][v % 7];
    if (k.key.endsWith('_trig')) return v ? 'Rtrg' : 'Free';
    if (k.key.endsWith('_dest')) {
        if (v < 0) return 'Off';
        return `FX${(v >> 3) + 1}${String.fromCharCode(65 + (v & 7))}`;
    }
    return `${v}`;
}

function drawUI() {
    clear_screen();

    /* Header: edit page + focused machine, then transport on the right */
    let title = EDIT_NAME[editPage];
    if (editPage === EDIT_FX) title = `FX${focusSlot + 1} ${machineName[focusSlot]}`;
    if (title.length > 20) title = title.slice(0, 20);
    print(0, 1, title, 1);

    const right = `${seqOn ? '>' : '||'}${seqPos + 1}/${seqLen}`;
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
        } else if (editPage === EDIT_FX && effVals[focusSlot].length === 8) {
            /* show the value actually reaching the DSP when it differs */
            const e = effVals[focusSlot][i];
            if (Number.isFinite(e) && e !== (cfg[knobs[i].key] | 0)) val = `${e}~`;
        }
        print(x, y + 9, val, 1);
    }

    fill_rect(0, 55, 128, 1, 1);
    let foot;
    if (heldStep >= 0) {
        const st = steps[heldStep];
        foot = `S${heldStep + 1} ${condList[st.cond] || ''} u${st.micro} L${st.nlocks} ${st.prob}%`;
    } else {
        foot = `Pg${patPage + 1} ${MODE_NAME[attrMode]}${fillLatched ? ' FILL' : ''}${liveRec ? ' REC' : ''}`;
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
        const code = i;
        let color = Black;
        if (code < N_MACHINES) {
            color = MACHINE_COLOR[code];
            /* the machine loaded in the focused slot burns brighter */
            if (code === (cfg[`fx${focusSlot + 1}`] | 0)) color = White;
        }
        setLED(PALETTE_PADS[i], color, force);
    }
}

function paintTransport(force) {
    setLED(PAD_PLAY,  liveRec ? Red : (seqOn ? BrightGreen : DarkGrey), force);
    setLED(PAD_FILL,  fillLatched ? BrightRed : 0x0C, force);
    setLED(PAD_SLOT,  focusSlot === 0 ? SkyBlue : YellowGreen, force);
    setLED(PAD_PPAGE, [Blue, Cyan, Purple, OrangeRed][patPage % 4], force);
    if (shiftHeld) {
        /* while shift is held row 4 IS the step-attribute mode row */
        setLED(PAD_MODE_COND,   attrMode === MODE_COND   ? Cyan   : 0x0A, force);
        setLED(PAD_MODE_MICRO,  attrMode === MODE_MICRO  ? Purple : 0x0A, force);
        setLED(PAD_MODE_RETRIG, attrMode === MODE_RETRIG ? Blue   : 0x0A, force);
        setLED(PAD_LOCK_CLEAR,  heldStep >= 0 ? OrangeRed : 0x08, force);
        setLED(PAD_MODE_PROB,   attrMode === MODE_PROB ? YellowGreen : 0x0A, force);
        setLED(PAD_LIVE_REC,    liveRec ? Red : 0x0A, force);
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
    paintTransport(force);
}

/* ----------------------------------------------------------------- input */

function handlePadPress(note) {
    /* While the preset browser is open only CLEAR (delete) is live; the rest
     * of the surface would otherwise edit a pattern you cannot see. */
    if (presetMode && note !== PAD_CLEAR) {
        announce('Preset browser open');
        return;
    }

    /* Machine palette */
    const pi = PALETTE_PADS.indexOf(note);
    if (pi >= 0) {
        if (pi < N_MACHINES) loadMachine(pi);
        return;
    }

    /* SHIFT + row 4 selects the step-attribute mode the jog edits. */
    if (shiftHeld) {
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
        case PAD_SLOT:
            focusSlot = focusSlot ? 0 : 1;
            fetchAll();
            announceView(`FX ${focusSlot + 1}, ${machineName[focusSlot]}`);
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

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === 0xB0) {
        if (d1 === MoveShift) {
            shiftHeld = d2 >= 64;
            paintTransport(false);      /* row 4 swaps to the mode layer */
            return;
        }

        /* Step buttons arrive as CC. Press latches the hold; release either
         * commits the hold's edits or, if nothing happened, toggles the trig. */
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            const idx = patPage * PAGE_STEPS + (d1 - STEP_FIRST);
            if (d2 >= 64) {
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
            return;
        }

        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta === 0) return;
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
            if (shiftHeld) {
                if (presetMode) {
                    presetMode = false;
                    announceView(EDIT_NAME[editPage]);
                } else {
                    enterPresetMode();
                }
                needsRedraw = true;
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
            if (heldStep >= 0 && !shiftHeld) {
                lockKnob(heldStep, knob, delta);
            } else {
                if (heldStep >= 0) heldUsed = true;   /* don't also toggle the trig */
                adjustKnob(knob, delta);
            }
            return;
        }
        return;
    }

    if (status === 0x90 && d2 > 0) { handlePadPress(d1); return; }
    if (status === 0x80 || (status === 0x90 && d2 === 0)) { handlePadRelease(d1); return; }
}

/* ------------------------------------------------------------- lifecycle */

globalThis.init = function () {
    editPage = EDIT_FX;
    attrMode = MODE_NONE;
    focusSlot = 0;
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

    /* One cheap poll per tick keeps the playhead moving without hammering
     * the DSP with a dozen get_params. */
    const pos = getNum('seq_pos');
    if (pos !== seqPos) {
        seqPos = pos;
        paintSteps(false);
        needsRedraw = true;
    }

    if (tickCount % 12 === 0) {
        fetchSteps();
        for (let s = 0; s < 2; s++) {
            const ev = getParam(`eff${s + 1}`);
            effVals[s] = ev ? ev.split(',').map((x) => parseInt(x, 10)) : [];
        }
        paintAll(false);
        needsRedraw = true;
    }

    if (resumeRepaints > 0 && tickCount % 8 === 0) {
        paintAll(true);
        resumeRepaints--;
    }

    if (needsRedraw) {
        if (presetMode) drawPresets();
        else drawUI();
        needsRedraw = false;
    }
};

globalThis.onUnload = function () {
    /* The host clears LEDs and unloads the DSP; nothing to release here. */
};

globalThis.onMidiMessageInternal = onMidiMessageInternal;

export { onMidiMessageInternal };
