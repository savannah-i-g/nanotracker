# 06 — Packaging Norms (2026)

Grounds Stage 21 GA packaging (`../10-native-stage-abi.md`) and the
Stage 13 beta artifact shape.

## Linux: AppImage vs Flatpak

| Axis | AppImage | Flatpak |
| --- | --- | --- |
| Audio path | direct ALSA/PipeWire, no sandbox | PipeWire's sandbox integration is first-class (designed for it) |
| Plugin hosting | host filesystem as-is: system CLAP/VST3 dirs, user dlopen | sandbox blocks system plugin dirs; needs extension points (`org.freedesktop.LinuxAudio.Plugins`) and portals |
| Distribution | single file, no infrastructure | Flathub account + manifest review |
| 2026 sentiment | "maximum portability" niche | desktop favourite for *sandboxable* apps |

**Decision: AppImage primary for GA.** NanoTracker is a plugin host —
it dlopens system-installed CLAP/VST3 plugins and arbitrary
`native_stage` libraries from NTP archives; the Flatpak sandbox
either blocks that (broken product) or gets punched full of holes
(sandbox theatre). AppImage preserves host semantics with one-file
distribution. Flatpak is parked as a community-driven follow-up if
demand appears; PipeWire's Flatpak-friendly audio path means the
audio side would not be the blocker.

## Windows: zip vs installer vs winget

- winget (the built-in package manager) accepts **zip packages**
  directly since 1.4, alongside EXE/MSI/Inno/NSIS.
- Manifests are community-submitted to `microsoft/winget-pkgs`;
  anyone can contribute one for a released app.

**Decision: portable zip for beta (Stage 13) and GA (Stage 21); a
winget manifest submitted at GA pointing at the release zip.** No
installer unless community feedback demands one — a tracker's users
are comfortable with portable apps, and zip keeps the artifact
identical between CI output and release.

## Plan implications

- Stage 13's beta artifact shape (zip with exe + OpenAL Soft DLL +
  libopenmpt DLL + licence texts) is already GA-shaped; Stage 21
  packaging work is then AppImage tooling + winget manifest + release
  notes, not a new Windows artifact.

## Sources

- https://sumguy.com/flatpak-vs-snap-vs-appimage/
- https://pipewire.org/
- https://www.linuxteck.com/pipewire-linux-audio-problem-solved/
- https://flatpak.org/
- https://winaero.com/winget-now-supports-installing-apps-from-zip-archives/
- https://pureinfotech.com/windows-package-manager-1-4-winget-zip-installs/
- https://github.com/microsoft/winget-pkgs
- https://learn.microsoft.com/en-us/windows/package-manager/
