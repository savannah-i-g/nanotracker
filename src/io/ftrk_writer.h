// io/ftrk_writer — writes .ftrk version 15. The layout is the web
// serializer's v13 (normative spec: Docs/ftrk-format.md) plus two
// native additions: v14's XPLG (external CLAP/VST3 plugin state chunks
// with parameter snapshots) and v15's XINS — per-instance NTP state
// (envelope-stage overrides + native_stage ABI chunks). The reserved
// header region grew from 31 to 35 (v14) to 39 bytes (v15) to carry the
// ninth block offset. Pre-v15 readers refuse the version loudly, which
// is the intended failure mode for files that need features they cannot
// represent.
#pragma once

#include "engine/tracker_types.h"
#include "io/ftrk_reader.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::io {

// Everything the writer needs beyond the engine project. The session
// assembles this from its live state. POVR: `povr` entries serialise
// into the web-compatible block v1 (grouped by instance id in
// first-appearance order, each unique hash's bytes emitted once);
// when `povr` is empty a non-empty `povr_raw` — a block the reader
// could not interpret — passes through verbatim instead. Entries win
// over raw when both are set (live state beats an uninterpretable
// carry).
struct FtrkWriteExtras {
    std::string workspace_json; // WPBR payload
    std::vector<FtrkBundledPlugin> plugins;
    std::string pprs_json;
    std::vector<FtrkPovrOverride> povr;
    std::vector<std::uint8_t> povr_raw;
    std::vector<FtrkExternalPlugin> external;
    std::vector<FtrkInstanceState> instance_states; // XINS (v15)
};

[[nodiscard]] std::vector<std::uint8_t> write_ftrk(const engine::TrackerProject& project,
                                                   const FtrkWriteExtras& extras);

bool write_ftrk_file(const std::filesystem::path& path, const engine::TrackerProject& project,
                     const FtrkWriteExtras& extras, std::string& error);

} // namespace nt::io
