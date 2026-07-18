# Post-v1 Plan — Index

The second cycle: from "feature-complete on one Linux machine"
(v1, all 12 stages closed 2026-07-17) to "released on both platforms,
in the open, with the recorded debts paid". Companion to
`../Plan_NativePort/` (v1 design docs, still normative for the
architecture) and `../PROGRESS.md` (stage log, shared between cycles).

## Read order

1. `01-context.md` — why this cycle exists; the deferral inventory
   and the parity audit that ground it
2. `02-release-engineering.md` — Stage 13 spine: repo, CI, the
   Windows seam, publish scrub
3. `03-editing-depth.md` — Stage 14: pattern + piano-roll tools
4. `04-export-suite.md` — Stage 15: export options + post-processing
5. `05-sample-lifecycle.md` — Stage 16: retirement + destructive ops
6. `06-midi-completion.md` — Stage 17: cable transport, record,
   effect→MIDI, CV→params
7. `07-local-api.md` — Stage 18: native remote control
8. `08-sampler-platform.md` — Stage 19: POVR slots + sliceMap
9. `09-plugin-platform.md` — Stage 20: partitioned FFT, sprites,
   envelopes, VST3 editors
10. `10-native-stage-abi.md` — Stage 21: the C-ABI + GA release
11. `11-roadmap.md` — stage table, dependencies, risk centers
12. `Research/` — web-research findings with citations (Burst 2)

## Decisions locked (Savannah, 2026-07-18)

1. **Opening move: repo + CI + Windows beta first.** The v1 decision
   "Windows released shortly after v1" is re-ratified as *beta via CI
   shortly after v1, GA at the final stage*. CI is the Windows
   toolchain; no local Windows machine exists or is planned.
2. **Public from the start.** git init immediately; publish scrub;
   the repo goes public at the start of the cycle with CI on both
   platforms. All post-v1 work happens in the open (GPLv3).
3. **The Local API is ported this cycle** as its own stage
   (WebSocket), fixing the web fix-list #5 defects in the port.
4. **No Windows hardware.** The Windows build stays labeled beta
   until community testers confirm it; GA promotion is gated on
   external reports.

Standing directives carried from v1: developer-first comments, clean
codebase first-class, fix-don't-retain with `FIXES.md` logging, no
timescales, stage-close ritual — now: verify → ledgers → **git tag +
tar backup**.

## Parked (recorded, not staged)

Out-of-process plugin bridging, multi-viewport patchbay, additional
AudioDevice backends, help-manual depth (the site carries docs),
project browser / asset manager windows, history panel, OpenAL
buffer-queue fallback (dissolved by shipping OpenAL Soft; revisit
only if the Windows beta reports callback problems).
