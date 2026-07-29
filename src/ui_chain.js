/*
 * Work — Signal Chain UI
 *
 * Loads when Work's component editor is opened inside a slot's Signal Chain.
 * (The Master FX editor uses auto-generated knob pages from module.json's
 * ui_hierarchy instead.)
 *
 * Input reality for a slot editor, established on hardware during Smack
 * v0.12.2/v0.12.3: Move firmware keeps the pad grid and the Capture button,
 * so a chain UI receives ONLY the screen, the eight knobs, the jog wheel and
 * Back. Everything here is therefore knob- and jog-driven — do not add pad
 * interactions to this file.
 *
 * Controls:
 *   Jog turn            move between pages
 *   Jog click           jump back to the MACHINES page
 *   Shift + jog click   swap the module in this slot
 *   Knobs 1-8           the parameters of the current page
 *
 * Knob labels are fetched from the DSP ("labels1"/"labels2") rather than
 * duplicated here, so adding a machine in C cannot leave this file stale.
 *
 * QuickJS loads this as an ES module, which means strict mode: every
 * identifier must be declared. An assigned-but-undeclared variable throws on
 * the first knob release and the host treats a handler exception as fatal
 * (the Smack v0.8.6 incident). Audit before shipping.
 */

import {
    MoveKnob1, MoveShift, MoveMainButton, MoveMainKnob
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    announce, announceParameter, announceView
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ------------------------------------------------------------------ pages */

const PAGE_MACHINES = 0;
const PAGE_FX1      = 1;
const PAGE_FX2      = 2;
/* pages 3 and 4 are FX LFO 1 and 2 — they need no named constant because
 * nothing branches on them; PAGES and PAGE_NAME carry their layout */
const PAGE_COUNT    = 5;

const PAGE_NAME = ['MACHINES', 'FX 1', 'FX 2', 'LFO 1', 'LFO 2'];

const WAVE_NAME = ['Tri', 'Sine', 'Sqr', 'Saw', 'Ramp', 'Exp', 'Rand'];

/* Per-page knob descriptors: DSP key, fallback label, range and step. FX page
 * labels come from the DSP at runtime, so their `label` here is only used
 * before the first fetch completes. */
const LFO_KNOBS = (n) => ([
    { key: `lfo${n}_dest`,  label: 'DEST',  min: -1, max: 15,  step: 1 },
    { key: `lfo${n}_spd`,   label: 'SPD',   min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_mult`,  label: 'MULT',  min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_wave`,  label: 'WAVE',  min: 0,  max: 6,   step: 1 },
    { key: `lfo${n}_depth`, label: 'DEP',   min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_phase`, label: 'SPH',   min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_trig`,  label: 'TRIG',  min: 0,  max: 1,   step: 1 }
]);

const FX_KNOBS = (slot) => {
    const out = [];
    for (let i = 0; i < 8; i++) {
        out.push({ key: `fx${slot}_p${i + 1}`, label: `${String.fromCharCode(65 + i)}`,
                   min: 0, max: 127, step: 1 });
    }
    return out;
};

const PAGES = [
    [ { key: 'machine1', label: 'FX 1', min: 0, max: 20,  step: 1 },
      { key: 'machine2', label: 'FX 2', min: 0, max: 20,  step: 1 },
      { key: 'mix', label: 'MIX',  min: 0, max: 127, step: 1 } ],
    FX_KNOBS(1),
    FX_KNOBS(2),
    LFO_KNOBS(1),
    LFO_KNOBS(2)
];

/* ------------------------------------------------------------------- state */

let page        = PAGE_MACHINES;
let shiftHeld   = false;
let needsRedraw = true;
let tickCount   = 0;

let values      = {};      /* key -> number, mirrored from the DSP  */
let machineName = ['Bypass', 'Bypass'];
let fxLabels    = [[], []];
let machineList = [];

/* ------------------------------------------------------------- DSP access */

function getParam(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? '' : `${v}`;
}

function getNum(key) {
    const n = parseInt(getParam(key), 10);
    return Number.isFinite(n) ? n : 0;
}

function setNum(key, v) {
    values[key] = v;
    host_module_set_param(key, `${v}`);
}

/* Pull everything the screen shows. Called on init and periodically, so edits
 * made from the Master FX page or a preset load appear here too. */
function fetchAll() {
    const before = JSON.stringify(values) + machineName.join('|');

    for (let p = 0; p < PAGE_COUNT; p++) {
        for (const k of PAGES[p]) values[k.key] = getNum(k.key);
    }

    const list = getParam('machines');
    if (list) machineList = list.split(',');

    for (let s = 0; s < 2; s++) {
        const code = values[`machine${s + 1}`] | 0;
        machineName[s] = machineList[code] || `#${code}`;
        const lab = getParam(`labels${s + 1}`);
        fxLabels[s] = lab ? lab.split(',') : [];
    }

    if (JSON.stringify(values) + machineName.join('|') !== before) needsRedraw = true;
}

/* --------------------------------------------------------------- rendering */

/* Label for knob `i` on the current page. FX pages defer to the DSP's table
 * so the machine's real abbreviation (TUNE, FDBK, N.FRQ...) is shown. */
function knobLabel(i) {
    const k = PAGES[page][i];
    if (!k) return '';
    if (page === PAGE_FX1 || page === PAGE_FX2) {
        const t = fxLabels[page === PAGE_FX1 ? 0 : 1][i];
        return t && t.length ? t : '';
    }
    return k.label;
}

/* Display string for knob `i`: machine names, waveform names and the LFO's
 * Off/destination read better than raw numbers. */
function knobValue(i) {
    const k = PAGES[page][i];
    if (!k) return '';
    const v = values[k.key] | 0;

    if (k.key === 'machine1' || k.key === 'machine2') {
        const nm = machineList[v] || `#${v}`;
        return nm.length > 6 ? nm.slice(0, 6) : nm;
    }
    if (k.key.endsWith('_wave')) return WAVE_NAME[v % 7];
    if (k.key.endsWith('_trig')) return v ? 'Retrig' : 'Free';
    if (k.key.endsWith('_dest')) {
        if (v < 0) return 'Off';
        return `FX${(v >> 3) + 1}${String.fromCharCode(65 + (v & 7))}`;
    }
    return `${v}`;
}

function drawUI() {
    clear_screen();

    /* Header: page name left, the relevant machine right */
    print(0, 1, PAGE_NAME[page], 1);
    let right = '';
    if (page === PAGE_FX1) right = machineName[0];
    else if (page === PAGE_FX2) right = machineName[1];
    else if (page === PAGE_MACHINES) right = `${page + 1}/${PAGE_COUNT}`;
    else right = `${page + 1}/${PAGE_COUNT}`;
    if (right.length > 18) right = right.slice(0, 18);
    print(128 - text_width(right), 1, right, 1);
    fill_rect(0, 9, 128, 1, 1);

    /* Four columns, two rows — the eight knobs in hardware order */
    for (let i = 0; i < PAGES[page].length; i++) {
        const lab = knobLabel(i);
        if (!lab) continue;
        const col = i % 4;
        const row = i < 4 ? 0 : 1;
        const x   = col * 32;
        const y   = 13 + row * 20;
        print(x, y, lab.length > 6 ? lab.slice(0, 6) : lab, 1);
        print(x, y + 9, knobValue(i), 1);
    }

    fill_rect(0, 55, 128, 1, 1);
    const hint = page === PAGE_MACHINES ? 'Jog: page' : 'Jog: page  Click: home';
    print(0, 57, hint, 1);
}

/* ------------------------------------------------------------------ input */

function adjustKnob(i, delta) {
    const k = PAGES[page][i];
    if (!k) return;

    /* decodeDelta reports the ACCUMULATED movement, which for a quick turn is
     * easily 20+. On a short range like the 21-machine select that lands on an
     * end stop every time — the reported symptom was "FX 2 only does Warble or
     * Bypass". Short ranges therefore advance one step per event regardless of
     * how fast the knob moved; wide ranges keep the acceleration, which is
     * what makes 0-127 usable. */
    const span = k.max - k.min;
    const move = span <= 32 ? (delta > 0 ? 1 : -1) : delta * k.step;

    let v = (values[k.key] | 0) + move;
    if (v < k.min) v = k.min;
    if (v > k.max) v = k.max;
    if (v === values[k.key]) return;

    setNum(k.key, v);

    /* Selecting a machine reloads that slot's defaults in the DSP, so the
     * whole page has to be re-read rather than just this one knob. */
    if (k.key === 'machine1' || k.key === 'machine2') fetchAll();

    announceParameter(knobLabel(i) || k.label, knobValue(i));
    needsRedraw = true;
}

function setPage(p) {
    if (p < 0) p = PAGE_COUNT - 1;
    if (p >= PAGE_COUNT) p = 0;
    if (p === page) return;
    page = p;
    fetchAll();
    let spoken = PAGE_NAME[page];
    if (page === PAGE_FX1) spoken = `FX 1, ${machineName[0]}`;
    else if (page === PAGE_FX2) spoken = `FX 2, ${machineName[1]}`;
    announceView(spoken);
    needsRedraw = true;
}

function onMidiMessageInternal(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status !== 0xB0) return;

    if (d1 === MoveShift) {
        shiftHeld = d2 >= 64;
        return;
    }

    /* Jog click: home. Shift + jog click: swap the module in this slot —
     * host_swap_module unloads this UI, so touch nothing after calling it. */
    if (d1 === MoveMainButton && d2 > 0) {
        if (shiftHeld) {
            if (typeof host_swap_module === 'function') {
                announce('Module chooser');
                host_swap_module();
            } else {
                announce('Swap needs a newer schwung');
            }
            return;
        }
        setPage(PAGE_MACHINES);
        return;
    }

    if (d1 === MoveMainKnob) {
        const delta = decodeDelta(d2);
        if (delta !== 0) setPage(page + (delta > 0 ? 1 : -1));
        return;
    }

    if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
        const delta = decodeDelta(d2);
        if (delta !== 0) adjustKnob(d1 - MoveKnob1, delta);
        return;
    }
}

/* ------------------------------------------------------------- lifecycle */

globalThis.init = function () {
    page = PAGE_MACHINES;
    shiftHeld = false;
    values = {};
    fetchAll();
    announceView('Work');
    needsRedraw = true;
    drawUI();
};

globalThis.tick = function () {
    tickCount++;
    /* Re-read roughly twice a second so edits from the web editor or a
     * preset load show up here without a page change. */
    if (tickCount % 12 === 0) fetchAll();
    if (needsRedraw) {
        drawUI();
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = onMidiMessageInternal;

export { onMidiMessageInternal };
