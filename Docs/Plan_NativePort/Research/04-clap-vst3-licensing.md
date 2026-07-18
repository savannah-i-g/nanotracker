# Research — VST3 Relicensing and Project License

## Findings

| Fact | Value |
| --- | --- |
| VST3 SDK license (current) | **MIT** — confirmed on Steinberg's own developer portal |
| Change date | November 2025 (Steinberg relicensed VST3; ASIO became GPL-compatible in the same move) |
| Prior state | GPLv3-or-proprietary dual license (the basis of the plan's original license reasoning) |
| Migration pressure | none — existing products may stay on old terms; new integrations simply use MIT |
| Host/plugin mixing | MIT is compatible with everything; a permissive host may load proprietary plugins freely |

## Plan implications

This invalidates the *premise* of locked decision 1 ("GPLv3 from day
one" was chosen because linking the VST3 SDK forced GPLv3). With VST3
under MIT:

- The project license is a free identity choice: GPLv3 (copyleft —
  derivatives must stay open) and MIT/zlib (maximum adoption/reuse) are
  both fully workable with CLAP + VST3 hosting.
- CLAP-first ordering is unaffected — it was justified by hosting
  simplicity, not only license, and stands
  ([05-clap-hosting.md](05-clap-hosting.md)).
- The `include/ntp/` headers stay permissive in every scenario.

Decision re-put to the project owner rather than silently changed —
recorded in [00-index.md](00-index.md) and folded into
[../08-external-plugins.md](../08-external-plugins.md) once answered.

## Sources

- [VST 3 Developer Portal — Licensing FAQ (states MIT)](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Licensing.html)
- [Libre Arts — VST3 becomes open-source, ASIO goes GPL-compatible (Nov 2025)](https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/)
- [CDM — Open Steinberg: VST3 and ASIO SDKs now have open source licenses](https://cdm.link/open-steinberg-vst3-and-asio/)
- [vst3sdk repository](https://github.com/steinbergmedia/vst3sdk)
