# winget manifest (staged, not yet submitted)

Windows distribution is a portable zip; winget accepts zip packages
directly. This manifest set is prepared for submission to
`microsoft/winget-pkgs` **when the Windows build is promoted from
beta to GA** — promotion is gated on community confirmation (locked
decision 4, `Docs/Plan_PostV1/00-index.md`), so submission
deliberately waits.

To submit at promotion time: update `PackageVersion`, the
`InstallerUrl` (point at the GitHub release zip), and
`InstallerSha256`, then open a PR adding these files under
`manifests/s/SavannahGoring/NanoTracker/<version>/`.
