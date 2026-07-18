# 02 — libopenmpt on Windows

Grounds Stage 13's MOD/XM/S3M/IT import dependency
(`../02-release-engineering.md`). The upstream build script is
bash-only, so Windows needs an acquisition decision.

## Headline facts

| Question | Answer |
| --- | --- |
| vcpkg port state | unmaintained for years; upstream maintainer calls it "in abysmal shape from the start" (vcpkg issue #18480) — **do not use** |
| Upstream prebuilts | yes: `lib.openmpt.org` ships Windows *development packages* — VS2022 builds for amd64 / x86 (and arm64), containing headers + import libs + DLL |
| Licence | BSD-3-Clause (whole project; `include/`/`contrib/` subtrees may differ but are not shipped in the dev package DLL) |
| Redistribution | permitted with licence text included — add to `LICENSES/` and `Docs/DEPENDENCIES.md` |

## Acquisition path (Stage 13 concrete)

- Windows CI + local Windows builds consume the upstream VS2022
  amd64 development package: download a pinned version URL in CMake
  (`file(DOWNLOAD)` guarded by hash, or a thin script step in the
  workflow), cache via `actions/cache`.
- Linux keeps the existing FetchContent/source path unchanged.
- The shipped Windows artifact bundles `libopenmpt.dll` + its licence
  text.
- Pin the exact upstream version in one place (CMake variable) so
  Linux-source and Windows-prebuilt stay on the same release.

## Plan implications

- `../02-release-engineering.md` originally said "upstream prebuilt
  or vcpkg" — vcpkg is now ruled out; upstream prebuilt is the
  locked path.
- Importer fixtures (S3M/IT, folded into Stage 13) get exercised on
  the Windows runner against the prebuilt DLL from day one — exactly
  the endianness/path bug-farm coverage the fold-in wanted.

## Sources

- https://github.com/microsoft/vcpkg/issues/18480
- https://lib.openmpt.org/libopenmpt/download/
- https://lib.openmpt.org/libopenmpt/license/
- https://vcpkg.io/en/package/libopenmpt.html
- https://github.com/OpenMPT/openmpt/
