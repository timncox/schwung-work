# Work — the eight-track restructure

Status: **steps 1-3 done (2026-07-29); 4 onwards not started.** Read this
before touching `work_core.h`. It exists because the refactor spans more work
than one session holds, and because two of its decisions are expensive to
reverse.

**Step 4 is blocked on the MIDI decision below** — channel-per-track vs a track
field in the NRPN number. Eight lanes need to know which one before the CC map
grows a track axis.

## Why

Work models one track. The reference device has eight, each with its own source
stage and **two** insert FX — not one track with a deep chain. Everything built
between v0.7.0 and the `four-insert-slots` branch grew the chain deeper, which
is motion away from the shape we want.

Grounding, from the OS 1.3.3 manual (see `docs/REFERENCE.md` for the extract):

- Sixteen tracks: eight audio (1-8), four bus (9-12), three send FX (13-15).
- Each audio and bus track has its own **two** selectable insert FX and its own
  routing destination.
- Per audio track, the signal is SRC -> amp/envelope -> base-width filter,
  overdrive and multimode filter in a routable order -> INSERT FX 1 -> INSERT
  FX 2 -> routing.
- The MOD pages hold **two LFOs for the track parameters, one envelope for the
  track parameters, and two LFOs for the track's insert FX parameters.**
- Track selection is `[TRK] + [TRIG 1-16]`.

Scope for now: **the eight audio tracks.** Bus tracks, send FX tracks and the
routing matrix are explicitly out — they are a second phase, and nothing in this
design should make them impossible.

## It fits — measured, not extrapolated

`make bench-tracks-arm`, run on the Move 2026-07-29. Percentages of one core,
where a track is 1 SRC + 2 insert FX + voice filter + sounding voices:

| profile | 1 | 4 | 8 | 12 | 16 |
|---|---|---|---|---|---|
| light | 4% | 15% | 30% | 44% | 59% |
| mid | 4% | 15% | 29% | 47% | 61% |
| heavy (2 reverbs/track) | 9% | 38% | 74% | 115% | 152% |

Eight fits. Reverbs are the only machines that move the needle, so the design
guidance is "don't run a reverb on all eight" rather than a hard cap. Memory is
not a constraint: eight two-slot tracks is ~36 MB against ~1.3 GB free.

Caveat that must travel with these numbers: the benchmark is its own process on
a free core, while `MoveOriginal` is a 20-thread process using about one core's
worth. DSP inside its render callback shares that thread, not a free core.

## Engine shape

`work_t` today *is* one track — that is why the benchmark above is honest. The
split:

```c
typedef struct {
    /* SRC is its own stage now, not a machine occupying an FX slot. */
    work_slot_cfg_t src_cfg;
    work_slot_t     src;

    work_slot_cfg_t cfg[WORK_INSERTS];      /* insert FX 1 and 2 */
    work_slot_t     slot[WORK_INSERTS];

    work_vfilt_cfg_t vfilt;                 /* per voice, per track */
    int16_t         *sample;
    char             sample_path[192];

    work_lfo_cfg_t   vlfo[2];               /* reach SRC / filter / amp */
    work_lfo_cfg_t   fxlfo[2];              /* reach this track's insert FX */
    work_menv_t      menv;

    uint8_t level, pan, mute, solo;
} work_track_t;

typedef struct {
    work_track_t   trk[WORK_TRACKS];        /* 8 */
    work_pattern_t pat[WORK_PATTERNS];      /* each holds 8 lanes */
    ...                                     /* transport, song, mix stay global */
} work_t;
```

`WORK_INSERTS` is 2 and should stay 2. If a third ever seems necessary, re-read
the CC-space note below first.

### The two LFO families are not interchangeable

A voice LFO reaches SRC, voice-filter and amp parameters. An FX LFO reaches this
track's two insert FX. Keeping them as separate arrays with separate destination
ranges is deliberate — one flat "LFO with a dest 0..N" would let a voice LFO
point at a reverb parameter, which the reference device does not allow and which
makes the destination list twice as long to page through on eight encoders.

