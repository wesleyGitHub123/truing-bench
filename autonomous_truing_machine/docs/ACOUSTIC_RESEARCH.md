# Acoustic measurement: what the research pipeline is, and what has been checked

The firmware's acoustic path (`lib/truing_dsp`) is a C port of a Python research pipeline that was
developed and tested first on recordings of plucked spokes. That pipeline lives in separate repositories
and is summarised here so this repository stands on its own.

## The signal chain

```
gate  ->  Hann window  ->  zero-pad  ->  FFT  ->  peak pick  ->  parabolic interpolation
```

One auditable transform from raw samples to a reported frequency, with no noise reduction, normalisation,
adaptive filtering or multi-window averaging. The fundamental frequency f1 of a plucked spoke is converted
to tension with a string model (an ideal-string relation and a stiff-string correction; a two-mode
inversion exists but is expected to be ill-conditioned on near-harmonic spectra).

The Python pipeline is organised as three stages with the features persisted between them (extract,
evaluate, report), its DSP core is pure (no I/O, no config, no global state, enforced by a test), and
every DSP parameter lives in one config file. It carries over a hundred tests.

## What was checked

**1. The frequency ruler, by an independent audit.** An independently written estimator (about 150
lines, importing only `numpy`) was run on exactly the same input samples as the pipeline. Result:

| | |
|---|---|
| synthetic cases (17) | repo and independent estimator agree to the printed precision (max 0.00000 Hz) |
| real recorded events (20) | dominant in-band frequency identical (max delta 0 Hz); qualified peak sets identical on 20 of 20 |
| the one truth mismatch | a deliberately planted coupled partner 8 Hz away; both estimators merge it into the same 453.66 Hz peak, a property of the signal, not of either estimator |

The audit's verdict was to freeze the ruler as infrastructure. **What that does and does not mean:** it
shows the FFT peak-picking measures a spectral peak's frequency correctly. It says nothing about whether
the peak it picks is the spoke's fundamental, and nothing about tension accuracy. The 16 of 20 real events
the audit classed as "intrinsic ambiguity" are multi-peak or high-drift signals, which is the
mode-identification question the audit explicitly left outside its verdict.

**2. The C port against the Python reference.** `acoustic_prep/make_golden.py` cuts excerpts from the
recorded campaign, runs the reference pipeline on them and stores the outputs as test fixtures. On the
three recorded plucks the C implementation's worst difference from the Python reference is 0.0000 Hz in
f1 and 0.004 dB in peak prominence (`test_acoustic_golden`). On the ESP32-S3 one analysis took 16.2 s
when first ported and 3.7 s after profiling (`docs/IMPLEMENTATION_NOTES.md`, Phase 1f).

**3. On this machine, with a solenoid and the INMP441.** This is where the evidence is weak, and it is
recorded in full in `docs/SOLENOID_CAMPAIGN.md`:

- The research constants (for example the 350 to 600 Hz search band) were set on recordings from a
  magnetic pickup. Nothing guarantees they hold for a MEMS microphone on a different wheel.
- Solenoid strikes excite the rig as much as the spoke. In the pre-registered strike blocks the board's
  DSP passed 4 of 168 strikes, and 1 of 54 in a later pulse-width sweep. Dozens of structural lines
  appear in every strike spectrum.
- A hand pluck on one spoke (spoke 0) passed 5 of 15 times, and four of those five agreed to within
  0.2 Hz at 406.2 Hz. One spoke, no tension reference.

No frequency or tension reading from this machine has been compared against a tensiometer.
