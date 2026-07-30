---
status: active
last_touched: 2026-07-29
---

# Work

Schwung module for the Ableton Move: **twenty-six machines in a source stage
and two insert FX** — twenty-one effects and six sources — inspired by the
Elektron Tonverk's per-track shape. Three FX LFOs and a modulation envelope
modulate any stage parameter.

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
- **Every machine name here is ours** (2026-07-29). The machines used to carry
  Elektron's documented names — Rumsklang, Supervoid, Dirtshaper, Grainer,
  Single/Multi Player, Subtracks, Wavefinder, Shape, Phase 98, Warble — and
  they were all renamed. Generic audio terms (Compressor, Filterbank,
  Multimode Filter, Low-Pass Filter) stayed, because nobody owns those. Do not
  reintroduce the old names, in code, comments, tests or docs.
- Keep "Elektron" and "Tonverk" out of everything a user sees: the manual page,
  README, module.json and release.json descriptions, on-device help, and UI
  strings. `mono` is the calibration — its docs page names no one. The
  provenance belongs here and in `docs/REFERENCE.md`, nowhere else.

## Architecture

The reference device is 8 encoders per page and Move has 8 knobs, so parameter
pages map 1:1. Three stages in series — a SOURCE stage then two INSERT FX —
each holding one machine with up to 8 parameters. That is the reference
device's per-track shape, and it is what this engine models.

A stage only accepts machines from its own family, and refuses the rest rather
than substituting something near enough:

- **sources** (6): Bypass, Granulator, One Shot, Polysample, Slicer, Wavescan
- **effects** (21): Bypass and everything else, including Tilt

The families are NOT complements. Granulator is a source because that is where
a player looks for it, but it still reads its input when no sample is loaded,
which is what makes live granulation work from the source stage. Bypass is in
both, because every stage needs a way to be empty.

**21 effects against 21 free palette pads is exact.** Adding an effect without
freeing a pad puts it out of reach of the Move's surface entirely; that has
happened twice. `test_every_machine_is_reachable_from_the_palette` in
`test/ui_overtake.mjs` is the guard.

Growing this further means growing TRACKS, not stages — see DESIGN-8TRACK.md.

