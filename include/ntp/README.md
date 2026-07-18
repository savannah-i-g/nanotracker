# include/ntp — NanoTracker Plugin Interface

Public headers for the NanoTracker native plugin format (NTP). Licensed
MIT with a plugin exception (`../../LICENSES/NTP-MIT.txt`) — deliberately
more permissive than the GPLv3 application so plugin authors and host
implementers are never required to adopt the GPL.

Contents land with the NTP stage
(`../../Docs/Plan_NativePort/07-plugins-ntp.md`): the manifest schema as
C structs and, post-v1, the `native_stage` C ABI. Headers here are C
(not C++) — they define a stable interface boundary.
