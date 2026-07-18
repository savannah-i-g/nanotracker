# 12 — Doctrine

Standing rules for how this codebase is built and maintained. These are
commitments, not aspirations; deviations get fixed or the doctrine gets
amended here, never silently.

## Code

- **One concern per file.** Every module carries a header comment
  stating its single responsibility and what it depends on. The web
  app's `Tracker.tsx` (3,599 LOC) is the named anti-pattern; nothing in
  this tree may grow into it.
- **Dependency direction is law** — the arrows in
  [02-architecture.md](02-architecture.md#module-decomposition). `engine/`
  and `rt/` depend on the standard library only. UI never touches audio
  state except through the command queue and snapshots.
- **RT discipline**: no allocation, locks, or syscalls on the audio
  thread; debug builds assert it. FTZ/DAZ on DSP threads.
- **Port semantics, not idioms.** The web app is the behavioural
  reference, not the implementation reference. JS patterns (singletons,
  stringly-typed registries, parallel legacy models) do not cross over.
- Style is mechanical: `.clang-format` and `.clang-tidy` in-tree from
  Stage 0; both clean before any stage closes.

## Documentation and comments

- Developer-first: comments explain structure, constraints, and
  invariants — the things the code cannot say. No workflow narration, no
  release-process language, no explanations of why a change is correct
  relative to a previous version. The source is written to be released.
- Format and protocol knowledge lives in `Docs/` (e.g.
  `ftrk-format.md`), not scattered in comments.

## Fixing beats retaining

Broken or patchy web behaviour is fixed, not ported
([01-context.md](01-context.md#fix-dont-retain-list)). Every deliberate
divergence is logged in `Docs/FIXES.md` with the web `file:line` it
replaces and the reason — the trail keeps divergence reviewable.

## Ledgers and logging

- `Docs/PROGRESS.md` — one entry per stage: what landed, verification
  results, backup filename. This is the reliable log the project brief
  asked for.
- `Docs/FIXES.md` — divergence ledger (above).
- `Docs/DEPENDENCIES.md` — every third-party dependency with pinned
  version and license; updated the moment a dep is added.
- `LICENSES/` — full license texts; program GPLv3, `include/ntp/` MIT
  with plugin exception.

## Backups

No git repository yet (user directive). Until one exists:

- `tools/backup.sh` produces
  `Backups/NanoTracker_<label>_<yyyy-mm-dd>.tar.gz` from `Output/`
  (excluding build directories).
- A backup is taken at every stage boundary and before any risky
  restructure. The backup filename is recorded in `PROGRESS.md`.
- When a git repo is introduced later, the ledger history seeds the
  initial commit messages.

## Stage exit checklist

A stage is closed only when all of these hold:

1. Stage exit criteria met ([14-roadmap.md](14-roadmap.md)).
2. Tests green, including golden vectors where applicable; ASan/UBSan
   run on the test suite.
3. clang-format/clang-tidy clean.
4. `PROGRESS.md` + `FIXES.md` + `DEPENDENCIES.md` updated.
5. Tar backup taken and named in `PROGRESS.md`.
