/*
 * Work — Signal Chain UI
 *
 * Loads when Work's component editor is opened inside a slot's Signal Chain.
 *
 * Input reality for a slot editor, established on hardware during Smack
 * v0.12.2/v0.12.3: Move firmware keeps the pad grid and the Capture button,
 * so a chain UI receives ONLY the screen, the eight knobs, the jog wheel and
 * Back. Everything here is therefore knob- and jog-driven — do not add pad
 * interactions to this file.
 *
 * READING A PARAMETER IS EXPENSIVE HERE. In a chain editor schwung shims
 * host_module_get_param onto shadow_get_param (setupModuleParamShims in
 * shadow_ui.js), and that is a blocking shared-memory round-trip to the shim,
 * serviced once per SPI frame — schwung's own comment above js_shadow_get_param
 * calls it "the where-does-the-tick-time-go measurement". One read costs ~23 ms
 * of busy-wait and gives up after 100 ms.
 *
 * This file used to re-read all 36 of its keys on every machine-select detent.
 * That is ~0.8 s of stall per click (reported as "I can change fx but it's
 * super slow"), and the resulting contention made individual reads time out —
 * a timed-out read fell through to 0 and was written back on the next turn,
 * snapping the slot to Bypass. Two rules, both load-bearing:
 *
 *   1. The knob handler NEVER reads. It updates the local mirror and writes.
 *   2. A failed read leaves the mirror alone. Absent is not zero.
 *
 * Everything else is a budgeted background refresh of only the keys the
 * visible page actually draws.
 *
 * Controls:
 *   Jog turn            move between pages
 *   Jog click           jump back to the MACHINES page
 *   Shift + jog click   swap the module in this slot
 *   Knobs 1-8           the parameters of the current page
 *
 * Knob labels and the machine list are fetched from the DSP ("labels1",
 * "labels2", "machines") rather than duplicated here, so adding a machine in C
 * cannot leave this file stale.
 *
 * QuickJS loads this as an ES module, which means strict mode: every
 * identifier must be declared. An assigned-but-undeclared variable throws on
 * the first knob release and the host treats a handler exception as fatal
 * (the Smack v0.8.6 incident). Audit before shipping.
 */

import {
    MoveKnob1, MoveShift, MoveMainButton, MoveMainKnob
} from '/data/UserData/schwung/shared/constants.mjs';

import { decodeDelta, shouldFilterMessage }
    from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    announce, announceParameter, announceView
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ------------------------------------------------------------------ pages */

const N_SLOTS       = 3;
const PAGE_MACHINES = 0;
const PAGE_FX1      = 1;          /* slot N's page is PAGE_FX1 + N */
/* The LFO pages follow the FX pages and need no named constant, because
 * nothing branches on them; PAGES and PAGE_NAME carry their layout. Both are
 * derived from N_SLOTS so a slot cannot be added without its page. */
const PAGE_COUNT    = 1 + N_SLOTS + 2;

const PAGE_NAME = ['MACHINES', 'FX 1', 'FX 2', 'FX 3', 'LFO 1', 'LFO 2'];

/* Which slot a page edits, or -1 for the machine and LFO pages. */
function pageSlot(p) {
    const s = p - PAGE_FX1;
    return s >= 0 && s < N_SLOTS ? s : -1;
}

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
    /* `max` for the two machine selects is a placeholder — the real bound is
     * taken from the engine's own list in applyMachineRange(). A constant here
     * is what left Granulator unreachable when the 21st machine landed. */
    [ { key: 'machine1', label: 'FX 1', min: 0, max: 0,   step: 1 },
      { key: 'machine2', label: 'FX 2', min: 0, max: 0,   step: 1 },
      { key: 'machine3', label: 'FX 3', min: 0, max: 0,   step: 1 },
      { key: 'mix',      label: 'MIX',  min: 0, max: 127, step: 1 } ],
    FX_KNOBS(1),
    FX_KNOBS(2),
    FX_KNOBS(3),
    LFO_KNOBS(1),
    LFO_KNOBS(2)
];

/* Is this key one of the machine selects? A chain of === comparisons is what
 * left slot 3 out of the label refresh when the slot was added. */
function isMachineKey(key) { return /^machine[1-9]$/.test(key); }

/* ------------------------------------------------------------------- state */

let page        = PAGE_MACHINES;
let shiftHeld   = false;
let needsRedraw = true;
let tickCount   = 0;

let values      = {};           /* key -> number, mirrored from the DSP    */
let machineName = ['--', '--', '--'];
let fxLabels    = [[], [], []];
let machineList = [];
let labelsDirty = [true, true, true];

/* Background-refresh budget. BURST covers a full page plus both label strings
 * so a page or machine change fills in within a few hundred ms; the steady
 * trickle afterwards only exists so edits made from the web editor or a preset
 * load eventually show up here. */
