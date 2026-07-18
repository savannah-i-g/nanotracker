// ntp/ntp_stage_abi — the NTP native-stage C ABI, version 1.
//
// A `native_stage` node in an NTP manifest names a shared library
// shipped inside the plugin archive. Unlike every other NTP node, a
// native stage is compiled code the host executes — hosts surface that
// trust decision to the user and never load such an archive silently.
// The stage itself is DSP only: float32 stereo blocks in, float32
// stereo blocks out, block-rate parameters delivered by the host. UI
// stays declarative in the manifest; this ABI has no UI, file, or
// host-service access.
//
// ── Archive layout ───────────────────────────────────────────────────
// Binaries live under per-platform subdirectories (the CLAP
// multi-platform bundle model), one per platform the author ships:
//
//   binaries/<platform-tag>/<stage>.<ext>
//
//   binaries/linux-x86_64/drive.so
//   binaries/windows-x86_64/drive.dll
//
// <stage> is the manifest node's "stage" value — a bare basename, no
// path separators. Version-1 platform tags: linux-x86_64 (.so) and
// windows-x86_64 (.dll); further tags (linux-arm64, macos-arm64,
// macos-x86_64 with .dylib, …) are additive. A host refuses an archive
// that carries no binary for its own platform, and the refusal lists
// the tags the archive does provide.
//
// ── Entry point ──────────────────────────────────────────────────────
// The library exports exactly one symbol:
//
//   const ntp_stage_interface_t* ntp_stage_entry(void);
//
// The returned struct — and everything reachable from it (descriptor,
// parameter table, strings) — must stay valid until the library is
// unloaded; static storage is the expected implementation. The host
// checks `abi_version` before reading ANY other field and refuses a
// mismatch with a message naming both versions. `abi_version` is, and
// will remain in every future version, the first member of
// ntp_stage_interface_t.
//
// ── Threading ────────────────────────────────────────────────────────
//   [main]  create, destroy, state_size, state_save, state_load
//   [audio] process, reset — real-time rules: no allocation, no
//           locks, no blocking, no I/O, no unbounded work.
// Per instance the host serialises every callback: no two calls on the
// same instance ever run concurrently (state functions are only called
// while the instance is not being processed). Different instances may
// be driven from different threads at once, so anything shared between
// instances must be immutable after ntp_stage_entry returns.
//
// ── Versioning ───────────────────────────────────────────────────────
// Growth is additive. Reserved fields exist so a future version can
// define new members without changing any struct size: version-1
// stages must zero them, and hosts must ignore them (never validate
// reserved space — a newer stage using it must still load in an older
// host). Any change a version-1 host could misread bumps
// NTP_STAGE_ABI_VERSION instead.
//
// This header is licensed MIT with the NTP plugin exception (see
// LICENSES/NTP-MIT.txt) so stage authors can embed it freely. It is
// plain C (C99), compiles as C or C++, and depends on stdint.h and
// stdbool.h only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTBEGIN(modernize-use-using, cppcoreguidelines-macro-usage) —
// this is a C header: typedef and #define ARE the language here.

// The ABI this header describes. A stage reports the version it was
// built against in ntp_stage_interface_t.abi_version; hosts refuse any
// value they do not implement.
#define NTP_STAGE_ABI_VERSION 1

// The one exported symbol, by name (host side: dlsym/GetProcAddress).
#define NTP_STAGE_ENTRY_SYMBOL "ntp_stage_entry"

#if defined(_WIN32)
#define NTP_STAGE_EXPORT __declspec(dllexport)
#else
#define NTP_STAGE_EXPORT __attribute__((visibility("default")))
#endif

// ── Parameters ───────────────────────────────────────────────────────