## The lock map becomes per-track

This is the decision most expensive to get wrong.

Today the map is global and **append-only**, because a lock index is part of the
saved pattern. Eight lanes changes the pattern format anyway, so the map is
rebuilt once, cleanly, and becomes **per track** — index 0 means the same thing
on every track:

```
 0..7    SRC parameters A-H
 8..15   insert FX 1 parameters A-H
16..23   insert FX 2 parameters A-H
24       SRC machine
25       insert FX 1 machine
26       insert FX 2 machine
27       level
28       pan
29..35   voice filter: base, width, reso, env, attack, decay, track
------
36 lockable per track       -> uint64_t lock_mask, as on `four-insert-slots`
```

Rebuilding it is allowed exactly once, here, and only because the lane count
already breaks the format. After this it is append-only again, forever. Add new
lockables at 36+.

### Preset migration is required, not optional

Saved presets carry the v1 global map (0..27, or 0..36 if anything from the
`four-insert-slots` branch ever shipped — it must not). The state blob gains
`"v":2`. A v1 blob loads into **track 1** with an index translation:

Written before step 2 landed, this table described migrating from the
PRE-promotion map. What shipped covers both, because step 2 changed what v1's
indices mean without changing the version:

| v1 index (post-promotion) | meaning | v2 index |
|---|---|---|
| 0..7 | SRC params | 0..7 |
| 8..15 | FX 1 params | 8..15 |
| 16 | SRC machine | 24 |
| 17 | FX 1 machine | 25 |
| 18 | global dry/wet | dropped, and reported |
| 19..26 | FX 2 params | 16..23 |
| 27 | FX 2 machine | 26 |

A blob written BEFORE the promotion has every index shifted one stage earlier.
`state_stage_shift()` tells them apart by the flat-mirror key names.

A v1 patch whose slot 1 held a source machine is the common case, and the
translation above puts that source in insert FX 1 rather than the SRC stage.
Special-case it: if the v1 slot 1 machine is a source, route it to SRC (0..7 and
index 24) instead. Anything ambiguous should load and say what it did, never
silently land somewhere wrong.

## Surface

Move gives 16 step buttons (notes 16-31) **separate** from the 32 pads (68-99),
so steps and the palette never compete. Eight encoders, jog, Shift.

**Default layer**
- Steps 1-16: the selected track's steps, current page.
- Pad rows 1-3 (92-99, 84-91, 76-80): machine palette for **the stage the knobs
  are currently editing**. 81/82/83 stay UNDO / MEMO / SONG.
- Pad row 4 (68-75): PLAY, TRACK, PAT PG, EDIT PG, COPY, PASTE, CLEAR, FILL.

**Hold TRACK (pad 70)** — the `[TRK] + [TRIG]` gesture
- Steps 1-8 select a track; steps 9-16 dark and reserved (track copy/paste is
  the obvious future use, mirroring `[TRK]+[TRIG]+[REC]`).
- Pad row 1 mutes track 1-8, row 2 solos track 1-8.
- Tapping TRACK alone advances to the next track.

**Hold SHIFT** — unchanged: steps are pattern select, row 4 the step-attribute
modes.

### Three surface decisions

1. **The palette loads into whatever stage the knobs are editing.** EDIT PG
   cycles SRC -> VOICE FLT -> FX 1 -> FX 2 -> VOICE LFO 1/2 -> FX LFO 1/2 ->
   MOD ENV -> GLOBAL, and a palette pad loads into the current stage. This is
   what frees pad 70 (the old slot-cycle button) to become TRACK. It also
   retires the SHIFT palette bank: split into 21 effects and 5 sources, each
   list fits rows 1-3 outright, where today reaching machine 22+ needs Shift.

