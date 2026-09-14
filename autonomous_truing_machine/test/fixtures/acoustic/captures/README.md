# Acoustic capture bundles

Samples kept from real measurements, so that a pluck which behaved badly once can be worked on
afterwards. A bundle is two files:

    <name>.pcm     the raw int32 words the front end delivered (24-bit sample in the upper
                   bits, little-endian) — byte-identical to what the DSP analysed on the board
    <name>.json    a flat object: where the words came from, what the board concluded from
                   them, and the digest of the acoustic chain configuration in force

`index.txt` lists the bundles to replay, one name per line.

## The loop

    pluck on the bench                      the board keeps the words it just analysed
    tools/capture_fetch.py                  metadata + samples off the board, into a bundle
    pio test -e native -f test_acoustic_replay
                                            the same layers 2–4, on the host, on those bytes
    tools/capture_accept.py <name>          promote a host result to the regression baseline

## expect_origin — what a passing replay proves

| value | the expectations are | agreement means |
|---|---|---|
| `board` | the numbers the board produced from these exact words | host and target agree on identical bytes |
| `golden` | the research pipeline's reference for a recorded excerpt | the C port still reproduces the reference |
| `host` | a previously accepted host result | nothing changed since it was accepted |

A bundle carrying no `expect_*` keys is still replayed; its diagnostics are printed and written
to `<name>.observed.json` (gitignored) for inspection.

## chain_digest

The replay refuses a bundle whose `chain_digest` does not match the build replaying it. The
same samples under different DSP constants give a different answer, and reporting that answer
as though it were the original measurement would be worse than not replaying at all. If you
changed a constant deliberately, the refusal is the reminder that every stored capture predates
the change.

### chain_profile/2 and `chain_digest_v1`

The digest is tagged by schema. `chain_profile/1` also hashed `excitation_pulse_ms`. Schema 2 moved
excitation into its own excitation profile, with its own `excitation_digest`, so that tuning how a
spoke is struck no longer invalidates DSP evidence captured under the same chain. The three bundles
below predate that split. Their `chain_digest` was rewritten to the `/2` value, and the digest they
were actually captured under is kept, unchanged, as `chain_digest_v1`.

That rewrite is a derivation, not a relabel:

- At the commit before the split (`551df73`) the replay passed on all three. That proves each
  stored `/1` digest matched the fixture chain profile's values.
- The split changed no DSP or capture field. It only removed `excitation_pulse_ms` and changed the
  tag.
- `/2` is therefore computed from the very values these words were analysed under.

Board captures from firmware after the split carry `/2` natively, plus `excitation_digest`, under
capture schema `truing.acoustic.capture/2`. `capture_fetch.py`, `capture_wav.py` and the replay
harness accept `/2` and `/1`.

**Not yet evidenced end to end:** no `/2` bundle is checked in. The first one comes from the
on-target B1 check, and until then `/2` is exercised by code review and T2 only.

`golden_ts03_e2` is not a board capture — it is the `ts03_e2` campaign excerpt in bundle form,
so the replay path is exercised on every clone rather than only on a bench with hardware
attached.