const BURST_READS    = 20;
const REFRESH_PERIOD = 8;       /* ticks between trickle reads (~5.5/sec) */

let refreshCursor = 0;
let burst         = 0;

/* ------------------------------------------------------------- DSP access */

/* One blocking round-trip. Returns null for "no answer" — which is NOT the
 * same as "zero", and must never be collapsed into one. */
function readRaw(key) {
    const v = host_module_get_param(key);
    if (v === null || v === undefined) return null;
    const s = `${v}`;
    return s.length ? s : null;
}

/* Mirror one numeric key. Returns true if the mirrored value changed.
 * A failed or unparseable read is a no-op: falling back to 0 here is what let
 * a timed-out round-trip rewrite the slot to Bypass. */
function readNum(key) {
    const s = readRaw(key);
    if (s === null) return false;
    const n = parseInt(s, 10);
    if (!Number.isFinite(n)) return false;
    if (values[key] === n) return false;
    values[key] = n;
    return true;
}

/* The machine select's range is the engine's list length, never a constant. */
function applyMachineRange() {
    const max = machineList.length > 0 ? machineList.length - 1 : 0;
    for (let s = 0; s < N_SLOTS; s++) PAGES[PAGE_MACHINES][s].max = max;
}

/* Resolve both slot machine names from the mirror. Local — costs no reads. */
function syncMachineNames() {
    for (let s = 0; s < N_SLOTS; s++) {
        const code = values[`machine${s + 1}`];
        if (code === undefined) machineName[s] = '--';
        else machineName[s] = machineList[code] || `#${code}`;
    }
}

function readLabels(s) {
    const lab = readRaw(`labels${s + 1}`);
    if (lab === null) return false;
    fxLabels[s] = lab.split(',');
    labelsDirty[s] = false;
    return true;
}

/* One unit of background refresh, cheapest-useful-first: the machine list
 * (nothing can be named without it), then labels for a slot whose machine just
 * changed, then one numeric key of the VISIBLE page in round-robin. Keys on
 * pages that are not on screen are deliberately not read at all. */
function refreshStep() {
    if (machineList.length === 0) {
        const list = readRaw('machines');
        if (list) {
            machineList = list.split(',');
            applyMachineRange();
            syncMachineNames();
            needsRedraw = true;
        }
        return;
    }

    for (let s = 0; s < N_SLOTS; s++) {
        if (labelsDirty[s]) {
            if (readLabels(s)) needsRedraw = true;
            return;
        }
    }

    const knobs = PAGES[page];
    if (knobs.length === 0) return;
    const k = knobs[refreshCursor % knobs.length];
    refreshCursor++;
    if (readNum(k.key)) {
        if (isMachineKey(k.key)) syncMachineNames();
        needsRedraw = true;
    }
}

/* --------------------------------------------------------------- rendering */

const SCREEN_W = 128;

/* Truncate to a MEASURED pixel width. The Move font is proportional, so the
 * old fixed six-character slice both cut short names that would have fitted
 * and let wide ones run into the next column — the "text is scrunched"
 * report. text_width is local font metrics, not a round-trip; it is cheap. */
function fit(s, maxPx) {
    let t = `${s}`;
    if (text_width(t) <= maxPx) return t;
    while (t.length > 1 && text_width(t) > maxPx) t = t.slice(0, -1);
    return t;
}

/* Label for knob `i` on the current page. FX pages defer to the DSP's table
 * so the machine's real abbreviation (TUNE, FDBK, N.FRQ...) is shown. */
function knobLabel(i) {
    const k = PAGES[page][i];
    if (!k) return '';
    const slot = pageSlot(page);
    if (slot >= 0) {
        const t = fxLabels[slot][i];
        return t && t.length ? t : '';
    }
    return k.label;
}

/* Display string for knob `i`: machine names, waveform names and the LFO's
 * Off/destination read better than raw numbers. "--" means not yet mirrored,
 * which is honest — it is never shown as 0. */
function knobValue(i) {
    const k = PAGES[page][i];
    if (!k) return '';
    const v = values[k.key];
    if (v === undefined) return '--';

    if (isMachineKey(k.key)) return machineList[v] || `#${v}`;
    if (k.key.endsWith('_wave')) return WAVE_NAME[v % 7];
    if (k.key.endsWith('_trig')) return v ? 'Retrig' : 'Free';
    if (k.key.endsWith('_dest')) {
        if (v < 0) return 'Off';
        return `FX${(v >> 3) + 1}${String.fromCharCode(65 + (v & 7))}`;
    }
    return `${v}`;
}

function drawHeader(left, right) {
    print(0, 1, left, 1);
    if (right) {
        /* Right-aligned into whatever the title leaves, with a 4 px gap, so a
         * long machine name shortens instead of overprinting "FX 1". */
        const t = fit(right, SCREEN_W - text_width(left) - 4);
        print(SCREEN_W - text_width(t), 1, t, 1);
    }
    fill_rect(0, 9, SCREEN_W, 1, 1);
}

