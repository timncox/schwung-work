---
status: active
last_touched: 2026-07-30
---

# Work

Schwung module for the Ableton Move: **twenty-six machines in a source stage
and two insert FX** — twenty-one effects and six sources — inspired by the
Elektron Tonverk's per-track shape. Eight tracks, each with four LFOs in two
families and a modulation envelope.

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

Measured 2026-07-30, at eight tracks:

| pattern | blob | host reads |
|---|---|---|
| empty | 716 B | 1 |
| 64 trigs on every lane | 11,368 B | 1 |
| …plus 4 locks per step | 14,120 B | 1 |
| …plus all 36 locks per step | 35,944 B | 3 |

`test_blob_sizes_are_reported` prints this table every run, because a table
nothing measures is a table that drifts — these numbers were several format
changes stale before it existed.

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

## Reaching the eight tracks from the surface

Everything either UI shows belongs to the SELECTED track, so changing track is
navigation and not a parameter — it replaces every value on screen exactly as
changing page does. Both UIs put it on a navigation control for that reason.

**Overwork** (full surface): `UP` / `DOWN` step through the tracks, and holding
`MENU` turns the sixteen step buttons into a direct-jump track picker (the LEDs
change to say so, and change back on release). The screen's header rule doubles
as an eight-cell strip — solid is selected, underlined has trigs — so "where is
my material" is answerable without leaving the page.

**Work** (chain slot): `SHIFT` + jog turn. A knob was tried first and does not
fit: the machines page's scalar row is three cells of 42 px, and a fourth makes
`LVL 117` render as `LVL 1`. There is no room for a second row either — the
first already runs to y=56 against a footer rule at y=55.

Both clamp at the ends rather than wrapping. Pages are a short ring you spin
through; tracks are an instrument's worth of material, and landing on track 8
because you nudged once past track 1 is a jump, not a nudge.

The buttons are a CHOICE, and the alternatives were checked rather than assumed:
the four row buttons beside the pad grid are **LED-only** (absent from schwung's
`MoveCCButtons`, so they send nothing), `SHIFT` + step already selects the
pattern and uses all sixteen, and pad row 4 is full on both its layers.

`tracks` and `track_map` come from the ENGINE — the same rule the machine count
follows. `track_map` is `"<trigs>:<source machine>"` per track in ONE read,
because a parameter read is a blocking ~23 ms round trip and eight of them to
redraw a strip is 184 ms of the audio thread's budget every time the selection
moves.

## Modulation: two LFO families

Four LFOs per track, and they are **not interchangeable**:

- **voice** LFO 1, 2 — the source stage's eight parameters, then the seven
  voice-filter fields. 15 destinations.
- **FX** LFO 1, 2 — insert 1 A–H, then insert 2 A–H. 16 destinations.

The two destination spaces both count from zero and mean different things, so a
destination may only be read alongside the family of the LFO carrying it —
`dest` 8 is the filter's BASE on a voice LFO and insert 2's knob A on an FX one.
That is why the parameter keys put the family first (`vlfo1_dest`, not
`lfo1_dest`) and why the engine serves `lfo_dests` rather than letting a UI
compute the ranges.

Resolution lives in **exactly one place** — the family branch at the end of
`build_effective`. Everything else (defaults, phase advance, retrig, state I/O)
treats an LFO as an LFO. `DESIGN-8TRACK.md` asked for two separate arrays; one
array plus `work_lfo_is_voice()` keeps the uniform loops uniform and puts the
guarantee where it can actually be checked.

A pre-v4 preset had three LFOs in one flat 0..23 space. They migrate **by where
they pointed**: at the source stage makes a voice LFO, at an insert makes an FX
one with 8 subtracted. Three into two-plus-two does not always fit, and the
overflow goes on `load_note` rather than vanishing.

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
machine selects, 26 dry/wet, 27 track level, 28 track pan, 32/40 voice LFOs, 48/96 FX LFOs,
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

