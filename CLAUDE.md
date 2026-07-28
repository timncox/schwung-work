---
status: active
last_touched: 2026-07-28
---

# Work

Schwung module for the Ableton Move: **twenty FX machines in two insert slots**,
inspired by the Elektron Tonverk's FX section. Two FX LFOs modulate any slot
parameter. Named Work because Tonverk is Swedish for "tone works" — the same
naming move as Mono for the Monomachine.

## Clean-room statement — read before touching the DSP

Every machine is written from the parameter descriptions in the **published
Tonverk user manual** (OS 1.3.3). This project contains **no Elektron code and
no Elektron factory content**. The goal is the described character, not a
bit-exact clone, and it is not represented as one anywhere in the docs, the
README or the module metadata.

The manual itself is Elektron's copyrighted document and is **not redistributed
here** — `docs/*.local.txt` is gitignored. `docs/REFERENCE.md` has the public
URL, the two commands to regenerate the local extract, and an explicit account
of what was and was not taken from it.

Practical consequences:

- Do not port, decompile, or transcribe anything from a Tonverk OS image.
- Do not ship Elektron preset or wavetable content.
- The module is called **Work**, not Tonverk. Keep Elektron's product name out
  of the module id, repo name and UI strings; describing the machines by their
  documented names inside the manual reference is fine.

## Architecture

Tonverk is an 8-encoder-per-page device and Move has 8 knobs, so parameter
pages map 1:1. A track's FX section is two insert slots in series, each holding
one machine with up to 8 parameters — that is exactly what this engine models.

```
src/work_core.{c,h}   the engine: 20 machines, 2 slots, 2 FX LFOs, sequencer
src/work_fx.c         audio_fx_api_v2 wrapper  -> work.so
src/work_overtake.c   plugin_api_v2 wrapper    -> dsp.so (reads audio-in)
src/ui_chain.js       Signal Chain slot editor UI
src/ui_overtake.js    full-surface UI
src/help_work.json    on-device Help, chain build
src/help_overwork.json  on-device Help, overtake build
test/host_sim.c       native host simulator      (make test)
test/dump_contract.c  emits the real param contract as JSON
test/ui_overtake.mjs  UI harness, mocked against that contract
```

Builds — **both compile from the same `work_core.c`; tag them together**:

- **`work`** — `audio_fx`, loads in Signal Chain slots **and** Master FX slots.
  The `.so` **must** be named `work.so`: the chain host loads a slot's audio FX
  as `modules/audio_fx/<id>/<id>.so` and never reads `module.json`'s `dsp`.
- **`overwork`** — `overtake`. Takes the whole surface and processes the
  hardware input. Its `.so` is a plain `dsp.so` (overtake modules *are* loaded
  via `module.json`). It must answer `get_param("module_id")` with `"overwork"`
  or schwung-manager decides no tool is loaded.

## The sequencer

Lives in the shared core, so the `audio_fx` build gets it too — a sequenced FX
in a chain slot is a real use, not an accident. It defaults **off** so the FX
build behaves as a plain static chain until something turns it on; the overtake
wrapper turns it on at create.

- 64 steps (four 16-step pages), per-step trig / condition / micro-timing / retrig.
- **19 lockable parameters**: 16 slot parameters, both machine selects, global mix.
  The lock index is part of the pattern format — **append only**.
- Resolution order per block is **base → locks → FX LFOs**, which is Elektron's:
  a lock sets the value, the LFO moves around whatever the lock set.
- **Lock semantics:** each firing trig is a complete snapshot — parameters it
  locks take their locked value, parameters it does not lock revert to base. A
  trig that does not fire changes nothing. This is documented in help.json
  because it is a choice, not an inevitability.
- Micro-timing resolves at one block (~2.9 ms), finer than 1/24 of a step at any
  sane tempo but **not sample-accurate**.
- Retrig restarts the FX LFOs and the filter envelope. It does **not** stutter
  audio — the Degrader's FREZ is the machine for that.

Worst-case `state` blob (64 steps, every lock set) is **7570 bytes** against the
device host's 16 KB read buffer. Re-measure if the format grows.

## Machine list

Order in `work_fx_t` is the preset format. **Append only** — inserting a machine
renumbers every saved preset.

| # | Machine | # | Machine |
|---|---|---|---|
| 0 | Bypass | 10 | Infinite Flanger |
| 1 | Chrono Pitch | 11 | Low-Pass Filter |
| 2 | Comb ± Filter | 12 | Multimode Filter |
| 3 | Compressor | 13 | Panoramic Chorus |
| 4 | Daisy Delay | 14 | Phase 98 |
| 5 | Degrader | 15 | Rumsklang Reverb |
| 6 | Dirtshaper | 16 | Saturator Delay |
| 7 | Filter Folder | 17 | Steel Box Reverb |
| 8 | Filterbank | 18 | Supervoid Reverb |
| 9 | Frequency Warper | 19 | Warble |

