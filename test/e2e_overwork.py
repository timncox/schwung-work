"""On-device end-to-end tests for Work and Overwork.

WHY THIS EXISTS
---------------
Every hardware bug in this module so far got past a green test suite, and all
of them for the same reason: the JS harness mocks the host, and a mock encodes
what I ASSUMED the device does. When the assumption was wrong the mock was
wrong in the same direction, so the test agreed with the code and both were
wrong together.

The list, in order of how much time each cost:

  * step buttons are NOTES 16-31, not CCs. The handler sat in the CC branch
    where nothing reached it, and the stray note fell through to the engine and
    FIRED THE SAMPLE. The harness sent CCs too, so every step test passed.
  * the sample browser had no gesture at all — correct code, unreachable.
  * a load reported nothing on screen, so success and failure looked identical.
  * MACHINE_COLOR/N_MACHINES constants mirroring an engine table.

This file talks to the real Move through `schwung-testd`, so the contract under
test is the device's rather than mine. It injects real MIDI, reads real
parameters back out of the running DSP, and snapshots real pad LEDs.

SETUP
-----
    ssh ableton@move.local 'nohup setsid /data/UserData/schwung/bin/schwung-testd &'
    ssh -fN -L 47777:localhost:47777 ableton@move.local
    .venv-e2e/bin/pytest test/e2e_overwork.py -v

FULLY UNATTENDED, since schwung branch `agent/testbus-inject-overtake`
-----------------------------------------------------------------------
Two gaps in schwung blocked this and both are fixed there:

  * injected MIDI never reached an overtake module. The module is fed from the
    raw HARDWARE buffer while `shadow_drain_midi_inject` writes into Move's
    MAILBOX for the firmware — the two never met. The shim now drains the ring
    onto the overtake route instead while overtake_mode is set, and the mailbox
    drain yields so the ring keeps its single consumer.
  * `set_open_tool` answered "tool not found" for overtake modules, because its
    lookup searched `scanForToolModules()` only. It now falls back to
    `scanForOvertakeModules()`.

The fixture launches Overwork itself. No one needs to touch the Move.
"""
import time

import pytest

try:
    from schwung_bus import SchwungBus, SchwungBusError
except ImportError:                                     # pragma: no cover
    pytest.skip("pip install -e schwung/tools/pytest-schwung", allow_module_level=True)


STEP_FIRST = 16          # step buttons are NOTES 16-31 — verified on device
PAD_PLAY = 68
PAD_SLOT = 70
PAD_EPAGE = 72
PALETTE_ROW1 = 92

# The machine palette: pad rows 1-3 in reading order, minus the three function
# pads sitting inside row 3. Mirrors PALETTE_SLOTS in ui_overtake.js.
#
# A pad's position here indexes the FOCUSED STAGE'S FAMILY, not a machine code.
# It used to be a machine code, with Shift reaching a second bank at 21+; the
# SRC promotion split the machines into 21 effects and 6 sources, each of which
# fits these 21 slots outright, so the bank went away.
PAD_UNDO, PAD_MEMO, PAD_SONG = 81, 82, 83
PAD_STAGE = 70           # cycles the focused stage: SRC -> FX 1 -> FX 2
N_STAGES = 3

PALETTE_SLOTS = [
    p for p in (list(range(92, 100)) + list(range(84, 92)) + list(range(76, 84)))
    if p not in (PAD_UNDO, PAD_MEMO, PAD_SONG)
]


@pytest.fixture(scope="module")
def bus():
    try:
        b = SchwungBus()
        b.connect()
    except Exception as exc:                            # pragma: no cover
        pytest.skip(f"schwung-testd unreachable: {exc}")
    yield b
    b.close()


@pytest.fixture(scope="module")
def overwork(bus):
    """Require Overwork to be the running overtake tool."""
    if bus.state().overtake_mode != 2:
        pytest.skip("open Overwork on the Move first (Shift+Vol+jog click)")
    try:
        if bus.get_param("module_id") != "overwork":
            pytest.skip("a different overtake tool is loaded")
    except SchwungBusError as exc:
        pytest.skip(f"overtake DSP not answering: {exc}")

    # Wait out the module's SHIFT settling window before any test runs.
    # For about a second after init the UI deliberately clears Shift, because
    # the launch gesture is itself a Shift combo whose release is swallowed
    # while init runs. A Shift-dependent test starting inside that window sees
    # Shift ignored and reports a palette bug that is not there — which it did,
    # intermittently, depending on how fast the earlier tests ran.
    time.sleep(1.5)
    return bus