**`set_open_tool` does NOT work on the host built from `main`** (2026-07-30,
second finding of the day). `bus.set_open_tool('overwork')` returns without
error and leaves `overtake_mode` at 0; the manager still reports no active
tool. It was recorded as working earlier the same day, against whichever host
happened to be installed then — so read that as describing one build, not the
contract. Open Overwork by hand (Shift+Vol+Jog Click -> Tools) before the suite.

Two ways to misread the daemon's state, both of which cost time today:

- `pgrep -f schwung-testd` **over SSH matches its own command line** and
  reports the daemon running when nothing is. Use `ps aux | grep "[s]chwung-testd"`.
- An SSH port-forward established before a reboot **survives locally while
  forwarding to nothing**, and fails with `ConnectionResetError` that reads
  exactly like a daemon crash. Every reboot kills testd; it is not
  auto-started.

**`tool_info` lies in the `(none)` direction.** The manager's
`subscribe_tool` reply is the best available check for what is open, and it
transiently reports `(none)` while a tool is demonstrably running — observed
2026-07-30, two spurious `(none)` readings inside five seconds with Overwork up
and answering. It does NOT appear to invent a tool that is not there, so treat
a tool id as trustworthy and `(none)` as needing corroboration: poll it several
times a few seconds apart and require the whole run to agree before writing
over a `dsp.so`. One spurious reading is enough to greenlight a deploy onto a
mapped module, which is the crash that costs a power cycle.

**You cannot check whether a module is mapped from `ableton`.** `grep
/data/UserData/schwung/modules /proc/*/maps` returns EMPTY whether or not an
overtake DSP is loaded, because the shim lives inside MoveOriginal and
`/proc/<its pid>/maps` is permission-denied to us — verified 2026-07-30, after
I used exactly that check to declare a deploy safe. It proved nothing. The
honest check is the manager's `tool_info` over `ws://move.local:7700/ws/remote-ui`
(subscribe_tool), which reports the active overtake tool by id.

**Deploy with the tool CLOSED.** `scripts/deploy.sh` overwrites `dsp.so`; doing
that while an overtake module has it mapped crashed MoveOriginal on 2026-07-30
and needed a power cycle. Check `bus.state().overtake_mode == 0` first, deploy,
then `set_open_tool`. The script's own warning is about stale code — this is the
worse failure it does not mention.

**The suite reaches fully green — 10 passed, 1 skipped, 1 xfailed — but only
about half of runs get there.** Both causes are in schwung, not here.