2. **Tracks live on the screen, not permanently on pads** (agreed 2026-07-29).
   Pads show tracks only while TRACK is held; the persistent view — selected,
   playing, muted — is a row of eight on the 128x64 screen. Pads are for doing,
   the screen is for knowing. The cost, accepted: no glanceable track state
   without looking at the screen.

3. **FILL moves to the end of row 4** and MONITOR takes its Shift position,
   because row 4 gained TRACK and had to lose something. Least confident of the
   three; revisit if FILL turns out to be played constantly.

Remaining free gestures after this: Shift + pad 70, and steps 9-16 under TRACK.
That is the entire budget — spend deliberately.

## MIDI

Per-track CC is not possible in 7-bit space: one track already spends ~30
controllers and eight would need ~240 plus the globals. Two options, and this is
**not yet decided**:

- **MIDI channel per track.** Track N listens on channel N, and the existing
  two-slot CC map (8..26) is reused verbatim on each. Cheap, conventional, and
  it makes the map someone already learned keep working.
- **NRPN with a track field in the parameter number.** More controllers, but
  nothing on a hardware controller can reach it without NRPN support.

Channel-per-track is the recommendation. Note it retires the CC 80-97 block that
slots 3 and 4 used on the `four-insert-slots` branch.

## Order of work

Each step lands with tests green; nothing here needs a big-bang merge.

1. ~~`work_track_t` extracted, `WORK_TRACKS` = 1.~~ **done.** Pure refactor,
   no behaviour change, existing tests passed untouched.
2. ~~SRC promoted out of the FX slots into its own stage; machine list split
   into source and effect families; `WORK_INSERTS` = 2.~~ **done.** What it
   turned up, all of which was already broken and merely invisible:
   - a machine p-lock bypassed the family gate entirely
   - `meter` reported a stage that cannot hold a compressor
   - the CC/NRPN machine selects had dead zones — 103 of 128 positions on the
     source stage — because they scaled across all 26 codes and the refused
     ones did nothing
   - `test/dump_contract.c` probed keys with values the fixture had already
     set, so `src` was reported read-only and the chain harness blamed the UI
   - the manual site had Tilt tagged a source
   The lesson for steps 3-6: **migrating the tests is where the bugs are.**
   Several passed for the wrong reason the moment families were enforced — the
   machine sweep loaded every machine into an insert, where every source was
   refused, and swept an empty chain.
3. ~~Lock map rebuilt per-track, `lock_mask` widened, v1 -> v2 preset
   migration with its own tests.~~ **done.** The map came out at 36 with the
   stage parameters CONTIGUOUS, level and pan made real rather than reserved,
   and the global dry/wet dropped as a lockable. What it turned up:
   - a trig in the same block as a machine change was swallowed — the
     machine-change reset cleared the voice seq_run had just started
   - `work_src_trigger` switched on the BASE machine, so a machine lock's
     machine was never started at all
   - the chain editor's MIX row had been drawing at `y=undefined` since the
     third stage landed, invisible because `undefined < 0` is false
   The migration needed one thing the plan did not anticipate: "v":1 covers TWO
   maps, and only the flat-mirror key names date a blob to before or after the
   SRC promotion.
4. `WORK_TRACKS` = 8; sequencer grows to eight lanes.
5. LFOs split into voice and FX families, two of each per track.
6. Surface: TRACK modifier, per-stage palette, screen track strip.
7. Re-run `bench-tracks` on the Move against the real engine rather than N
   instances, and record the delta.

## What must not be lost

- The clean-room boundary in `CLAUDE.md` — none of the above changes it.
- Machine names stay ours. The families are "source" and "effect", not any name
  the reference uses for them.
- `four-insert-slots` must **not** merge. Two inserts is the target. The branch
  is kept for the two real bug fixes inside it (the chain editor's LFO
  destination frozen at 15, and `work_lock_label`'s hand-written case chain) —
  cherry-pick those, drop the rest.