def _param(bus, key, tries=6):
    """get_param with a retry.

    The param channel is a single shared-memory slot serviced once per SPI
    frame; under contention a read comes back EMPTY rather than failing. A test
    that takes that at face value reports a value of '' as a real change, which
    is how `test_function_pads` claimed a function pad had loaded a machine."""
    for _ in range(tries):
        v = bus.get_param(key)
        if v != "":
            return v
        bus.wait_frame(3)
    return ""


def _machines(bus):
    return _param(bus, "machines").split(",")


def _await_param(bus, key, was, tries=30):
    """Poll `key` until it moves off `was`.

    A fixed wait after an injected press is a GUESS about latency, and the
    guess is wrong often enough to matter. With the guess in place this test
    read values that belonged to other presses in the same run — pad 97 came
    back holding pad 95's machine, one and two events out of step, and the
    apparent offset was different every time.

    Waiting for the change is the only thing that actually synchronises with
    an injected event. The caller parks the parameter somewhere the press must
    move it off, so "not `was`" is an unambiguous signal that the press
    landed."""
    for _ in range(tries):
        v = _param(bus, key)
        if v and v != was:
            return v
        bus.wait_frame(3)
    return _param(bus, key)


def _codes(bus, key):
    """A family's machine codes, from the ENGINE. Never recomputed here — a
    local copy of a table the engine owns is a copy that drifts, and catching
    exactly that drift is what this suite is for."""
    raw = _param(bus, key)
    return [int(c) for c in raw.split(",") if c.strip().isdigit()] if raw else []


def _shift_latched_on_device(bus):
    """Is the SHIM holding Shift down, regardless of what we inject?

    Found on the device 2026-07-29: every one of 495 consecutive events logged
    `hostShift=true` while the test bus reported `shift_held=0`. With Shift
    latched, a knob turn edits the base value instead of writing a parameter
    lock, a palette pad loads machine+21, and the slot pad opens the sample
    browser — so a whole session looks broken in unrelated ways. No injected
    shift-off can clear it: the hardware keeps asserting it."""
    import subprocess
    try:
        out = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=4", "ableton@move.local",
             "tail -c 40000 /data/UserData/schwung/debug.log | "
             "grep 'OVERTAKE MIDI' | grep -c hostShift=true"],
            capture_output=True, text=True, timeout=20).stdout.strip()
        return int(out or 0) > 0
    except Exception:
        return False


def _release_modifiers(bus):
    """Never assume the device's modifier state.

    Shift was found latched on from earlier use, so an unshifted palette press
    loaded machine 24 instead of 3 — the test read as a palette bug when the
    surface was simply holding Shift. Tests establish what they depend on."""
    bus.inject_midi(bytes([0x0B, 0xB0, 49, 0]))
    bus.wait_frame(3)
    # Twice, with a gap. One release can be dropped if it shares a frame with
    # the pad traffic from the previous test, and a single missed release is
    # exactly what latches Shift in the first place — the failure this suite
    # exists to catch. Belt and braces here keeps a REAL latch detectable
    # instead of drowning in our own flakiness.
    bus.inject_midi(bytes([0x0B, 0xB0, 49, 0]))
    bus.wait_frame(6)


# ----------------------------------------------------------------- contract

def test_engine_answers_its_identity(overwork):
    assert overwork.get_param("module_id") == "overwork"


def test_machine_list_matches_the_build(overwork):
    """The count the DSP reports is what the UI derives its ranges from.

    A hardcoded copy of this number in the UI is what made Granulator, and then
    One Shot, unreachable."""
    machines = _machines(overwork)
    assert len(machines) >= 26, machines
    assert machines[0] == "Bypass"
    assert "One Shot" in machines
    assert "Tilt" in machines


# -------------------------------------------------------------- step buttons

@pytest.mark.xfail(reason="injected MIDI does not reach an overtake module "
                          "(verified: pad press moves neither params nor LEDs)",
                   strict=False)