Knob labels live in `PARAM_NAME[][]` in `work_core.c` and are served to the UI
via `get_param("labels1"/"labels2")`. **The UI must never keep its own copy** —
a second table is a table that drifts.

## Conventions inherited from the sibling modules

These are all lessons other Schwung modules paid for. Do not relearn them.

- **`get_param` appends must be clamped.** `snprintf` returns the length it
  *would* have written, so `n += snprintf(...)` walks past the buffer and the
  next append writes out of bounds. Every append here goes through `nclamp()`.
  Smack v0.8.2 crashed on device from exactly this. `test_get_param_tiny_buffers`
  guards it with canaries.
- **A chain UI gets screen + knobs + jog + Back only.** Move firmware keeps the
  pad grid and the Capture button in a slot editor. Do not design pad
  interactions in `ui_chain.js` (Smack v0.12.2/0.12.3).
- **QuickJS module = strict mode.** An assigned-but-undeclared identifier
  throws on the first knob release and the host treats handler exceptions as
  fatal. Run the regex audit before shipping (Smack v0.8.6).
- **`os.readdir` returns `[names, errno]`** including `.` and `..`, and
  `os.rename`/`os.remove` return `-errno` rather than throwing. Not used here
  yet; it will matter when presets land. See the memory note.
- **Realtime path never allocates.** Every buffer comes from `work_create()`.
- **Never mock a host API from the code under test.** `test/ui_overtake.mjs`
  mocks `host_module_get_param` against `build/contract.json`, which
  `test/dump_contract.c` generates **from the engine**. A mock written from the
  UI's assumptions encodes the same misunderstanding twice and goes green while
  the feature is dead on hardware — how Mono shipped a preset browser that
  never listed a file. This paid for itself immediately: the first fixture run
  caught that `lock<N>_<P>` had a setter but **no getter**, so lock display and
  lock nudging were both silently reading the base value.

## Verification

```bash
make test        # engine simulator + UI harness — green before any release
make sanitize    # ASan + UBSan over the engine suite
make bench       # per-machine cost ranking (host machine, not the Move)
make arm         # Docker cross-compile + both tarballs (no hardware touched)
```

`make test` runs two suites. The engine simulator covers bypass transparency,
every machine bounded at min/default/max parameters, the global mix law, state
round-trip, short-buffer `get_param` canaries, compressor gain reduction,
machine-change state reset, FX LFO modulation, two-slot series routing, MIDI
clock/note handling, and the whole sequencer — lock apply/revert, every trig
condition, machine locks, micro-timing, a full 64-step round trip, and transport
restart. The UI harness covers the parameter-lock gesture, lock nudging, trig
toggling, the Shift escape, the machine palette, transport pads, jog behaviour,
copy/paste/clear, resume repaints, and that the UI never reads or writes a key
the engine does not handle.

**Nothing is hardware-verified.** `make bench` on a Mac puts the heaviest
machine (Rumsklang Reverb) at ~164x realtime and two of them at ~93x. Belt
benched 65x and was *extrapolated* to ~10-15% of an A53 core, so this looks
affordable — but that is an extrapolation from a different module, not a
measurement. Measure on device before claiming headroom.

## Roadmap

Phase 1 (**done**) — the 20 FX machines as an `audio_fx` build.
Phase 2 (**done**) — `overwork`: the overtake build. Step buttons as Tonverk's
[TRIG] keys, machine palette on the pads, hold-step + turn-knob parameter locks,
trig conditions, micro-timing, retrig, pattern pages, copy/paste.
Phase 3 — the SRC machines (Single/Multi Player, Subtracks, Grainer, Wavefinder,
Shape). Highest risk: sample memory and CPU on the A53 are unmeasured.

Phase 2 leftovers worth doing: song mode, per-step trig probability beyond the
three fixed percentages, and a browser editor (`web_ui.html`) — the manager
serves one for overtake tools via the Tool tab, and Tim's Move runs a
main-built manager that has it.

## Release

Not released yet. When it is, follow the store contract in
`reference_schwung_store_releases` (memory): the catalog pins no versions and
resolves `release.json` on `main`, so shipping means cutting the release asset
**and** bumping `release.json`. Rebuild with `make arm` *after* the version bump
— tarballs embed `module.json`'s version. Also update the module's badge on the
all-modules landing page in the `schwung` fork (`docs/index.html`).
