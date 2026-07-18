# include/ntp — NanoTracker Plugin Interface

Public headers for the NanoTracker native plugin format (NTP). Licensed
MIT with a plugin exception (`../../LICENSES/NTP-MIT.txt`) — deliberately
more permissive than the GPLv3 application so plugin authors and host
implementers are never required to adopt the GPL.

Contents:

- `ntp_manifest.h` — the NTP v1 manifest schema as plain C++ structs
  (`../../Docs/Plan_NativePort/07-plugins-ntp.md`), for hosts and
  tooling.
- `ntp_stage_abi.h` — the `native_stage` C ABI, version 1
  (`../../Docs/Plan_PostV1/10-native-stage-abi.md`): the frozen
  interface stage binaries build against. Plain C99; compiles as C or
  C++; growth is additive only.