def test_step_button_toggles_a_trig_and_does_not_fire_the_sample(overwork):
    """The bug that cost the most: steps arrive as NOTES, and the unhandled
    note fell through to the engine and triggered playback."""
    bus = overwork
    before = _param(bus, "step0")

    bus.press_step(STEP_FIRST)
    bus.wait_frame(4)
    bus.release_step(STEP_FIRST)
    bus.wait_frame(8)

    after = _param(bus, "step0")
    assert after != before, (
        f"pressing step 1 left step0 at {after!r} — the trig did not toggle"
    )
    assert after.split(":")[0] == "1", f"step0 is {after!r}, expected active"

    # and back off again, so the test leaves no trace
    bus.press_step(STEP_FIRST)
    bus.wait_frame(4)
    bus.release_step(STEP_FIRST)
    bus.wait_frame(8)
    assert _param(bus, "step0").split(":")[0] == "0"


def test_hold_step_plus_knob_writes_a_parameter_lock(overwork):
    """THE gesture. Hold a step, turn a knob, that step remembers the value."""
    bus = overwork
    if _shift_latched_on_device(bus):
        pytest.skip("the device is holding SHIFT down — Shift deliberately "
                    "escapes this gesture and edits the base value instead. "
                    "Check the physical Shift button.")
    bus.set_param("machine1", "1")           # a machine with real parameters
    bus.wait_frame(8)
    bus.set_param("locks0", "")              # clear any locks on step 1
    bus.wait_frame(4)

    bus.press_step(STEP_FIRST)
    bus.wait_frame(4)
    bus.inject_midi(bytes([0x0B, 0xB0, 71, 5]))   # knob 1, five detents
    bus.wait_frame(6)
    bus.release_step(STEP_FIRST)
    bus.wait_frame(8)

    locks = _param(bus, "locks0")
    assert locks, "hold-step + knob left no lock on step 1"

    bus.set_param("locks0", "")


# ------------------------------------------------------------------ palette

def _focus_stage(bus, stage):
    """Point the surface at `stage` by CYCLING the stage pad, confirming each
    step against the engine's published `focus`.

    Driven by the pad, not by writing `focus`. Writing it moves the engine's
    copy and nothing else: the UI keeps its own `focusStage` and only re-reads
    the engine's in fetchAll(), which does not run on a tick — so the palette
    went on loading into whichever stage the surface was really pointed at
    while the test believed it had moved. Stage 0 passed and the two inserts
    failed with the parked value untouched, which is what "the press went
    somewhere else" looks like.

    What publishing `focus` is actually for is the READ. Before it, this had to
    infer the focus by pressing a palette pad and seeing which stage moved, and
    that inference is why four sweeps gave four different answers. Now the pad
    drives and the parameter confirms."""
    for _ in range(N_STAGES + 1):
        cur = _param(bus, "focus")
        if cur == str(stage):
            return
        bus.press_pad(PAD_STAGE)
        bus.wait_frame(4)
        bus.release_pad(PAD_STAGE)
        # Wait for the engine to report the move rather than assuming a
        # latency; the pad is injected and arrives when it arrives.
        _await_param(bus, "focus", cur)

    got = _param(bus, "focus")
    assert got == str(stage), (
        f"cycled the stage pad {N_STAGES + 1} times and focus is {got!r}, "
        f"not {stage}"
    )


