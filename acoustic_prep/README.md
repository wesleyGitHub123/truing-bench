# acoustic_prep — golden fixtures for the firmware acoustic port

Phase 1f ports the research repository's acoustic layers 2–4 to C (SPEC 4.4.1,
9.2). SPEC 14.2 requires that port to be verified against the Python
implementation on identical inputs, with the fixtures copied into this
repository rather than referenced across repositories.

`make_golden.py` imports the research repository's own pure DSP modules
(`lib/dsp`), cuts 1.2 s excerpts around individual plucks in the recorded
tension-sweep campaign, stores them as the int32 I2S word stream the firmware
front end delivers (24-bit sample in the upper bits), runs the reference
pipeline on each excerpt, and writes:

| Output | Purpose |
|---|---|
| `autonomous_truing_machine/test/fixtures/acoustic/*.pcm` | the excerpts (three plucks, one noise-floor segment) |
| `golden/acoustic_golden.json` | every reference output and every parameter used, plus the research commit |
| `autonomous_truing_machine/lib/truing_fixtures/include/truing_fixtures/acoustic_golden.h` | the same values as C initialisers for the native and on-target tests |

```bash
# from the repository root; the system Python 3.12 carries numpy, scipy and pyyaml
python acoustic_prep/make_golden.py --acoustic-repo "../Acoustic Repo" --out-dir autonomous_truing_machine
python -m pytest -q acoustic_prep/tests
```

Two things in the generator are deliberate departures from the research
configuration, both recorded in the JSON:

- **An absolute onset floor** (−55 dBFS rms). `config/dsp.yaml` leaves it
  null because a campaign file always contains plucks; a device capture may
  contain nothing, and the relative rule then fires on noise. The floor sits
  14 dB above this chain's recorded noise floor and below every recorded
  pluck's own relative threshold, so pluck detection is unchanged.
- **The two-mode model's length bounds** (`models.yaml m2.l_eff_bounds`) are
  applied, as `lib/models/m2.py` does; the third pluck's inversion is
  rejected by them and the fixture records that.

The model outputs use the firmware fixture profile's constants (2.0 mm gauge,
L_eff = crossing distance 193.5 mm). Those are SYNTHETIC fixture content: the
ideal-string tension they give for the 1332.8 N tensiometer pluck is about
half the reading, which is the effective-length question SPEC 4.4.1 leaves
open. The fixtures verify the port, not the physics.
