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

/* Stages, mirroring work_core.h: stage 0 is SRC, then the inserts. The engine
 * keys them "src" / "fx1" / "fx2" — note that fx1 is stage 1, not stage 0. */
const N_INSERTS     = 2;
const N_STAGES      = 1 + N_INSERTS;
const STAGE_SRC     = 0;
const STAGE_KEY     = ['src', 'fx1', 'fx2'];
const STAGE_LABEL   = ['SRC', 'FX 1', 'FX 2'];

const PAGE_MACHINES = 0;
const PAGE_STAGE1   = 1;          /* stage N's page is PAGE_STAGE1 + N */
/* The LFO pages follow the stage pages and need no named constant, because
 * nothing branches on them; PAGES and PAGE_NAME carry their layout. Both are
 * derived from N_STAGES so a stage cannot be added without its page. */
const PAGE_COUNT    = 1 + N_STAGES + 2;

const PAGE_NAME = ['MACHINES', ...STAGE_LABEL, 'LFO 1', 'LFO 2'];

/* Which stage a page edits, or -1 for the machine and LFO pages. */
function pageSlot(p) {
    const s = p - PAGE_STAGE1;
    return s >= 0 && s < N_STAGES ? s : -1;
}

const WAVE_NAME = ['Tri', 'Sine', 'Sqr', 'Saw', 'Ramp', 'Exp', 'Rand'];

/* Per-page knob descriptors: DSP key, fallback label, range and step. FX page
 * labels come from the DSP at runtime, so their `label` here is only used
 * before the first fetch completes. */
/* DEST reaches every stage parameter. It was frozen at 15 — two stages' worth —
 * which quietly made the third unmodulatable from this build. Derive it. */
const DEST_MAX = N_STAGES * 8 - 1;

