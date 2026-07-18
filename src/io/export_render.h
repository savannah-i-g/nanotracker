// io/export_render — offline render: a sample-clocked run of the same
// engine the device drives (no ScriptProcessorNode-style capture
// workaround — fix #7). The project is serialised to FTRK bytes and
// loaded into a fresh offline session, so what exports is exactly what
// persists; rendering advances the transport block by block through the
// requested order range (song wrap by default), then keeps a release
// tail while voices ring out.
//
// Encoders: WAV writes PCM16/PCM24/float32 directly (LIST/INFO
// metadata chunk); OGG uses libvorbis (VBR quality, Vorbis comments);
// MP3 dlopens the system libmp3lame at run time (LGPL dynamic linking,
// no build dependency) and reports cleanly when the library is absent —
// its optional id3tag_* symbols carry the ID3v2 tags and are silently
// skipped when the runtime lame lacks them.
#pragma once

#include "engine/tracker_types.h"
#include "io/export_post.h"
#include "io/ftrk_writer.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::io {

enum class ExportFormat : std::uint8_t { kWav, kOgg, kMp3 };

enum class WavDepth : std::uint8_t { kPcm16, kPcm24, kFloat32 };

// Text tags, embedded per container: WAV LIST/INFO (INAM/IART/IPRD/
// ICRD/ICMT), Vorbis comments (TITLE/ARTIST/ALBUM/DATE/COMMENT), ID3v2
// via lame. Empty fields are omitted everywhere.
struct ExportMetadata {
    std::string title;
    std::string artist;
    std::string album;
    std::string date;
    std::string comment;
};

struct ExportOptions {
    ExportFormat format = ExportFormat::kWav;
    WavDepth wav_depth = WavDepth::kPcm16;
    float ogg_quality = 0.5F;   // vorbis VBR quality, -0.1 .. 1.0
    int mp3_bitrate_kbps = 192; // CBR bitrate
    int mp3_quality = 2;        // lame quality, 0 (best) .. 9 (fastest)
    std::uint32_t sample_rate = 48000;
    double tail_seconds = 2.0; // keeps reverb/release tails
    // Order-list range, inclusive on both ends; end_order < 0 means the
    // last position. The offline transport seeds order_pos at
    // start_order and parks at the range end (voices ring into the
    // tail) instead of wrapping back into the song.
    int start_order = 0;
    int end_order = -1;
    // Stems: a non-zero mask renders one pass per set bit (bit N =
    // channel N) into "<base>.chNN.<ext>" beside `path` (NN = channel
    // number, 1-based); zero renders the single mix to `path` itself.
    // `stem_zip` bundles the stem files into "<base>.stems.zip" and
    // removes the loose files.
    std::uint32_t stem_mask = 0;
    bool stem_zip = false;
    ExportPostOptions post; // fades + normalization (io/export_post.h)
    ExportMetadata metadata;
};

struct ExportResult {
    bool ok = false;
    std::string error;
    std::uint64_t frames = 0; // per rendered pass (stems all match)
    double seconds = 0.0;
    std::vector<std::filesystem::path> files; // everything written
};

// Renders `project` (with its write extras, so plugins and workspace
// state participate exactly as in a load) and encodes to `path` (or the
// stem files derived from it) per `options`.
ExportResult export_project(const engine::TrackerProject& project, const FtrkWriteExtras& extras,
                            const std::filesystem::path& path, const ExportOptions& options);

// Compatibility entry point (CLI --export, session export): format at
// `rate` with default options.
ExportResult export_project(const engine::TrackerProject& project, const FtrkWriteExtras& extras,
                            const std::filesystem::path& path, ExportFormat format,
                            std::uint32_t rate, double tail_seconds = 2.0);

// Minimal in-memory PCM16 WAV (classic 16-byte fmt, no metadata) from
// interleaved stereo — the same quantisation the file writer's PCM16
// branch applies. The destructive sample editor persists edited
// buffers through this so what FTRK stores is exactly what re-decodes.
std::vector<std::uint8_t> encode_wav_pcm16(const std::vector<float>& interleaved,
                                           std::uint32_t rate);

} // namespace nt::io