*Fixed 2026-07-30:* `shadow_ui.js:15208` logs EVERY MIDI message while in
overtake mode, before any filtering ("Debug: log all MIDI ... to diagnose
escape issues"). Two synchronous appends per event to `debug.log`, which had
reached **1.2 GB** and grew at **143 KB per 10 s while IDLE** — the ring
misalignment that degrades to an endless run of `status=0 d1=0 d2=0` was
written out in full. Disabled by renaming the flag file
`/data/UserData/schwung/debug_log_on` (it gates all unified logging;
`unified_log.h` checks for it periodically). Growth went 286,000 -> 3,231 bytes
per 20 s, and the suite went from never green to green roughly half the time.
**Rename the flag back to restore logging.**

*Largely fixed 2026-07-30*, in schwung (timncox/schwung PR #4). The param
channel is ONE shared-memory slot and the test daemon reads through the same
one as a running module. Three defects compounded: both sides generated request
ids from the same counter starting at 1, so each matched the OTHER's response;
a reply could carry the newlines of a module's BULK payload, which desynced the
socket so one collision failed the whole run; and nothing verified the response
belonged to the requester. Eight-run samples: **never green -> 6/8 green, worst
run 1 failure**.

**The device is running a hand-placed `schwung-testd`** until the next real
`install.sh` — schwung's own rule is never to scp individual files, and this
one was, because a full install would have taken the host 0.11.4 -> 0.11.6 to
legitimise a test binary. The previous one is at
`bin/schwung-testd.pre-race-fix`. A normal install supersedes it.

*Still open:* both producers claim the slot with check-then-write rather than an
atomic claim, so writes can still interleave. Closing it needs a claim value the
shim ignores, and the shim clears any non-zero `request_type` it does not
recognise — an audio-path change, not worth it for a test tool. The residue is a
retryable error now, and the Python client retries it. **A red run is still
worth re-running once** before believing it.

Idling the tick poll when nothing can have changed (see `pollLive`) narrows the
window and is worth having anyway — a blocking ~23 ms round-trip 22x/sec to
read a stopped playhead is half the param channel for nothing — but it did NOT
measurably help here, because the FX page is the default and keeps the poll
live.

**One manual step remains.** Overwork must already be open on the device.
`bus.set_open_tool('overwork')` writes the command and shadow_ui reads it, but
answers `tool not found: overwork` — its open_tool path searches
`scanForToolModules()` (component_type `tool`) and overtake modules come from
`scanForOvertakeModules()` instead. Teaching schwung's open_tool to fall back to
the overtake list would make the suite fully unattended; until then every test
skips with a clear message rather than failing.

## The browser editor

`src/web_ui.html` — one page, shipped into **`work-in`** (sound generator) and
**`overwork`** (overtake tool) by `scripts/build.sh`. It sniffs its own
parameter prefix at runtime (`overtake_dsp:` as the tool, `synth:` in a slot)
rather than being built twice.

**The audio_fx build does not get one and cannot.** `remote_ui.go` only looks
for `web_ui.html` on a slot's `synth` component, so a chain slot could never
load it. That build's Remote UI is the auto-generated controls, which is what
`ui_hierarchy` and `chain_params` are for — see below.

Three constraints shaped the whole page, and none of them are obvious:

- **Everything it draws must be a scalar in the `state` blob.** The manager
  parses `state` as a flat object and drops every array, and
  `schwungRemote.getParam` answers from the iframe's CACHE of that parse
  rather than reading the device. A value the mirror omits is a value the page
  cannot obtain at all — which is why `fmach`, `ffam*` and `flab*` exist:
  `machines`, `src_codes` and `labels_src` are served as their own keys and
  the browser can reach none of them.
- **Writes go through one `rui_set` key** — `"<param>:<value>"`. The param
  channel is a blocking round trip serviced once per SPI frame, so a key per
  control would be a round trip per control, and dragging a slider would be a
  queue.
- **The step grid unpacks the lane's base64 in JS** rather than reading 64
  step values. The lane is already in the blob it was handed.

What it shows: eight tracks with what each is running, then full detail for
the selected one — three stages under the loaded machines' own knob names, the
voice filter, four LFOs, the mod envelope, and the pattern. Every parameter
shows the knob value AND the effective one when a lock or LFO has moved them
apart; without that a modulated parameter looks stuck. Selecting a track
writes `track`, exactly as the surface does.

**Verify by rendering it.** `make build/state.json` emits a real blob from the
engine (`test/dump_state.c`), and `node test/make_web_harness.mjs` replays it
through a mock with the manager's own semantics — flat parse, arrays dropped —
into `build/web_ui_harness.html`. Open that file in a browser. It is the only
way to catch the class of bug that shipped here first time: `#ui { display:
none }` in the sheet against `style.display = ""` in the code, which built the
entire page and showed none of it. Reading the code would not have found it.

`test/site_matches_engine.mjs` guards the seam: every mirror key the page
names must exist in a real blob, and a hardcoded machine name fails the suite.

### The chain slot's Remote UI

`ui_hierarchy` and `chain_params` are both served, and the hierarchy carries
**`"remote_only": true`**.

That flag is the whole reason the chain slot can have a Remote UI at all.
schwung-manager builds the browser's control list from `ui_hierarchy` and has
no other source, but on the device answering that key hands
`enterComponentEdit()` to the generic hierarchy editor and `ui_chain.js` never
loads — and that editor re-reads `chain_params` only on preset change or for
keys matching `/_rate_mode$/`, so Work's labels would sit on the previous
machine. One key, two surfaces, opposite needs. `remote_only` splits them
(timncox/schwung#5; schwung's `hierarchyDrivesDeviceEditor`, documented in its
`docs/MODULES.md`).

**This needs a schwung host that knows the flag.** An older host ignores it and
diverts as it always did — the old behaviour, not a new break, and the labels
are correct either way now because the hierarchy is built from the loaded
machine rather than declared statically.

Machine options in `chain_params` come from `work_machine_fits_stage`, the same
gate `set_param` enforces, so a client cannot offer a machine the write would
then refuse. LFO destination ranges follow the family, because both count from
zero and `dest` 8 otherwise names two different things.

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

**Verified by ear, 2026-07-30** — and only this much: ONE track running
Granulator as its source into two insert effects, on the v0.9.0 build, judged
"working great". That covers the source-into-two-inserts path and the effects
on it. It does not cover eight tracks at once, or any machine not named here.

**Still NOT verified by ear:** Polysample, Slicer, Wavescan and Tilt;
polyphony and voice stealing; note-to-pitch tracking; more than one track
sounding at a time; anything about how the machines actually SOUND as opposed
to producing signal.

**CPU, measured on the A53.** `make bench-tracks-arm` cross-compiles
`test/bench_tracks.c`; copy it to the Move and run it there. A "track" is
1 SRC machine + 2 insert FX + the voice filter + sounding voices. Percentages
are of ONE core, measured 2026-07-30 (two runs, agreeing within 1-2 points):

| profile                    | 1 | 2 | 4 | **8** | 12 | 16 |
|----------------------------|---|---|---|-------|----|----|
| light  1shot+tilt+fbank — N instances |  5% | 10% | 20% | **39%** | 59% | 80% |
| light — one engine, N tracks          |  5% |  9% | 15% | **28%** |  —  |  —  |
| mid    poly+mmf+drivedelay — instances |  5% | 10% | 19% | **39%** | 60% | 80% |
| mid — one engine                       |  5% |  9% | 16% | **29%** |  —  |  —  |
| heavy  poly+2 reverbs — instances      | 10% | 21% | 42% | **86%** |132% |172% |
| heavy — one engine                     | 10% | 20% | 41% | **82%** |  —  |  —  |

**Eight light or mid tracks fit at under 30% of a core. Eight HEAVY tracks do
not** — 82% is past the ~50% line, and it is the reverbs, as it always was.

The **real engine is cheaper than N one-track instances**, and by a shrinking
margin as the profile gets heavier: 11 points at light, 9 at mid, 4 at heavy.
That is the shape you would expect — what a single instance shares between its
tracks is the FIXED overhead (sequencer, transport, pattern, mix stage), so it
is a large fraction of a light track and a small one of a track running two
reverbs. `bench_tracks` measures both modes and prints the delta.

**Two corrections to what was recorded on 2026-07-29.** Those figures (light
4/15/30%, heavy 9/38/74%) were wrong in the same way: `set_slot()` wrote
`"fx%d"` for every stage including the source, so after the SRC promotion the
source machine went to insert 1 and the **family gate silently refused it**.
Every profile measured two effects over an empty source stage with no voices
sounding. `set_stage()` now uses the stage's own key and **reads back what it
loaded**, exiting if a stage refuses. A benchmark that does not check what it
configured is measuring something it cannot name.

**Read the caveat before quoting these.** The benchmark runs as its own process
on a free core, with `MoveOriginal` sitting at roughly one core's worth of a
4-core box (load average was 3.1 during these runs). DSP hosted inside its audio
callback shares *that* thread's budget, not a free core's. So the table is an
upper bound on what the hardware can do, not a measurement of what is left
inside the render callback. Whether audio_fx slots on different Move tracks are
rendered on different threads is UNKNOWN and worth settling before betting on it.

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
