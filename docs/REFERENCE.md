# Reference sources

## The Tonverk user manual

Work's machines are written from the parameter descriptions in Elektron's
**published** Tonverk user manual, OS 1.3.3 (2026-05-07):

<https://www.elektron.se/wp-content/uploads/2026/05/Tonverk-User-Manual_ENG_OS1.3.3_260507.pdf>

That document is Elektron's copyright. It is **not** redistributed here — the
working text extract is gitignored (`docs/*.local.txt`). To reproduce it:

```bash
curl -sL -o /tmp/tonverk.pdf "https://www.elektron.se/wp-content/uploads/2026/05/Tonverk-User-Manual_ENG_OS1.3.3_260507.pdf"
pdftotext /tmp/tonverk.pdf docs/tonverk-manual-1.3.3.local.txt
```

The FX machines are Appendix A.3; the SRC machines (phase 3) are A.2. Appendix
B lists the modulation destinations, Appendix D the MIDI CC/NRPN maps.

## What was taken from it

Only the **documented parameter set and described behaviour** of each machine:
which knobs exist, what they are called, what range they span, and what the
manual says they do. For example, the manual states Filter Folder's signal flow
explicitly ("Input level > High-pass filter > Wavefolder > Multimode filter >
Dist > Output level"), so the implementation follows that order; it does not
follow Elektron's code, because we have never seen it.

Everything below that — the actual filter topologies, the reverb tank, the
pitch-shifter windowing, the Hilbert network, every coefficient — is our own.

## What was NOT taken

- No Elektron code, in any form.
- No factory presets, samples or wavetables.
- No firmware image was obtained, unpacked or inspected.

## OS release notes

The 1.0.0 → 1.3.3 release notes were also consulted for behaviour the manual
states loosely. Two entries shaped implementation choices directly:

- *"The Compressor FX machine's DRY/WET parameter did not have the correct snap
  values"* and *"Added snap values for the DRY/WET parameters for the FX
  machines"* — dry/wet is a snapped control on the hardware, worth matching if
  a UI ever adds detents.
- *"The PHASE 98 FX machine's FDBK parameter could, via modulation, be pushed
  outside its range to produce too loud sounds"* — our Phase 98 clamps feedback
  to 0.95 and the LFO path cannot push it past that.
