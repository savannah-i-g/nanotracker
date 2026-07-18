# Research — libopenmpt

## Findings

| Fact | Value |
| --- | --- |
| Current stable | 0.8 (0.8.0 released 2025-05-31) |
| License | BSD-3 |
| Design position | "pure module playback library" (project FAQ) |
| Sample/instrument extraction | NOT exposed by the public API — no sample PCM, limited sample/instrument metadata; external-sample support declined; third parties fork to add extraction (e.g. openmpt-lspx) |
| Pattern read API | `openmpt::module::get_pattern_row_channel_command` + format/highlight variants exist (C and C++ APIs) |
| Interactive extension | `openmpt::ext::interactive` — per-channel mute/solo, tempo/pitch factors, seeking |
| Pattern visualisation | `openmpt::ext::pattern_vis` available for display metadata |

## Plan implications

- **Confirms the split design** in
  [../05-module-playback.md](../05-module-playback.md): libopenmpt is
  the faithful-playback node; the four hand-written importers get
  ported because import needs sample PCM/loops/envelopes that
  libopenmpt will not provide. The stress-test's claim is now verified
  against the project's own FAQ and tracker.
- Pattern-read + `pattern_vis` APIs are sufficient for the
  test-oracle role (cross-checking imported pattern data) and for
  showing live order/row position on the Module Player node.
- Pin 0.8.x at Stage 0 (pkg-config first, vendored fallback —
  [../02-architecture.md](../02-architecture.md)).

## Sources

- [libopenmpt 0.8.0 release](https://lib.openmpt.org/libopenmpt/2025/05/31/release-0.8.0/)
- [libopenmpt FAQ (pure playback library)](https://lib.openmpt.org/libopenmpt/faq/)
- [openmpt::module class reference](https://lib.openmpt.org/doc/classopenmpt_1_1module.html)
- [Issue 826 — richer metadata not exposed](https://bugs.openmpt.org/view.php?id=826)
- [Issue 925 — external samples unsupported](https://bugs.openmpt.org/view.php?id=925)
- [openmpt-lspx fork (extraction requires forking)](https://github.com/dansalvato/openmpt-lspx)
