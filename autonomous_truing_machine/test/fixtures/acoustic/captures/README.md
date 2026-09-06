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

`golden_ts03_e2` is not a board capture — it is the `ts03_e2` campaign excerpt in bundle form,
so the replay path is exercised on every clone rather than only on a bench with hardware
attached.
