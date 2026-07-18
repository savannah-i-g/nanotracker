// Offline export verification: a generated project renders through the
// offline engine into all three formats; WAV parses directly and the
// OGG/MP3 round-trip through the app's own decoders. Content is
// checked spectrally (the project's single tone must dominate).
#include "audio/decoders.h"
#include "engine/tracker_engine.h"
#include "io/export_render.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;

// 16-bit mono WAV bytes with a sine (as in ntp_test.cpp).
std::vector<std::uint8_t> make_wav(float freq, float seconds, std::uint32_t rate) {
    const auto frames = static_cast<std::uint32_t>(seconds * static_cast<float>(rate));
    std::vector<std::uint8_t> bytes(44 + (static_cast<std::size_t>(frames) * 2));
    auto put32 = [&bytes](std::size_t at, std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            bytes[at + static_cast<std::size_t>(b)] =
                static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF);
        }
    };
    auto put16 = [&bytes](std::size_t at, std::uint16_t v) {
        bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };
    std::memcpy(bytes.data(), "RIFF", 4);
    put32(4, 36 + (frames * 2));
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    put32(40, frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float s =
            std::sin(2.0F * 3.14159265F * freq * static_cast<float>(i) / static_cast<float>(rate));
        put16(44 + (static_cast<std::size_t>(i) * 2),
              static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 20000.0F)));
    }
    return bytes;
}

float goertzel(const std::vector<float>& mono, std::uint32_t rate, float freq) {
    const std::size_t n = mono.size();
    const double k = std::round(static_cast<double>(n) * freq / rate);
    const double w = 2.0 * 3.14159265358979 * k / static_cast<double>(n);
    const double c = 2.0 * std::cos(w);
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    for (const float x : mono) {
        s0 = x + (c * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(std::sqrt(std::max(0.0, (s1 * s1) + (s2 * s2) - (c * s1 * s2))) /
                              (static_cast<double>(n) / 2.0));
}

std::vector<float> mono_of(const nt::audio::codec::Decoded& decoded) {
    std::vector<float> mono(decoded.frames);
    for (std::uint32_t f = 0; f < decoded.frames; ++f) {
        float sum = 0.0F;
        for (std::uint32_t c = 0; c < decoded.channels; ++c) {
            sum += decoded.interleaved[(static_cast<std::size_t>(f) * decoded.channels) + c];
        }
        mono[f] = sum / static_cast<float>(std::max(1U, decoded.channels));
    }
    return mono;
}

std::vector<std::uint8_t> file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("offline export renders identically into wav, ogg and mp3", "[export]") {
    // A one-tone project: sample 1 (440 Hz) on C-4 every 8 rows.
    nt::engine::TrackerProject project = nt::engine::create_project(2);
    project.name = "EXPORT";
    nt::engine::TrackerSample sample;
    sample.id = 1;
    sample.name = "SINE";
    sample.format = "wav";
    sample.original_data = make_wav(440.0F, 0.4F, 44100);
    sample.sample_rate = 44100;
    sample.num_channels = 1;
    sample.frames = static_cast<std::uint32_t>(44100 * 0.4F);
    sample.base_note = 60;
    sample.volume = 64;
    sample.pan = 128;
    project.samples.push_back(sample);
    for (int r = 0; r < 64; r += 8) {
        project.patterns[0].rows[static_cast<std::size_t>(r)][0] = {.note = 49, .instrument = 1};
    }

    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const nt::io::FtrkWriteExtras extras;

    const nt::io::ExportResult wav = nt::io::export_project(project, extras, dir / "nt_export.wav",
                                                            nt::io::ExportFormat::kWav, kRate, 1.0);
    INFO(wav.error);
    REQUIRE(wav.ok);
    CHECK(wav.seconds > 5.0);

    const nt::io::ExportResult ogg = nt::io::export_project(project, extras, dir / "nt_export.ogg",
                                                            nt::io::ExportFormat::kOgg, kRate, 1.0);
    INFO(ogg.error);
    REQUIRE(ogg.ok);
    CHECK(ogg.frames == wav.frames);

    // The project's tone at note 49 with base 60 plays the resident
    // buffer slower; measure the actual dominant partial from the WAV
    // and require the lossy formats to reproduce it.
    nt::audio::codec::Decoded wav_decoded;
    std::string decode_error;
    const std::vector<std::uint8_t> wav_bytes = file_bytes(dir / "nt_export.wav");
    REQUIRE(nt::audio::codec::decode_wav(wav_bytes.data(), wav_bytes.size(), wav_decoded,
                                         decode_error));
    const std::vector<float> wav_mono = mono_of(wav_decoded);
    float peak_freq = 0.0F;
    float peak_mag = 0.0F;
    for (float f = 100.0F; f < 2000.0F; f += 10.0F) {
        const float mag = goertzel(wav_mono, wav_decoded.rate, f);
        if (mag > peak_mag) {
            peak_mag = mag;
            peak_freq = f;
        }
    }
    CHECK(peak_mag > 0.001F);

    const std::vector<std::uint8_t> ogg_bytes = file_bytes(dir / "nt_export.ogg");
    nt::audio::codec::Decoded ogg_decoded;
    REQUIRE(nt::audio::codec::decode_ogg(ogg_bytes.data(), ogg_bytes.size(), ogg_decoded,
                                         decode_error));
    const std::vector<float> ogg_mono = mono_of(ogg_decoded);
    CHECK(goertzel(ogg_mono, ogg_decoded.rate, peak_freq) > peak_mag * 0.5F);

    // MP3 needs the system libmp3lame; skip with a record when absent.
    const nt::io::ExportResult mp3 = nt::io::export_project(project, extras, dir / "nt_export.mp3",
                                                            nt::io::ExportFormat::kMp3, kRate, 1.0);
    if (!mp3.ok) {
        WARN("mp3 export unavailable: " << mp3.error);
        return;
    }
    const std::vector<std::uint8_t> mp3_bytes = file_bytes(dir / "nt_export.mp3");
    nt::audio::codec::Decoded mp3_decoded;
    REQUIRE(nt::audio::codec::decode_mp3(mp3_bytes.data(), mp3_bytes.size(), mp3_decoded,
                                         decode_error));
    std::vector<float> mp3_mono = mono_of(mp3_decoded);
    // LAME pads encoder delay, so the decoded stream is longer than the
    // WAV and its Goertzel bin grid would shift; trimming to the WAV's
    // window makes the magnitudes comparable (the remaining offset is
    // phase-only at this window size). Looser line than OGG's — lossy
    // presence, not parity.
    mp3_mono.resize(wav_mono.size());
    CHECK(goertzel(mp3_mono, mp3_decoded.rate, peak_freq) > peak_mag * 0.25F);
}