// One block-rate parameter. The host resolves each parameter through
// its own machinery (manifest defaults, host params, mod routes,
// audio-rate connections collapsed to block rate) and delivers one
// float per parameter per process() call, clamped to
// [min_value, max_value]. The stage never stores parameter state the
// host owns — the current values arrive with every block.
typedef struct ntp_stage_param_info {
    // Stable machine key, unique within the stage. Manifests address
    // the parameter as "<nodeId>.<id>" (host params, mod routes,
    // toParam connections), so renaming an id breaks existing plugins.
    // Both strings must be non-null and outlive the loaded library.
    const char* id;
    const char* name; // display label; hosts fall back to `id` when empty

    double min_value; // must be < max_value
    double max_value;
    double default_value; // must lie within [min_value, max_value]

    // No flag bits are defined in version 1: stages write 0; hosts
    // ignore bits they do not know (future bits are additive).
    uint32_t flags;

    uint32_t reserved0;   // v1: write 0
    uint64_t reserved[2]; // v1: write 0
} ntp_stage_param_info_t;

// ── Descriptor ───────────────────────────────────────────────────────

typedef struct ntp_stage_descriptor {
    const char* name; // display name; non-null, non-empty

    // Parameter table: `param_count` entries in `params`; a
    // parameterless stage uses 0 / NULL. Hosts may bound the table —
    // NanoTracker version 1 accepts up to 12 parameters and refuses
    // beyond (raising a host bound later is additive).
    uint32_t param_count;
    uint32_t reserved0; // v1: write 0
    const ntp_stage_param_info_t* params;

    void* reserved[4]; // v1: write NULL
} ntp_stage_descriptor_t;

// ── Interface ────────────────────────────────────────────────────────

typedef struct ntp_stage_interface {
    // Checked by the host before any other field is read. Forever the
    // first member.
    uint32_t abi_version; // = NTP_STAGE_ABI_VERSION
    uint32_t reserved0;   // v1: write 0

    const ntp_stage_descriptor_t* descriptor; // non-null

    // [main] Creates one independent stage instance, or returns NULL
    // on failure (hosts probe create/destroy once at load time and
    // refuse the archive when the probe fails). `max_block_frames`
    // bounds `frames` in every later process() call on the instance.
    // Required.
    void* (*create)(uint32_t sample_rate, uint32_t max_block_frames);

    // [main] Destroys an instance. Never called while any other
    // callback on the instance is running. Required.
    void (*destroy)(void* instance);

    // [audio] Renders `frames` (1..max_block_frames) samples of
    // non-interleaved stereo. in_l/in_r hold the summed input signal
    // (all zeros when nothing is connected — generators simply ignore
    // them); out_l/out_r must be fully written, the host does not
    // pre-zero them. Input and output buffers never alias. `params`
    // holds one value per descriptor parameter, in table order,
    // clamped to each parameter's range and constant for the call;
    // NULL when param_count is 0. Real-time rules apply. Required.
    void (*process)(void* instance, const float* in_l, const float* in_r, float* out_l,
                    float* out_r, uint32_t frames, const float* params);

    // ── State (optional, all-or-none) ────────────────────────────────
    // Opaque chunks the host stores and replays verbatim. state_size
    // reports the current serialised size; the host immediately
    // follows with state_save into a buffer of exactly that size, with
    // no intervening calls on the instance. state_size 0 means
    // "nothing to save" and suppresses the save; the host never calls
    // state_load with a zero-size chunk. state_save/state_load return
    // false on failure. Chunks may travel between machines — use
    // fixed-width, fixed-endian encoding when cross-platform restores
    // should work. All three NULL when the stage has no state worth
    // persisting.
    uint32_t (*state_size)(void* instance);
    bool (*state_save)(void* instance, void* buffer, uint32_t size);
    bool (*state_load)(void* instance, const void* buffer, uint32_t size);

    // [audio] Returns the instance to its post-create condition:
    // clear delay lines, tails, phases. Parameter values are host-
    // owned and arrive again with the next process(), so they need no
    // handling here. Real-time rules apply. NULL = the host treats
    // reset as a no-op (fine for stages with no inter-block state).
    void (*reset)(void* instance);

    void* reserved[8]; // v1: write NULL
} ntp_stage_interface_t;

// Implemented by the stage, resolved by the host through
// NTP_STAGE_ENTRY_SYMBOL. Must return the same pointer every call; the
// host may call it more than once.
NTP_STAGE_EXPORT const ntp_stage_interface_t* ntp_stage_entry(void);

// NOLINTEND(modernize-use-using, cppcoreguidelines-macro-usage)

#ifdef __cplusplus
} // extern "C"
#endif