```
src/work_core.{c,h}   the engine: 26 machines, 3 stages, 3 FX LFOs, sequencer
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

Builds — **all three compile from the same `work_core.c`; tag them together**:

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
- **36 lockable parameters, per track**: 24 stage parameters (contiguous, so an
  index is `stage * 8 + knob`), the three machine selects, track level and pan,
  and the seven voice-filter fields. The index is part of the pattern format —
  **append only**, and it was rebuilt exactly once, in v0.9.0, because the
  eight-lane format broke anyway. Add new lockables at 36.
- The **global dry/wet is not lockable**. It is not per-track, and "track 5
  step 3 changes the global mix" is the cross-track surprise that makes a
  pattern unpredictable; track LEVEL replaces it. A v1 mix lock is dropped on
  load and `get_param("load_note")` says so.
- A machine lock goes through the same family gate as any other machine write,
  so a lock cannot put a reverb in the source stage. It also fires the locked
  machine's voice — the trigger reads the EFFECTIVE machine and parameters, so
  a firing trig is a complete snapshot of the voice as well as the knobs.
- **Preset versions:** the blob is `"v":3`. v3 prefixes every per-track key
  with `t<N>` and packs each lane to base64; v2 and v1 have no prefix and write
  the pattern as `"stp"` text. v1 additionally covers two different lock maps
  (before and after the SRC promotion) and nothing in the lock data separates
  them — the flat-mirror key names do, because they were renumbered in the same
  change. `fp3` dates a blob to before it, `fp_src` to after.
- Resolution order per block is **base → locks → FX LFOs**, which is Elektron's:
  a lock sets the value, the LFO moves around whatever the lock set.
- **Lock semantics:** each firing trig is a complete snapshot — parameters it
  locks take their locked value, parameters it does not lock revert to base. A
  trig that does not fire changes nothing. This is documented in help.json
  because it is a choice, not an inevitability.
- Micro-timing resolves at one block (~2.9 ms), finer than 1/24 of a step at any
  sane tempo but **not sample-accurate**.
- Retrig restarts the FX LFOs and the filter envelope. It does **not** stutter
  audio — the Decimator's FREZ is the machine for that.

## The preset blob

`get_param("state")` is served through a 16,384-byte buffer in the host
(`js_host_module_get_param`, schwung's `src/schwung_host.c`). Over that a
preset does not fail, it **TRUNCATES** — it stays valid JSON, loses the later
tracks, and says nothing. That is what kept `WORK_TRACKS` at 1 until v0.9.0:
the v2 text format put a single maximally dense lane at 14,817 bytes, 90% of
the buffer, and eight of those is about 114 KB.

Three things fixed it, and none of them may be undone casually:

- **Lanes are packed to binary and base64'd** (`lane_pack` / `lane_unpack`). A
  step record is 12 bytes plus one per lock. Fields stay a byte each rather
  than bit-packing into the obvious spare room, because every one is bounded by
  a `WORK_*` constant that has already grown once. The lock mask is read back
  by population count over all 40 bits, so a blob from a future engine with
  more lockables parses instead of desynchronising.
- **Per-track keys are prefixed `t<N>`.** `apply_state` walks the blob with
  `strstr` rather than parsing JSON, so a nested `m1` would be found by
  whichever track came first. A track that is all Bypass with an empty lane is
  omitted entirely — it can make no sound whatever else is set — and comes back
  at `track_defaults()`.
- **The blob is served in WINDOWS**: `state_len` for the total, `state` and
  `state@<offset>` for the bytes. Ask for the length first rather than reading
  until a short answer; "short" means "shorter than the host's buffer", and
  that constant lives in schwung, not here.

Measured 2026-07-29, at eight tracks:

| pattern | blob | host reads |
|---|---|---|
| empty | 515 B | 1 |
| 64 trigs on every lane | 10,418 B | 1 |
| …plus 4 locks per step | 13,170 B | 1 |
| …plus all 36 locks per step | 34,994 B | 3 |

So the realistic case still costs one round trip. `test_one_dense_track_fits_a_single_read`
guards that, because a chain slot holds one track and paying for paging there
would be a cliff nobody asked for;
`test_every_track_survives_the_window_protocol` guards the rest.

## Machine list

Order in `work_fx_t` is the preset format. **Append only** — inserting a machine
renumbers every saved preset.

| # | Machine | # | Machine |
|---|---|---|---|
| 0 | Bypass | 10 | Endless Flanger |
| 1 | Clock Pitch | 11 | Low-Pass Filter |
| 2 | Comb ± Filter | 12 | Multimode Filter |
| 3 | Compressor | 13 | Wide Chorus |
| 4 | Chain Delay | 14 | Phase Array |
| 5 | Decimator | 15 | Roomtone Reverb |
| 6 | Gritshaper | 16 | Drive Delay |
| 7 | Fold Filter | 17 | Iron Room Reverb |
| 8 | Filterbank | 18 | Voidspace Reverb |
| 9 | Spectrum Bender | 19 | Flutter |
| | | 20 | Granulator |
| | | 21 | One Shot |
| | | 22 | Polysample |
| | | 23 | Slicer |
| | | 24 | Wavescan |
| | | 25 | Tilt |

Knob labels live in `PARAM_NAME[][]` in `work_core.c` and are served to the UI
via `get_param("labels_src"/"labels1"/"labels2")` — the source stage spells its
suffix `_src` and the inserts use their number, and every stage-addressed key
follows that convention (`eff_src`/`eff1`, `src_p1`/`fx1_p1`, `fp_src`/`fp1`).
**The UI must never keep its own copy** — a second table is a table that drifts.
The same goes for the FAMILIES: the engine serves them at `src_codes` and
`fx_codes`, and both UIs and the manual site read them rather than listing
membership locally.

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
  `os.rename`/`os.remove` return `-errno` rather than throwing. The preset
  browser in `ui_overtake.js` depends on both; the harness asserts the naive
  reading FAILS against its mock, so the mock cannot go soft.
- **Realtime path never allocates.** Every buffer comes from `work_create()`.
- **Never mock a host API from the code under test.** `test/ui_overtake.mjs`
  mocks `host_module_get_param` against `build/contract.json`, which
  `test/dump_contract.c` generates **from the engine**. A mock written from the
  UI's assumptions encodes the same misunderstanding twice and goes green while
  the feature is dead on hardware — how Mono shipped a preset browser that
  never listed a file. This paid for itself immediately: the first fixture run
  caught that `lock<N>_<P>` had a setter but **no getter**, so lock display and
  lock nudging were both silently reading the base value.

## MIDI

**Track N listens on channel N.** Channel 1 drives track 1, channel 8 drives
track 8, and a channel above the track count reaches nothing at all. Decided
2026-07-29 over addressing tracks through NRPN: a channel per track is what
every hardware sequencer already speaks, so the whole CC map works per track
unchanged instead of needing a second scheme beside it.

The spare channels are ignored rather than folded onto the selected track, on
purpose. A fallback would make one message mean different things depending on
where the UI was pointed, which is the kind of surprise that makes a rig
unreproducible — the same reason micro-timing had to become per-lane.

The CC map, per channel: 8–15 insert 1 A–H, 16–23 insert 2 A–H, 24/25 insert
machine selects, 26 dry/wet, 27 track level, 28 track pan, 32/40/48 LFOs,
56–60 mod envelope, 64/65/66 sequencer/fill/record, 80–87 source A–H, 88 source
machine. The source stage sits at 80 because 27–31 is not eight controls wide,
and because 8–26 was published meaning the inserts and the dry/wet — which is
still exactly what it means. Nothing anyone already mapped moved.

Only EXTERNAL and FX-broadcast CCs are acted on; Move's own encoders arrive as
INTERNAL and would fight the UI. Identical messages inside ~2 blocks are
dropped, because a channel-matched chain slot can deliver one CC twice.

## Feedback

Overwork reads the hardware input, so speakers plus a live mic is a loop. Two
things stop it, and they are different in kind:

- **The guard** (`pollFeedbackGuard` in `ui_overtake.js`) watches
  `host_speaker_active() && !host_line_in_connected()` and mutes `monitor`,
  which zeroes the input in the engine. It has to DETECT the risk, and it
  cannot see Move's own firmware monitoring, which is out of our reach.
- **A source machine in slot 1 removes the input path entirely**
  (`machine_is_source` in `work_core.c`). If the chain generates, the mic has
  nothing to blend with and is pure liability, so it is zeroed regardless of
  monitor state. This does not depend on detecting anything.

Granulator is deliberately NOT a source for this purpose: with no sample loaded it
granulates the live input, which is what it shipped with.

If feedback persists with a source machine loaded and "INPUT MUTED" showing,
the loop is Move's own input monitoring rather than ours — schwung's docs put
the firmware's autosample and line-in monitoring explicitly out of scope.

## On-device tests (test/e2e_overwork.py)

Every hardware bug in this module got past a green suite, and all for the same
reason: the JS harness mocks the host, and a mock encodes what I ASSUMED the
device does. When the assumption was wrong the mock was wrong the same way, so
the test agreed with the code and both were wrong together. Step buttons are the
canonical case — they are NOTES 16-31, the handler sat in the CC branch where
nothing reached it, and the harness sent CCs too.

`test/e2e_overwork.py` talks to the real Move through schwung's `schwung-testd`,
so the contract under test is the device's. It injects real MIDI, reads
parameters out of the running DSP, and snapshots real pad LEDs.

```sh
ssh ableton@move.local 'nohup setsid /data/UserData/schwung/bin/schwung-testd &'
ssh -fN -L 47777:localhost:47777 ableton@move.local
.venv-e2e/bin/pytest test/e2e_overwork.py -v
```

The venv exists because pytest-schwung needs Python >= 3.10 and the system
python is 3.9:

```sh
$(brew --prefix python@3.12)/bin/python3.12 -m venv .venv-e2e
.venv-e2e/bin/pip install -e ../schwung/tools/pytest-schwung pytest
```

**One manual step remains.** Overwork must already be open on the device.
`bus.set_open_tool('overwork')` writes the command and shadow_ui reads it, but
answers `tool not found: overwork` — its open_tool path searches
`scanForToolModules()` (component_type `tool`) and overtake modules come from
`scanForOvertakeModules()` instead. Teaching schwung's open_tool to fall back to
the overtake list would make the suite fully unattended; until then every test
skips with a clear message rather than failing.

## The browser editor (not built yet)

**Follow `smack`.** It has the identical three-build shape to Work — audio_fx +
sound_generator + overtake — and already ships one. Read `smack/src/web_ui.html`
before writing anything here.

A correction, because the first pass here got it wrong: schwung's
`docs/MODULES.md` says a custom `web_ui.html` is synth-slot only and audio-FX
modules cannot have one. Do not stop there. `remote_ui.go` also serves one for
an OVERTAKE tool (`handleSubscribeTool`), and smack proves the whole pattern
works in practice. The shape is:

- ONE `src/web_ui.html`. `scripts/build.sh` copies it into the sound_generator
  and overtake module dirs (`work-in/`, `overwork/`) and adds it to both
  tarballs. The audio_fx build does not get one and does not need one — the
  chain slot has the auto-generated controls.
- The page sniffs its own prefix at runtime rather than being built twice:

      let PREFIX = null;   /* "overtake_dsp:" (tool) or "synth:" (slot) */

  seeded from the first `onParamChange` burst, since `getParam` reads a LOCAL
  CACHE and not the device.
- Writes go through a single `rui_set` key rather than one param per control,
  which matters here for the same reason everything else does: the param
  channel is a blocking round-trip serviced once per SPI frame.
- Anything under the module dir is served at
  `/api/remote-ui/module-assets/<module-id>/<path>`; `module.json`,
  `config.json` and `secrets/` are refused. The iframe is sandboxed
  `allow-scripts allow-same-origin` — no navigation, popups or form submission.
- The manager polls `rui_poll` (rev:on:tick:bpm) and only does the expensive
  full `state` read when the revision moves. **The engine serves this as of
  v0.8.0.**
- It seeds the page by parsing `state` as a FLAT object, so only scalar fields
  survive. Work's blob is not flat — `p1`/`l1`/`vf` are arrays and `stp` is
  packed — so anything the page needs from those it must request itself, or
  the engine grows a flat view.

## Verification

```bash
make test        # engine simulator + UI harness — green before any release
make sanitize    # ASan + UBSan over the engine suite
make bench       # per-machine cost ranking (host machine, not the Move)
make arm         # Docker cross-compile + both tarballs (no hardware touched)
```

`make test` runs three suites. The engine simulator covers bypass transparency,
every machine bounded at min/default/max parameters, the global mix law, state
round-trip, short-buffer `get_param` canaries, compressor gain reduction,
machine-change state reset, FX LFO modulation, stage series routing, MIDI
clock/note handling, and the whole sequencer — lock apply/revert, every trig
condition, machine locks, micro-timing, a full 64-step round trip, and transport
restart. The UI harness covers the parameter-lock gesture, lock nudging, trig
toggling, the Shift escape, the machine palette, transport pads, jog behaviour,
copy/paste/clear, resume repaints, presets end to end, and that the UI never
reads or writes a key the engine does not handle. The third checks the manual
site's copy of the machine table against the engine's — it exists because a
careless edit silently swapped Flutter's N.LEV and N.HPF, and the page would
have shipped teaching the wrong knob.

**Never mock `os.*` from the code under test.** `test/ui_overtake.mjs` carries
a virtual filesystem written from QuickJS's real contracts — `os.readdir`
returns a `[names, errno]` tuple including `.` and `..`, and `os.rename` /
`os.remove` return `-errno` rather than throwing. One test deliberately runs
the naive flat-array reading against that mock and asserts it FAILS, so the
mock cannot quietly become too forgiving to catch the bug that broke Mono.

**Verified on hardware, 2026-07-29** — and only this much:

- FX machine selection through the whole 26-machine list (the knob-response fix).
- The sample path end to end: browser, WAV parse, chunked transfer across the
  param channel, and **One Shot firing from a sequencer trig**. Audio out
  of the Move.

**Still NOT verified by ear:** Polysample, Slicer, Wavescan and Tilt;
polyphony and voice stealing; note-to-pitch tracking; anything about how the
machines actually SOUND as opposed to producing signal.

**CPU, measured on the A53 for the first time on 2026-07-29.** `make
bench-tracks-arm` cross-compiles `test/bench_tracks.c`; copy it to the Move and
run it there. A "track" is 1 SRC machine + 2 insert FX + the voice filter +
sounding voices — the shape the reference device uses. Percentages are of ONE
core:

| profile                    | 1 trk | 4 trk | 8 trk | 12 trk | 16 trk |
|----------------------------|-------|-------|-------|--------|--------|
| light  1shot+tilt+fbank    |  4%   | 15%   | 30%   |  44%   |  59%   |
| mid    poly+mmf+drivedelay |  4%   | 15%   | 29%   |  47%   |  61%   |
| heavy  poly+2 reverbs      |  9%   | 38%   | 74%   | 115%   | 152%   |

Cost is essentially linear in track count, and light vs mid barely differ — the
fixed per-track overhead (voices, filter, modulators, sequencer) dominates
unless a reverb is involved. Reverbs are the only machines that move the needle.
So **eight tracks fits**; twelve fits unless every track runs two reverbs.

The Mac-vs-Move ratio came out at ~8x (8 heavy tracks: 9% on a Mac, 74% here),
which is worth knowing when reading `make bench` output.

**Read the caveat before quoting these.** The benchmark runs as its own process
on a free core. `MoveOriginal` is a 20-thread process sitting at roughly one
core's worth on a 4-core box, and DSP hosted inside its audio callback shares
*that* thread's budget, not a free core's. So the table is an upper bound on
what the hardware can do, not a measurement of what is left inside the render
callback. Whether audio_fx slots on different Move tracks are rendered on
different threads is UNKNOWN and worth settling before betting on it.

Memory is not a constraint: one 3-slot track is 6.1 MB (1.35 MB sample RAM,
1.58 MB of delay/reverb lines per slot), so 8 two-slot tracks is ~36 MB against
~1.3 GB free.

## Roadmap

Phase 1 (**done**) — the 20 FX machines as an `audio_fx` build.
Phase 2 (**done**) — `overwork`: the overtake build. Step buttons as Tonverk's
[TRIG] keys, machine palette on the pads, hold-step + turn-knob parameter locks,
trig conditions, micro-timing, retrig, pattern pages, copy/paste.
Phase 3 (**machines done**, v0.7.0) — the SRC machines.

All six now exist: **One Shot**, **Polysample**, **Slicer**,
**Granulator** (reading the loaded sample), **Wavescan** and **Tilt**, on top of
the sample memory and transfer below. Twenty-six machines total.

Three needed adaptation, and the docs say so rather than letting the names
imply otherwise:

- **Polysample** is eight-voice polyphonic with the documented vibrato, but
  Tonverk plays multi-sampled INSTRUMENTS mapped across the keyboard from its
  SD card. Work has one sample buffer, so every note plays that sample
  transposed. The polyphony and vibrato are real; the multisampling is not.
- **Slicer** implements the documented PLAYBACK set — play mode (forward,
  reverse, and both loops), STRT, LEN, L.ST. Its defining feature, eight
  samples on eight sequencer subtracks plus a supertrack, is NOT here and
  cannot be: Work is one source stage and two inserts, not an eight-track
  sampler. Reverse
  and a separate loop point still make it a real gain over One Shot.
- **Wavescan** has no SD card and no 127-slot wavetable store, so the loaded
  sample IS the wavetable, read as a series of 2048-frame waves with POS
  interpolating across them. SLOT is replaced by MIX.

**Tilt** needed no adaptation at all — a Work insert IS a bus insert, which is
exactly where the manual says its counterpart belongs. It is an EFFECT, not a
source: it makes no sound of its own, so it loads in an insert. The manual site
had it tagged a source for a while, and nothing caught that until the families
became real.

The constraint that shaped the whole design: `work_set_param` runs on the
SHIM'S AUDIO THREAD — `shim_handle_param_bulk` says so in its own comment
("this runs on the audio thread ~44x/sec") — so the DSP must never open a file.
The UI reads the WAV with `host_read_file_base64` (binary-safe; plain
`host_read_file` returns a C string and stops at the first NUL, which in a WAV
is usually inside the header), converts to interleaved 16-bit, and pushes it in
base64 chunks. Per chunk the engine does nothing but a bounded decode into
memory allocated once at `work_create()`.

    sample_begin  "<frames>[:<name>]"   reset the cursor, declare the length
    sample_chunk  "<base64>"            append, bounded by the allocation
    sample_end    anything              commit — sample_frames becomes visible
    sample_clear  anything              drop it

Nothing the render path reads moves until `sample_end`, so an interrupted
transfer leaves the previous sample playing rather than half of a new one.
Budget is `WORK_SAMPLE_SECONDS` (8) of stereo int16 = 1.35 MB per instance.

Voices: `WORK_VOICES` is 8, matching what the manual documents for Multi
Player, Slicer and Granulator. A voice is a read cursor plus an envelope, so
eight cost almost nothing next to the FX machines. Allocation is oldest-first.
Notes pitch the voice with 60 as unity, the sampler convention.

Still to do: per-voice filter and amp envelopes, the two voice LFOs, and the
arpeggiator. CPU on the A53 remains unmeasured.

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