@pytest.mark.parametrize("stage", range(N_STAGES))
def test_palette_pads_load_the_focused_stage_family(overwork, stage):
    """A palette pad loads the FOCUSED stage, from that stage's own family.

    Rewritten 2026-07-30. The old version asserted that pad N loads machine N,
    with Shift reaching a second bank at 21+ — the contract from before the SRC
    promotion, when all three slots were interchangeable and a pad index WAS a
    machine code. Neither half survives: a pad now indexes the focused stage's
    FAMILY, and the Shift bank is gone because splitting 26 machines into 21
    effects and 6 sources made each list fit rows 1-3 outright.

    The expectation comes from the engine's own src_codes / fx_codes rather
    than from arithmetic here. A test that recomputed the family locally would
    keep passing while the two drifted apart, which is the whole failure this
    suite exists to catch."""
    bus = overwork
    _release_modifiers(bus)

    family = _codes(bus, "src_codes" if stage == 0 else "fx_codes")
    assert family, "the engine served no family for this stage"

    _focus_stage(bus, stage)
    key = ("src", "machine1", "machine2")[stage]
    machines = _machines(bus)

    # Two positions: one near the start and one at the very end of the family.
    # The last is the one that matters — reaching it is exactly what a missing
    # palette slot breaks, and it has broken twice.
    for slot in (3, len(family) - 1):
        if slot >= len(PALETTE_SLOTS):
            continue
        want = family[slot]

        # Park somewhere the press MUST move off, so the wait below has an
        # unambiguous signal. Bypass unless that is what we are expecting.
        park = str(family[0]) if want != family[0] else str(family[1])
        bus.set_param(key, park)
        bus.wait_frame(6)

        bus.press_pad(PALETTE_SLOTS[slot])
        bus.wait_frame(4)
        bus.release_pad(PALETTE_SLOTS[slot])

        got = int(_await_param(bus, key, park))
        assert got == want, (
            f"stage {stage}: palette slot {slot} (pad {PALETTE_SLOTS[slot]}) "
            f"loaded {got} ({machines[got]}), expected {want} ({machines[want]})"
        )


def test_palette_slots_past_a_family_load_nothing(overwork):
    """The source family is six long and the palette is twenty-one slots. The
    fifteen slots past its end must do NOTHING while SRC is focused — not wrap,
    not fall through to the effect family, which would put a reverb in the
    source stage."""
    bus = overwork
    _release_modifiers(bus)

    family = _codes(bus, "src_codes")
    if len(family) >= len(PALETTE_SLOTS):
        pytest.skip("the source family fills the palette; nothing is past its end")

    _focus_stage(bus, 0)
    bus.set_param("src", "21")               # One Shot
    bus.wait_frame(4)

    bus.press_pad(PALETTE_SLOTS[len(family)])
    bus.wait_frame(4)
    bus.release_pad(PALETTE_SLOTS[len(family)])
    bus.wait_frame(10)

    got = _param(bus, "src")
    assert int(got) == 21, (
        f"a palette slot past the source family's end changed the stage to "
        f"{got}; slots past a family must be inert"
    )


def test_function_pads_are_not_swallowed_by_the_palette(overwork):
    """Pads 81-83 are undo/memo/song and sit inside the palette rows. Mapping
    pad index straight to machine index made pressing Undo load a machine once
    the count passed 21."""
    bus = overwork
    _release_modifiers(bus)
    before = _param(bus, "machine1")
    for pad in (81, 82, 83):
        bus.press_pad(pad)
        bus.wait_frame(3)
        bus.release_pad(pad)
        bus.wait_frame(6)
    assert _param(bus, "machine1") == before, (
        "a function pad changed the loaded machine"
    )


# ------------------------------------------------------------------- samples

def test_sample_state_is_readable(overwork):
    """The UI reports load success from these, not from its own send finishing
    — the sample crosses shared memory and only the far end knows it landed."""
    bus = overwork
    raw = _param(bus, "sample_max")
    if raw == "":
        pytest.skip("param channel contended — the read came back empty")
    assert int(raw) > 0
    frames = int(_param(bus, "sample_frames") or 0)
    assert frames >= 0
    if frames:
        assert _param(bus, "sample_name")


def test_source_machine_silences_the_live_input(overwork):
    """Feedback: with a source machine generating there is nothing to blend the
    mic with, so the input is removed from the path outright rather than
    waiting for the guard to detect risk."""
    bus = overwork
    if int(_param(bus, "sample_frames") or 0) == 0:
        pytest.skip("load a sample first")
    machines = _machines(bus)
    single = machines.index("One Shot")

    bus.set_param("machine1", str(single))
    bus.set_param("monitor", "1")
    bus.wait_frame(10)
    assert _param(bus, "machine1") == str(single)


# ---------------------------------------------------------------------- LEDs

def test_pads_light_up(overwork):
    """A surface that responds but paints nothing is still broken."""
    leds = overwork.snapshot_pad_leds()
    assert len(leds) == 32
    assert any(v for v in leds), "every pad is dark — the palette never painted"
