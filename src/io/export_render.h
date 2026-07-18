// io/export_render — offline render: a sample-clocked run of the same
// engine the device drives (no ScriptProcessorNode-style capture
// workaround — fix #7). The project is serialised to FTRK bytes and
// loaded into a fresh offline session, so what exports is exactly what
// persists; rendering advances the transport block by block until the
// order list wraps (or the safety cap), plus a release tail.
//
// Encoders: WAV writes PCM16 directly; OGG uses libvorbis; MP3 dlopens
// the system libmp3lame at run time (LGPL dynamic linking, no build
// dependency) and reports cleanly when the library is absent.
#pragma once

#include "engine/tracker_types.h"
#include "io/ftrk_writer.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::io {

enum class ExportFormat : std::uint8_t { kWav, kOgg, kMp3 };

struct ExportResult {
    bool ok = false;
    std::string error;
    std::uint64_t frames = 0;
    double seconds = 0.0;
};

// Renders `project` (with its write extras, so plugins and workspace
// state participate exactly as in a load) at `rate` and encodes to
// `path`. `tail_seconds` keeps reverb/release tails.
ExportResult export_project(const engine::TrackerProject& project, const FtrkWriteExtras& extras,
                            const std::filesystem::path& path, ExportFormat format,
                            std::uint32_t rate, double tail_seconds = 2.0);

} // namespace nt::io