const LFO_KNOBS = (n) => ([
    { key: `lfo${n}_dest`,  label: 'DEST',  min: -1, max: DEST_MAX, step: 1 },
    { key: `lfo${n}_spd`,   label: 'SPD',   min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_mult`,  label: 'MULT',  min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_wave`,  label: 'WAVE',  min: 0,  max: 6,   step: 1 },
    { key: `lfo${n}_depth`, label: 'DEP',   min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_phase`, label: 'SPH',   min: 0,  max: 127, step: 1 },
    { key: `lfo${n}_trig`,  label: 'TRIG',  min: 0,  max: 1,   step: 1 }
]);

const FX_KNOBS = (stage) => {
    const out = [];
    for (let i = 0; i < 8; i++) {
        out.push({ key: `${STAGE_KEY[stage]}_p${i + 1}`,
                   label: `${String.fromCharCode(65 + i)}`,
                   min: 0, max: 127, step: 1 });
    }
    return out;
};

/* `max` for the machine selects is a placeholder — the real bound is the length
 * of that stage's FAMILY, taken from the engine in applyMachineRange(). A
 * constant here is what left Granulator unreachable when the 21st machine
 * landed. The page is built from N_STAGES so a stage cannot be left off it. */
const MACHINE_PAGE = [];
for (let s = 0; s < N_STAGES; s++)
    MACHINE_PAGE.push({ key: STAGE_KEY[s], label: STAGE_LABEL[s],
                        min: 0, max: 0, step: 1 });
MACHINE_PAGE.push({ key: 'mix',   label: 'MIX', min: 0, max: 127, step: 1 });
/* Where this track sits in the mix, at the routing end of the chain. Global MIX
 * is the chain's dry/wet; LVL and PAN place the whole track. */
MACHINE_PAGE.push({ key: 'level', label: 'LVL', min: 0, max: 127, step: 1 });
MACHINE_PAGE.push({ key: 'pan',   label: 'PAN', min: 0, max: 127, step: 1 });

const PAGES = [MACHINE_PAGE];
for (let s = 0; s < N_STAGES; s++) PAGES.push(FX_KNOBS(s));
PAGES.push(LFO_KNOBS(1), LFO_KNOBS(2));

/* Is this key one of the machine selects? A chain of === comparisons is what
 * left slot 3 out of the label refresh when the slot was added. */
function isMachineKey(key) { return STAGE_KEY.indexOf(key) >= 0; }

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

/* Each stage selects from its own FAMILY, so the knob's range is that family's
 * length — not the full machine list. Sweeping raw machine codes would stall on
 * every code the stage refuses, which reads as a broken encoder. */
let familyCodes = [];           /* [stage] -> array of machine codes */

function applyMachineRange() {
    for (let s = 0; s < N_STAGES; s++) {
        const fam = familyCodes[s];
        PAGES[PAGE_MACHINES][s].max = fam && fam.length ? fam.length - 1 : 0;
    }
}

/* The knob mirrors a POSITION in the family; the engine speaks machine codes.
 * These two convert, and both fall back rather than throwing on a family that
 * has not arrived yet. */
function codeToPos(stage, code) {
    const fam = familyCodes[stage];
    if (!fam) return 0;
    const i = fam.indexOf(code);
    return i < 0 ? 0 : i;
}
function posToCode(stage, pos) {
    const fam = familyCodes[stage];
    if (!fam || !fam.length) return 0;
    return fam[Math.max(0, Math.min(fam.length - 1, pos))];
}

/* Resolve both slot machine names from the mirror. Local — costs no reads. */
function syncMachineNames() {
    for (let s = 0; s < N_STAGES; s++) {
        const code = values[STAGE_KEY[s]];
        if (code === undefined) machineName[s] = '--';
        else machineName[s] = machineList[code] || `#${code}`;
    }
}

/* Ask the engine which machines each stage accepts. Read once — the families
 * are compiled in — and never guessed at locally, because a JS copy of a table
 * the engine owns is a copy that drifts. */
function readFamilies() {
    const src = readRaw('src_codes');
    if (src === null) return false;
    const fx = readRaw('fx_codes');
    if (fx === null) return false;
    const s2n = (t) => t.split(',').map((x) => parseInt(x, 10)).filter(Number.isFinite);
    familyCodes = [s2n(src)];
    for (let i = 1; i < N_STAGES; i++) familyCodes.push(s2n(fx));
    applyMachineRange();
    return true;
}

function readLabels(s) {
    const lab = readRaw(`labels${s === STAGE_SRC ? '_src' : s}`);
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
            syncMachineNames();
            needsRedraw = true;
        }
        return;
    }
    if (familyCodes.length === 0) {
        if (readFamilies()) needsRedraw = true;
        return;
    }

    for (let s = 0; s < N_STAGES; s++) {
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

/* The machines page gives each STAGE a full-width row, because a machine name
 * is long and "Wide Chorus" cut to "Panora" is what this layout exists to
 * avoid. Everything after the stages is a short number, so those share one row
 * of cells along the bottom.
 *
 * Both are DERIVED from N_STAGES. A written-down [14, 27, 40] is what left the
 * MIX row printing at y=undefined the moment a third stage was added: the row
 * table still had three entries, the page had four knobs, and print() was
 * handed undefined — invisible in every test until a second overflowing row
 * turned up to collide with it. */
const ROW_TOP     = 14;
const ROW_STEP    = 13;
const ROW_LABEL_W = 24;
const ROW_VALUE_X = 28;
const CELL_Y      = ROW_TOP + N_STAGES * ROW_STEP - 5;

function drawMachinePage() {
    const knobs = PAGES[PAGE_MACHINES];
    for (let i = 0; i < N_STAGES && i < knobs.length; i++) {
        const y = ROW_TOP + i * ROW_STEP;
        print(0, y, fit(knobs[i].label, ROW_LABEL_W), 1);
        print(ROW_VALUE_X, y, fit(knobValue(i), SCREEN_W - ROW_VALUE_X), 1);
    }
    /* the scalars, side by side */
    const rest = knobs.length - N_STAGES;
    if (rest > 0) {
        const cell = Math.floor(SCREEN_W / rest);
        for (let i = 0; i < rest; i++) {
            const k = N_STAGES + i;
            print(i * cell, CELL_Y, fit(`${knobs[k].label} ${knobValue(k)}`, cell - 2), 1);
        }
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

    /* Machine selects move through the stage's FAMILY by position, then convert
     * back to a code on the way out. Stepping the raw code instead would walk
     * into machines the stage refuses, and the engine refuses rather than
     * substituting — so the encoder would appear to jam. */
    const machineStage = isMachineKey(k.key) ? STAGE_KEY.indexOf(k.key) : -1;
    if (machineStage >= 0) {
        const pos = codeToPos(machineStage, cur) + scaledMove(k, delta);
        const code = posToCode(machineStage, pos);
        if (code === cur) return;
        values[k.key] = code;
        host_module_set_param(k.key, `${code}`);
        labelsDirty[machineStage] = true;
        syncMachineNames();
        needsRedraw = true;
        return;
    }

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
    familyCodes = [];
    machineName = ['--', '--', '--'];
    fxLabels = [[], [], []];
    labelsDirty = [true, true, true];
    refreshCursor = 0;
    tickCount = 0;

    /* The machine list plus the three machine SELECTS — the values whose
     * absence would leave the first page reading "--" where a name belongs.
     * MIX, LVL and PAN fill in over the following ticks like every other
     * page's values do; each read here is a blocking ~23 ms round-trip, and
     * the first frame is what the player is waiting for. */
    refreshStep();
    for (let st = 0; st < N_STAGES; st++) readNum(STAGE_KEY[st]);
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
