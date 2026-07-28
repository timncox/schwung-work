# Work

**Twenty-one FX machines for the Ableton Move**, as a [Schwung](https://github.com/charlesvestal/schwung)
module. Two insert slots in series, each loaded with one machine, plus two FX
LFOs that can modulate any slot parameter.

Inspired by the FX section of the Elektron Tonverk. *Tonverk* is Swedish for
"tone works" — hence **Work**.

> Clean-room: every machine here is written from Elektron's **published user
> manual**, from the documented parameter names, ranges and descriptions. This
> project contains no Elektron code and no Elektron factory content, and is not
> a bit-exact emulation. See [docs/REFERENCE.md](docs/REFERENCE.md).

## The machines

| | | | |
|---|---|---|---|
| Bypass | Degrader | Infinite Flanger | Rumsklang Reverb |
| Chrono Pitch | Dirtshaper | Low-Pass Filter | Saturator Delay |
| Comb ± Filter | Filter Folder | Multimode Filter | Steel Box Reverb |
| Compressor | Filterbank | Panoramic Chorus | Supervoid Reverb |
| Daisy Delay | Frequency Warper | Phase 98 | Warble |
| | | | Grainer |

A few of the less obvious ones:

- **Chrono Pitch** — granular pitch shifter with feedback, so held notes climb
  or fall in steps rather than sitting at one interval.
- **Comb ± Filter** — the FREQ knob is bipolar and its *sign* flips the comb's
  polarity: positive feedback sounds like a plucked string, negative like a
  stopped pipe.
- **Frequency Warper** — a true single-sideband frequency shifter (Hilbert
  pair), not a pitch shifter. Intervals go inharmonic. SBND blends the two
  sidebands.
- **Infinite Flanger** — barber-pole motion: three crossfaded taps so the sweep
  appears to rise or fall forever without turning around.
- **Filter Folder** — high-pass, then a wavefolder, then a morphing multimode
  filter, then distortion. Adds overtones rather than removing them.
- **Grainer** — granulates the last two seconds of live input. Tonverk's is an
  SRC machine that granulates a *sample* across 24 parameters; this one has no
  sample slot and a fixed window, and works on what you play into it.
- **The three reverbs are three algorithms** — a Schroeder comb bank with early
  reflections, a Dattorro plate, and a Householder feedback delay network.

## Two builds

**Work** is the `audio_fx` build — it drops into a Signal Chain slot or a
Master FX slot and processes whatever is upstream.

**Overwork** is the `overtake` build. It takes over Move's whole surface and
runs the same FX on the live input (mic / line / USB-C), with an Elektron-style
step sequencer driving parameter locks, and a preset browser.

## Why the surface maps so cleanly

Tonverk is an eight-encoder-per-page instrument with sixteen `[TRIG]` keys.
Move has eight knobs and sixteen dedicated step buttons. So the mapping isn't
an approximation — it's the same instrument shape:

| Tonverk | Move |
|---|---|
| 8 rotary encoders | the 8 knobs |
| 16 `[TRIG]` keys | the 16 step buttons |
| `LEVEL/DATA` | the jog wheel |
| hold trig + turn encoder = p-lock | **hold step + turn knob = p-lock** |

That last row is the whole point. The gesture survives intact.

In Overwork the 32 pads are free for what Tonverk uses menus for: rows 1–3 are
a 21-machine palette (tap to load one into the focused slot), and row 4 is
transport, pattern pages, copy/paste and clear — with Shift turning its last
four into the step-attribute modes.

Steps carry trig conditions (`FILL`, `PRE`, `1ST`, `A:B` ratios, probabilities),
micro-timing (±23/24 of a step), and retrig. Each firing trig is a complete
snapshot: parameters it locks take their locked value, parameters it doesn't
revert to base.

## Install

schwung-manager → **Install Custom Module**, pointed at
[`timncox/schwung-work`](https://github.com/timncox/schwung-work) for Work,
[`timncox/schwung-work-in`](https://github.com/timncox/schwung-work-in) for Work In,
or [`timncox/schwung-overwork`](https://github.com/timncox/schwung-overwork) for Overwork.

Full manual: **https://timncox.github.io/schwung-work/**

## Build and test

```bash
make test        # engine (414) + UI harness (47) + site-vs-engine (44)
make sanitize    # the engine suite under ASan + UBSan
make bench       # per-machine cost ranking
make arm         # Docker cross-compile for the Move + both tarballs
```

The UI harness mocks the host against a contract generated *from the engine*
(`test/dump_contract.c`), so a UI that reads a key the DSP doesn't serve fails
in CI rather than on hardware.

Deployed to hardware, but **nothing has been verified by ear yet**.

## Roadmap

1. ~~**The FX machines** — `audio_fx` build: Signal Chain and Master FX slots.~~ done
2. ~~**Overwork** — the `overtake` build: step buttons as trig keys, machine
   palette, hold-step-and-turn parameter locks, trig conditions, micro-timing.~~ done
3. ~~**Presets**, three distinct reverb algorithms, and **Grainer**.~~ done (v0.2.0)
4. **The remaining SRC machines** — Single/Multi Player, Subtracks, Wavefinder,
   Shape. Gated on sample loading: the DSP has no filesystem access, so samples
   must arrive through the UI in chunks under the 16 KB `set_param` ceiling.

## Siblings

Other Schwung modules in this family: [Smack](https://github.com/timncox/schwung-smack)
(loop glitcher), [Mark](https://github.com/timncox/schwung-mark) (5-track looper),
[Belt](https://github.com/timncox/schwung-belt) (vocal processor),
[Mono](https://github.com/timncox/schwung-mono) (machine synth).

MIT licensed.
