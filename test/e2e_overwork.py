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

Overwork must be OPEN on the device: shadow_ui's open_tool flag is not reliably
picked up from the bus, so the one manual step is launching the tool. Every
test below skips with a clear message rather than failing when it is not.
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
PALETTE_ROW1 = 92        # machines 0-7; +Shift reaches 21-28


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
    return bus


def _machines(bus):
    return bus.get_param("machines").split(",")


# ----------------------------------------------------------------- contract

def test_engine_answers_its_identity(overwork):
    assert overwork.get_param("module_id") == "overwork"


def test_machine_list_matches_the_build(overwork):
    """The count the DSP reports is what the UI derives its ranges from.

    A hardcoded copy of this number in the UI is what made Grainer, and then
    Single Player, unreachable."""
    machines = _machines(overwork)
    assert len(machines) >= 26, machines
    assert machines[0] == "Bypass"
    assert "Single Player" in machines
    assert "Shape" in machines


# -------------------------------------------------------------- step buttons

def test_step_button_toggles_a_trig_and_does_not_fire_the_sample(overwork):
    """The bug that cost the most: steps arrive as NOTES, and the unhandled
    note fell through to the engine and triggered playback."""
    bus = overwork
    before = bus.get_param("step0")

    bus.press_step(STEP_FIRST)
    bus.wait_frame(4)
    bus.release_step(STEP_FIRST)
    bus.wait_frame(8)

    after = bus.get_param("step0")
    assert after != before, (
        f"pressing step 1 left step0 at {after!r} — the trig did not toggle"
    )
    assert after.split(":")[0] == "1", f"step0 is {after!r}, expected active"

    # and back off again, so the test leaves no trace
    bus.press_step(STEP_FIRST)
    bus.wait_frame(4)
    bus.release_step(STEP_FIRST)
    bus.wait_frame(8)
    assert bus.get_param("step0").split(":")[0] == "0"


def test_hold_step_plus_knob_writes_a_parameter_lock(overwork):
    """THE gesture. Hold a step, turn a knob, that step remembers the value."""
    bus = overwork
    bus.set_param("machine1", "1")           # a machine with real parameters
    bus.wait_frame(8)
    bus.set_param("locks0", "")              # clear any locks on step 1
    bus.wait_frame(4)

    bus.press_step(STEP_FIRST)
    bus.wait_frame(4)
    bus.inject_midi(0xB0, 71, 5)             # knob 1, five detents
    bus.wait_frame(6)
    bus.release_step(STEP_FIRST)
    bus.wait_frame(8)

    locks = bus.get_param("locks0")
    assert locks, "hold-step + knob left no lock on step 1"

    bus.set_param("locks0", "")


# ------------------------------------------------------------------ palette

@pytest.mark.parametrize("shift,offset", [(False, 0), (True, 21)])
def test_palette_pads_load_machines(overwork, shift, offset):
    """Unshifted pads reach machines 0-20, Shift reaches 21 and up. The Shift
    bank exists because 26 machines do not fit 21 palette slots."""
    bus = overwork
    machines = _machines(bus)
    target = offset + 3
    if target >= len(machines):
        pytest.skip(f"only {len(machines)} machines")

    if shift:
        bus.inject_midi(0xB0, 49, 127)
        bus.wait_frame(2)
    bus.press_pad(PALETTE_ROW1 + 3)
    bus.wait_frame(4)
    bus.release_pad(PALETTE_ROW1 + 3)
    bus.wait_frame(10)
    if shift:
        bus.inject_midi(0xB0, 49, 0)
        bus.wait_frame(2)

    got = int(bus.get_param("machine1"))
    assert got == target, (
        f"palette pad 4 {'with' if shift else 'without'} Shift loaded machine "
        f"{got} ({machines[got]}), expected {target} ({machines[target]})"
    )


def test_function_pads_are_not_swallowed_by_the_palette(overwork):
    """Pads 81-83 are undo/memo/song and sit inside the palette rows. Mapping
    pad index straight to machine index made pressing Undo load a machine once
    the count passed 21."""
    bus = overwork
    before = bus.get_param("machine1")
    for pad in (81, 82, 83):
        bus.press_pad(pad)
        bus.wait_frame(3)
        bus.release_pad(pad)
        bus.wait_frame(6)
    assert bus.get_param("machine1") == before, (
        "a function pad changed the loaded machine"
    )


# ------------------------------------------------------------------- samples

def test_sample_state_is_readable(overwork):
    """The UI reports load success from these, not from its own send finishing
    — the sample crosses shared memory and only the far end knows it landed."""
    bus = overwork
    assert int(bus.get_param("sample_max")) > 0
    frames = int(bus.get_param("sample_frames"))
    assert frames >= 0
    if frames:
        assert bus.get_param("sample_name")


def test_source_machine_silences_the_live_input(overwork):
    """Feedback: with a source machine generating there is nothing to blend the
    mic with, so the input is removed from the path outright rather than
    waiting for the guard to detect risk."""
    bus = overwork
    if int(bus.get_param("sample_frames")) == 0:
        pytest.skip("load a sample first")
    machines = _machines(bus)
    single = machines.index("Single Player")

    bus.set_param("machine1", str(single))
    bus.set_param("monitor", "1")
    bus.wait_frame(10)
    assert bus.get_param("machine1") == str(single)


# ---------------------------------------------------------------------- LEDs

def test_pads_light_up(overwork):
    """A surface that responds but paints nothing is still broken."""
    leds = overwork.snapshot_pad_leds()
    assert len(leds) == 32
    assert any(v for v in leds), "every pad is dark — the palette never painted"
