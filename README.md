# Work

**Twenty-six machines for the Ableton Move** — twenty-one effects and six
sources — as a [Schwung](https://github.com/charlesvestal/schwung) module.
A **source stage** followed by **two insert FX**, in series, each holding one
machine, plus three FX LFOs and a modulation envelope that can modulate any of
their parameters.

Loading a sampler costs you no insert: sources have their own stage. Each stage
only accepts machines from its own family and says so rather than substituting
something near enough, so the pads only ever offer what the focused stage will
actually take.

> Clean-room: every machine is an original DSP implementation, written from
> published descriptions of the effect it is named for — documented parameter
> names, ranges and behaviour — rather than from anyone's source. No
> third-party code and no factory content, and not a bit-exact emulation of
> anything. See [docs/REFERENCE.md](docs/REFERENCE.md).

## The machines

| | | | |
|---|---|---|---|
| Bypass | Decimator | Endless Flanger | Roomtone Reverb |
| Clock Pitch | Gritshaper | Low-Pass Filter | Drive Delay |
| Comb ± Filter | Fold Filter | Multimode Filter | Iron Room Reverb |
| Compressor | Filterbank | Wide Chorus | Voidspace Reverb |
| Chain Delay | Spectrum Bender | Phase Array | Flutter |
| | | | Granulator |

A few of the less obvious ones:

- **Clock Pitch** — granular pitch shifter with feedback, so held notes climb
  or fall in steps rather than sitting at one interval.
- **Comb ± Filter** — the FREQ knob is bipolar and its *sign* flips the comb's
  polarity: positive feedback sounds like a plucked string, negative like a
  stopped pipe.
- **Spectrum Bender** — a true single-sideband frequency shifter (Hilbert
  pair), not a pitch shifter. Intervals go inharmonic. SBND blends the two
  sidebands.
- **Endless Flanger** — barber-pole motion: three crossfaded taps so the sweep
  appears to rise or fall forever without turning around.
- **Fold Filter** — high-pass, then a wavefolder, then a morphing multimode
  filter, then distortion. Adds overtones rather than removing them.
- **Granulator** — granulates the last two seconds of live input, or the loaded
  sample when there is one. Eight knobs and a fixed Hann window, rather than the
  three pages of grain controls a dedicated granulator would give you.
- **The three reverbs are three algorithms** — a Schroeder comb bank with early
  reflections, a Dattorro plate, and a Householder feedback delay network.

## Two builds

**Work** is the `audio_fx` build — it drops into a Signal Chain slot or a
Master FX slot and processes whatever is upstream.

**Overwork** is the `overtake` build. It takes over Move's whole surface and
runs the same machines on the live input (mic / line / USB-C), with a 64-step
sequencer driving parameter locks, and a preset browser.

## Why the surface maps so cleanly

The gesture that defines hardware step sequencing — hold a step, turn an
encoder, and that step remembers the value — needs eight encoders and a row of
trig keys under your fingers at once. Move has both, so this is a 1:1 map
rather than an approximation:

| What it needs | Move |
|---|---|
| 8 rotary encoders | the 8 knobs |
| 16 trig keys | the 16 step buttons |
| a data encoder | the jog wheel |
| hold trig + turn encoder = p-lock | **hold step + turn knob = p-lock** |

That last row is the whole point. The gesture survives intact.

In Overwork the 32 pads are free for what a hardware sequencer spends menus on:
rows 1–3 are a machine palette for the focused stage (tap to load one), and row 4 is
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
3. ~~**Presets**, three distinct reverb algorithms, and **Granulator**.~~ done (v0.2.0)
4. ~~**The remaining source machines** — Polysample, Slicer, Wavescan.~~ done
   (v0.7.0). Sample loading goes through the UI in chunks, because the DSP has
   no filesystem access and `set_param` has a 16 KB ceiling.

## Siblings

Other Schwung modules in this family: [Smack](https://github.com/timncox/schwung-smack)
(loop glitcher), [Mark](https://github.com/timncox/schwung-mark) (5-track looper),
[Belt](https://github.com/timncox/schwung-belt) (vocal processor),
[Mono](https://github.com/timncox/schwung-mono) (machine synth).

MIT licensed.