function drawFooter(hint) {
    fill_rect(0, 55, SCREEN_W, 1, 1);
    print(0, 57, fit(hint, SCREEN_W), 1);
}

/* The machines page carries three values, so it gets full-width rows and the
 * whole machine name — "Wide Chorus", not "Panora". */
const ROW_Y       = [14, 27, 40];
const ROW_LABEL_W = 24;
const ROW_VALUE_X = 28;

function drawMachinePage() {
    const knobs = PAGES[PAGE_MACHINES];
    for (let i = 0; i < knobs.length; i++) {
        print(0, ROW_Y[i], fit(knobs[i].label, ROW_LABEL_W), 1);
        print(ROW_VALUE_X, ROW_Y[i], fit(knobValue(i), SCREEN_W - ROW_VALUE_X), 1);
    }
}

/* FX and LFO pages keep the 4x2 grid because it maps to the physical knobs —
 * knob 5 sits below knob 1. Columns are 32 px with a 2 px gutter so adjacent
 * columns cannot touch. */
const COL_W  = 32;
const GUTTER = 2;

function drawGridPage() {
    const knobs = PAGES[page];
    for (let i = 0; i < knobs.length; i++) {
        const lab = knobLabel(i);
        if (!lab) continue;
        const x = (i % 4) * COL_W;
        const y = 13 + (i < 4 ? 0 : 1) * 20;
        const w = COL_W - GUTTER;
        print(x, y, fit(lab, w), 1);
        print(x, y + 9, fit(knobValue(i), w), 1);
    }
}

function drawUI() {
    clear_screen();

    if (page === PAGE_MACHINES) {
        drawHeader('MACHINES', `${page + 1}/${PAGE_COUNT}`);
        drawMachinePage();
        drawFooter('Jog: page');
        return;
    }

    let right = `${page + 1}/${PAGE_COUNT}`;
    const slot = pageSlot(page);
    if (slot >= 0) right = machineName[slot];
    drawHeader(PAGE_NAME[page], right);
    drawGridPage();
    drawFooter('Jog:page  Click:home');
}

/* ------------------------------------------------------------------ input */

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
    return (delta > 0 ? mag : -mag) * k.step;
}

function adjustKnob(i, delta) {
    const k = PAGES[page][i];
    if (!k) return;

    /* Never derive a written value from a base we have not actually read —
     * that is how a timed-out read used to become a real edit. Ask for a
     * refresh and ignore this detent instead. */
    const cur = values[k.key];
    if (cur === undefined) { burst = BURST_READS; return; }

    let v = cur + scaledMove(k, delta);
    if (v < k.min) v = k.min;
    if (v > k.max) v = k.max;
    if (v === cur) return;

    values[k.key] = v;
    host_module_set_param(k.key, `${v}`);

    /* Selecting a machine installs that machine's defaults in the DSP, so the
     * slot's eight parameters and its knob labels are now stale. Mark them for
     * the background refresh — reading them HERE is what made scrolling the
     * machine list unusable. */
    if (isMachineKey(k.key)) {
        const s = parseInt(k.key.slice(7), 10) - 1;
        labelsDirty[s] = true;
        for (const p of PAGES[PAGE_FX1 + s]) delete values[p.key];
        syncMachineNames();
        burst = BURST_READS;
    }

    announceParameter(knobLabel(i) || k.label, knobValue(i));
    needsRedraw = true;
}

function setPage(p) {
    if (p < 0) p = PAGE_COUNT - 1;
    if (p >= PAGE_COUNT) p = 0;
    if (p === page) return;
    page = p;
    refreshCursor = 0;
    burst = BURST_READS;
    let spoken = PAGE_NAME[page];
    const slot = pageSlot(page);
    if (slot >= 0) spoken = `FX ${slot + 1}, ${machineName[slot]}`;
    announceView(spoken);
    needsRedraw = true;
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
    machineList = [];
    machineName = ['--', '--', '--'];
    fxLabels = [[], [], []];
    labelsDirty = [true, true, true];
    refreshCursor = 0;
    tickCount = 0;

    /* Four reads at load — the machine list plus the three values the first
     * page draws. Everything else fills in over the following ticks and reads
     * "--" until it does. */
    refreshStep();
    for (const k of PAGES[PAGE_MACHINES]) readNum(k.key);
    syncMachineNames();

    burst = BURST_READS;
    announceView('Work');
    needsRedraw = true;
    drawUI();
};

globalThis.tick = function () {
    tickCount++;

    let budget = 0;
    if (burst > 0) {
        budget = burst >= 2 ? 2 : 1;
        burst -= budget;
    } else if (tickCount % REFRESH_PERIOD === 0) {
        budget = 1;
    }
    while (budget-- > 0) refreshStep();

    if (needsRedraw) {
        drawUI();
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = onMidiMessageInternal;

export { onMidiMessageInternal };
