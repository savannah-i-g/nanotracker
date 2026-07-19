// io/ftrk_reader — reads .ftrk project files, versions 1 through 15.
// Normative reference for the format: the web serializer
// (Source/.../src/lib/trackerSerializer.ts); the native writer and the
// standalone spec Docs/ftrk-format.md derive from the same layout.
//
// Read philosophy (mirrors the web app): the core sections (header,
// order list, patterns, samples) fail loudly on corruption; the
// appended blocks (FXMX, INTB, WPBR, PLGB, SEQB, POVR, PPRS, XPLG,
// XINS) are individually tolerant — a damaged block is reported and
// skipped, the song still loads.
#pragma once

#include "engine/tracker_types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nt::io {

// FX mixer channel strips as stored in FXMX. The engine only consumes
// automation patterns; strip definitions hand over to the FX mixer
// stage intact.
struct FtrkFxModule {
    std::string module_id;
    bool enabled = true;
    std::vector<std::pair<std::string, float>> params;
};

struct FtrkFxChannel {
    std::string name;
    int volume = 100;
    int pan = 0;
    std::string color;
    bool enabled = true;
    std::vector<FtrkFxModule> modules;
    std::vector<float> tracker_sends;
};

struct FtrkBundledPlugin {
    std::string plugin_id;
    std::vector<std::uint8_t> archive; // original .ntins ZIP bytes
};

// One POVR slot override: a user sample dropped into a plugin
// instance's user-assignable slot. `bytes` is the original encoded
// file; `hash` its "sha256:<hex>" content hash (the block's dedup and
// lookup key). sample_rate/channels/duration describe the source as
// decoded when the override was made — display metadata, not decode
// authority.
struct FtrkPovrOverride {
    std::string instance_id; // plugin instance workspace id
    std::string slot_id;
    std::string hash;
    std::string name;
    std::uint32_t sample_rate = 0;
    std::uint8_t channels = 0;
    float duration = 0.0F;
    std::vector<std::uint8_t> bytes;
};

// One external plugin instance's persisted state (XPLG entry, v14).
struct FtrkExternalPlugin {
    std::string workspace_id;
    std::uint8_t kind = 0; // 0 = CLAP, 1 = VST3
    std::string plugin_id; // CLAP id / VST3 class UID
    std::string library_path;
    std::vector<std::uint8_t> state;
    // Parameter snapshot for degraded restore when the plugin is
    // missing (CLAP raw values / VST3 normalized).
    std::vector<std::pair<std::uint32_t, double>> params;
    // Out-of-process bridging opt-in (Stage 29e). Stored as an additive
    // trailing flag run after the record list (see io/ftrk_writer XPLG),
    // so a file written before S29e — with no flag run — reads back as
    // false = in-process, and a reader that predates the flag skips the
    // trailing run untouched. Persists the per-node user choice; the
    // bridged plugin's own state travels in `state` (the bridge's shadow).
    bool bridged = false;
};

// Per-instance NTP state (XINS entry, v15). One record per plugin
// instance that carries envelope-stage overrides and/or native_stage
// ABI state chunks — the state WPBR's paramSnapshot cannot express.
// `env_stages` are per-instance envelope-editor edits (node id + stage
// index + the overridden target/time); `stage_chunks` are opaque
// native_stage chunks keyed by node id (ntp_stage_abi.h state_save
// output). Applied to live instances after instantiation; unresolved
// records round-trip untouched.
struct FtrkInstanceEnvStage {
    std::string node_id;
    int stage_index = 0;
    double target = 0.0;
    double time = 0.0;
};

struct FtrkInstanceStageChunk {
    std::string node_id;
    std::vector<std::uint8_t> chunk;
};

struct FtrkInstanceState {
    std::string workspace_id;
    std::vector<FtrkInstanceEnvStage> env_stages;
    std::vector<FtrkInstanceStageChunk> stage_chunks;
};

// Everything in the file that is not (yet) part of the engine project:
// carried so later stages and the writer can round-trip it.
struct FtrkExtras {
    int version = 0;
    std::vector<FtrkFxChannel> fx_channels;
    std::string workspace_json;             // WPBR payload (patchbay state)
    std::vector<FtrkBundledPlugin> plugins; // PLGB
    // POVR (v12+): block version 1 parses into `povr_overrides`
    // (dedup'd bytes re-expanded per entry). An unknown block version
    // or an internally inconsistent block lands verbatim in
    // `povr_raw` instead, so the writer can round-trip what it cannot
    // interpret. Exactly one of the two is populated.
    std::vector<FtrkPovrOverride> povr_overrides;
    std::vector<std::uint8_t> povr_raw;
    std::string pprs_json;                          // PPRS payload (v13+)
    std::vector<FtrkExternalPlugin> external;       // XPLG entries (v14+)
    std::vector<FtrkInstanceState> instance_states; // XINS entries (v15+)
    std::vector<std::string> warnings;              // skipped-block reports
};

struct FtrkReadResult {
    engine::TrackerProject project;
    FtrkExtras extras;
};

// Parses a .ftrk buffer. Returns std::nullopt with `error` set on
// unreadable core data (bad magic, unsupported version, truncation).
std::optional<FtrkReadResult> read_ftrk(const std::uint8_t* data, std::size_t size,
                                        std::string& error);

std::optional<FtrkReadResult> read_ftrk_file(const std::filesystem::path& path, std::string& error);

} // namespace nt::io
