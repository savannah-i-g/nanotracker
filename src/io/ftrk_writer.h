// io/ftrk_writer — writes .ftrk version 14. The layout is the web
// serializer's v13 (normative spec: Docs/ftrk-format.md) plus one v14
// addition: the reserved header region grows from 31 to 35 bytes to
// carry an eighth block offset, XPLG — external (CLAP/VST3) plugin
// state chunks with parameter snapshots for degraded restore when a
// plugin is missing. Pre-v14 readers refuse the version loudly, which
// is the intended failure mode for files that need features they
// cannot represent.
#pragma once

#include "engine/tracker_types.h"
#include "io/ftrk_reader.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::io {

// Everything the writer needs beyond the engine project. The session
// assembles this from its live state; POVR passes through verbatim.
struct FtrkWriteExtras {
    std::string workspace_json; // WPBR payload
    std::vector<FtrkBundledPlugin> plugins;
    std::string pprs_json;
    std::vector<std::uint8_t> povr_raw;
    std::vector<FtrkExternalPlugin> external;
};

[[nodiscard]] std::vector<std::uint8_t> write_ftrk(const engine::TrackerProject& project,
                                                   const FtrkWriteExtras& extras);

bool write_ftrk_file(const std::filesystem::path& path, const engine::TrackerProject& project,
                     const FtrkWriteExtras& extras, std::string& error);

} // namespace nt::io
