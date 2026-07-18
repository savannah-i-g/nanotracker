# Research — Dear ImGui Docking Branch

## Findings

| Fact | Value |
| --- | --- |
| Docking + multi-viewport home | dedicated `docking` branch (not master) |
| Release tagging | docking-specific tags since July 2023, e.g. `v1.92.0-docking` |
| Activity | 1.92.8 WIP on the docking branch as of April 2026 |
| Maturity | branch is long-lived, widely used in production, recommended by the maintainer for teams |

Multi-viewport (OS-window-per-ImGui-window) ships in the same branch;
master carries only minimal viewport infrastructure.

## Plan implications

- Pin the newest `v1.92.x-docking` release tag via FetchContent at
  Stage 0 — reproducible builds, no branch-head drift. Bump
  deliberately per stage as needed, recorded in
  `../../DEPENDENCIES.md`.
- Confirms the locked windowing decision: docking branch now, docking
  enabled for editor surfaces only, multi-viewport a post-v1 flag-flip
  ([../09-windows-ui.md](../09-windows-ui.md)).

## Sources

- [Docking — ocornut/imgui wiki](https://github.com/ocornut/imgui/wiki/Docking)
- [Multi-Viewports — ocornut/imgui wiki](https://github.com/ocornut/imgui/wiki/Multi-Viewports)
- [Docking branch issue #2109](https://github.com/ocornut/imgui/issues/2109)
- [Issue #9384 (April 2026, shows 1.92.8 WIP on docking)](https://github.com/ocornut/imgui/issues/9384)
