// Stage 15 export suite verification: render options (order range,
// stems, zip bundling), WAV bit depths, OGG quality/comments, MP3
// knobs, post-processing (fades + peak/true-peak/LUFS normalise via
// libebur128) and metadata tags — all read back through the app's own
// decoders or raw container bytes.
#include "audio/decoders.h"
#include "engine/tracker_engine.h"
#include "io/export_post.h"
#include "io/export_render.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;

// 16-bit mono WAV bytes with a sine (as in export_test.cpp).
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

std::uint32_t u32_at(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

std::uint16_t u16_at(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(bytes[at]) |
                                      (static_cast<std::uint32_t>(bytes[at + 1]) << 8));
}

struct RiffChunk {
    std::size_t payload = 0; // byte offset of chunk payload
    std::uint32_t size = 0;
    bool found = false;
};

// Walks top-level RIFF chunks ("RIFF"+size+"WAVE" header assumed).
RiffChunk find_riff_chunk(const std::vector<std::uint8_t>& bytes, const char* id) {
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::uint32_t size = u32_at(bytes, pos + 4);
        if (std::memcmp(bytes.data() + pos, id, 4) == 0) {
            return {pos + 8, size, true};
        }
        pos += 8 + size + (size & 1U);
    }
    return {};
}

// INFO sub-chunk text (LIST payload begins with "INFO"); empty when
// the field is absent.
std::string info_field(const std::vector<std::uint8_t>& bytes, const RiffChunk& list,
                       const char* id) {
    std::size_t pos = list.payload + 4; // skip "INFO"
    const std::size_t end = list.payload + list.size;
    while (pos + 8 <= end && pos + 8 <= bytes.size()) {
        const std::uint32_t size = u32_at(bytes, pos + 4);
        if (std::memcmp(bytes.data() + pos, id, 4) == 0) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — RIFF text bytes
            std::string text(reinterpret_cast<const char*>(bytes.data() + pos + 8), size);
            while (!text.empty() && text.back() == '\0') {
                text.pop_back();
            }
            return text;
        }
        pos += 8 + size + (size & 1U);
    }
    return {};
}

// Two-channel fixture: channel 0 carries a 440 Hz sample, channel 1 a
// 1320 Hz sample (note 49 = MIDI 60 = the samples' base note, so both
// play at ratio 1.0). Pattern 0 (16 rows) triggers both channels;
// pattern 1 (8 rows) triggers channel 1 only — order ranges and stems
// are therefore spectrally distinguishable.
nt::engine::TrackerProject suite_project() {
    nt::engine::TrackerProject project = nt::engine::create_project(2);
    project.name = "SUITE";
    project.patterns.clear();
    project.patterns.push_back(nt::engine::create_pattern(0, 16, 2));
    project.patterns.push_back(nt::engine::create_pattern(1, 8, 2));
    project.order_list = {0, 1};

    auto add_sample = [&project](int id, float freq) {
        nt::engine::TrackerSample sample;
        sample.id = id;
        sample.name = "SINE";
        sample.format = "wav";
        sample.original_data = make_wav(freq, 0.25F, 44100);
        sample.sample_rate = 44100;
        sample.num_channels = 1;
        sample.frames = static_cast<std::uint32_t>(44100 * 0.25F);
        sample.base_note = 60;
        sample.volume = 64;
        sample.pan = 128;
        project.samples.push_back(sample);
    };
    add_sample(1, 440.0F);
    add_sample(2, 1320.0F);

    for (int r = 0; r < 16; r += 4) {
        project.patterns[0].rows[static_cast<std::size_t>(r)][0] = {.note = 49, .instrument = 1};
    }
    for (int r = 2; r < 16; r += 4) {
        project.patterns[0].rows[static_cast<std::size_t>(r)][1] = {.note = 49, .instrument = 2};
    }
    for (int r = 0; r < 8; r += 4) {
        project.patterns[1].rows[static_cast<std::size_t>(r)][1] = {.note = 49, .instrument = 2};
    }
    return project;
}

// Frames of one sequencer tick at the fixture's default 125 bpm.
constexpr double kTickFrames = kRate * 2.5 / 125.0; // = 960

std::vector<float> stereo_sine(float freq, float amplitude, double seconds) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto s = amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * freq *
                                                               static_cast<double>(i) /
                                                               static_cast<double>(kRate)));
        out[i * 2] = s;
        out[(i * 2) + 1] = s;
    }
    return out;
}

double db_of(double linear) {
    return 20.0 * std::log10(linear);
}

} // namespace

TEST_CASE("export order range renders the requested slice", "[export][suite]") {
    const nt::engine::TrackerProject project = suite_project();
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const nt::io::FtrkWriteExtras extras;

    nt::io::ExportOptions options;
    options.sample_rate = kRate;
    options.tail_seconds = 0.25;

    // Full pattern 0: 16 rows * speed 6 = 96 ticks.
    options.start_order = 0;
    options.end_order = 0;
    const nt::io::ExportResult first =
        nt::io::export_project(project, extras, dir / "nt_range0.wav", options);
    INFO(first.error);
    REQUIRE(first.ok);
    const auto tail_frames = static_cast<double>(kRate) * 0.25;
    CHECK(std::abs(static_cast<double>(first.frames) - ((96 * kTickFrames) + tail_frames)) <
          2.0 * kTickFrames);

    // Pattern 1 alone: 8 rows = 48 ticks, and only the 1320 Hz channel
    // plays there — the seeded start position is audible, not just
    // shorter.
    options.start_order = 1;
    options.end_order = 1;
    const nt::io::ExportResult second =
        nt::io::export_project(project, extras, dir / "nt_range1.wav", options);
    INFO(second.error);
    REQUIRE(second.ok);
    CHECK(std::abs(static_cast<double>(second.frames) - ((48 * kTickFrames) + tail_frames)) <
          2.0 * kTickFrames);

    nt::audio::codec::Decoded decoded;
    std::string error;
    const std::vector<std::uint8_t> bytes = file_bytes(dir / "nt_range1.wav");
    REQUIRE(nt::audio::codec::decode_wav(bytes.data(), bytes.size(), decoded, error));
    const std::vector<float> mono = mono_of(decoded);
    CHECK(goertzel(mono, decoded.rate, 1320.0F) > 5.0F * goertzel(mono, decoded.rate, 440.0F));
}

TEST_CASE("export stems isolate channels and bundle into zip", "[export][suite]") {
    const nt::engine::TrackerProject project = suite_project();
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const nt::io::FtrkWriteExtras extras;

    nt::io::ExportOptions options;
    options.sample_rate = kRate;
    options.tail_seconds = 0.25;
    options.stem_mask = 0b11;

    const nt::io::ExportResult stems =
        nt::io::export_project(project, extras, dir / "nt_stems.wav", options);
    INFO(stems.error);
    REQUIRE(stems.ok);
    REQUIRE(stems.files.size() == 2);
    CHECK(stems.files[0].filename() == "nt_stems.ch01.wav");
    CHECK(stems.files[1].filename() == "nt_stems.ch02.wav");

    std::string error;
    nt::audio::codec::Decoded ch01;
    nt::audio::codec::Decoded ch02;
    const std::vector<std::uint8_t> bytes01 = file_bytes(stems.files[0]);
    const std::vector<std::uint8_t> bytes02 = file_bytes(stems.files[1]);
    REQUIRE(nt::audio::codec::decode_wav(bytes01.data(), bytes01.size(), ch01, error));
    REQUIRE(nt::audio::codec::decode_wav(bytes02.data(), bytes02.size(), ch02, error));
    CHECK(ch01.frames == ch02.frames);

    // Each stem carries its own channel's tone and none of the other's.
    const std::vector<float> mono01 = mono_of(ch01);
    const std::vector<float> mono02 = mono_of(ch02);
    const float low01 = goertzel(mono01, ch01.rate, 440.0F);
    const float high01 = goertzel(mono01, ch01.rate, 1320.0F);
    const float low02 = goertzel(mono02, ch02.rate, 440.0F);
    const float high02 = goertzel(mono02, ch02.rate, 1320.0F);
    CHECK(low01 > 0.0005F);
    CHECK(high02 > 0.0005F);
    CHECK(low01 > 10.0F * high01);
    CHECK(high02 > 10.0F * low02);

    // ZIP bundling replaces the loose files with one archive.
    options.stem_zip = true;
    const nt::io::ExportResult zipped =
        nt::io::export_project(project, extras, dir / "nt_zipped.wav", options);
    INFO(zipped.error);
    REQUIRE(zipped.ok);
    REQUIRE(zipped.files.size() == 1);
    CHECK(zipped.files[0].filename() == "nt_zipped.stems.zip");
    CHECK(!std::filesystem::exists(dir / "nt_zipped.ch01.wav"));

    const std::vector<std::uint8_t> zip_bytes = file_bytes(zipped.files[0]);
    mz_zip_archive zip{};
    REQUIRE(mz_zip_reader_init_mem(&zip, zip_bytes.data(), zip_bytes.size(), 0) == MZ_TRUE);
    CHECK(mz_zip_reader_get_num_files(&zip) == 2);
    mz_zip_archive_file_stat stat{};
    REQUIRE(mz_zip_reader_file_stat(&zip, 0, &stat) == MZ_TRUE);
    CHECK(std::string(stat.m_filename) == "nt_zipped.ch01.wav");
    mz_zip_reader_end(&zip);
}

TEST_CASE("export wav depths write valid headers and round-trip", "[export][suite]") {
    const nt::engine::TrackerProject project = suite_project();
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const nt::io::FtrkWriteExtras extras;

    nt::io::ExportOptions options;
    options.sample_rate = kRate;
    options.tail_seconds = 0.25;
    options.end_order = 0;
    options.metadata.title = "Suite Title";
    options.metadata.artist = "Savannah";
    options.metadata.album = "NanoTracker";
    options.metadata.date = "2026";
    options.metadata.comment = "stage 15";

    struct Case {
        nt::io::WavDepth depth;
        const char* name;
        std::uint16_t tag;
        std::uint16_t bits;
    };

    const std::vector<Case> cases = {
        {nt::io::WavDepth::kPcm16, "nt_depth16.wav", 1, 16},
        {nt::io::WavDepth::kPcm24, "nt_depth24.wav", 1, 24},
        {nt::io::WavDepth::kFloat32, "nt_depth32.wav", 3, 32},
    };
    std::vector<nt::audio::codec::Decoded> decoded(cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        options.wav_depth = cases[i].depth;
        const nt::io::ExportResult result =
            nt::io::export_project(project, extras, dir / cases[i].name, options);
        INFO(result.error);
        REQUIRE(result.ok);

        const std::vector<std::uint8_t> bytes = file_bytes(dir / cases[i].name);
        const RiffChunk fmt = find_riff_chunk(bytes, "fmt ");
        REQUIRE(fmt.found);
        CHECK(u16_at(bytes, fmt.payload) == cases[i].tag);
        CHECK(u16_at(bytes, fmt.payload + 2) == 2); // stereo
        CHECK(u32_at(bytes, fmt.payload + 4) == kRate);
        CHECK(u16_at(bytes, fmt.payload + 14) == cases[i].bits);
        const RiffChunk data = find_riff_chunk(bytes, "data");
        REQUIRE(data.found);
        CHECK(data.size == result.frames * 2 * cases[i].bits / 8);
        if (cases[i].depth == nt::io::WavDepth::kFloat32) {
            const RiffChunk fact = find_riff_chunk(bytes, "fact");
            REQUIRE(fact.found);
            CHECK(u32_at(bytes, fact.payload) == result.frames);
        }

        // Metadata LIST/INFO chunk reads back on every depth.
        const RiffChunk list = find_riff_chunk(bytes, "LIST");
        REQUIRE(list.found);
        REQUIRE(std::memcmp(bytes.data() + list.payload, "INFO", 4) == 0);
        CHECK(info_field(bytes, list, "INAM") == "Suite Title");
        CHECK(info_field(bytes, list, "IART") == "Savannah");
        CHECK(info_field(bytes, list, "IPRD") == "NanoTracker");
        CHECK(info_field(bytes, list, "ICRD") == "2026");
        CHECK(info_field(bytes, list, "ICMT") == "stage 15");

        std::string error;
        REQUIRE(nt::audio::codec::decode_wav(bytes.data(), bytes.size(), decoded[i], error));
        CHECK(decoded[i].frames == result.frames);
        CHECK(decoded[i].channels == 2);
    }

    // Same render at every depth: decoded audio differs only by
    // quantisation depth.
    REQUIRE(decoded[0].interleaved.size() == decoded[1].interleaved.size());
    REQUIRE(decoded[1].interleaved.size() == decoded[2].interleaved.size());
    float diff_16_32 = 0.0F;
    float diff_24_32 = 0.0F;
    for (std::size_t i = 0; i < decoded[0].interleaved.size(); ++i) {
        diff_16_32 =
            std::max(diff_16_32, std::abs(decoded[0].interleaved[i] - decoded[2].interleaved[i]));
        diff_24_32 =
            std::max(diff_24_32, std::abs(decoded[1].interleaved[i] - decoded[2].interleaved[i]));
    }
    CHECK(diff_16_32 < 1.0F / 16384.0F);
    CHECK(diff_24_32 < 1.0F / 4194304.0F);
}

TEST_CASE("export ogg carries quality and vorbis comments", "[export][suite]") {
    const nt::engine::TrackerProject project = suite_project();
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const nt::io::FtrkWriteExtras extras;

    nt::io::ExportOptions options;
    options.format = nt::io::ExportFormat::kOgg;
    options.sample_rate = kRate;
    options.tail_seconds = 0.25;
    options.end_order = 0;
    options.metadata.title = "Suite Title";
    options.metadata.artist = "Savannah";
    options.metadata.date = "2026";

    options.ogg_quality = 0.0F;
    const nt::io::ExportResult low =
        nt::io::export_project(project, extras, dir / "nt_q_low.ogg", options);
    INFO(low.error);
    REQUIRE(low.ok);
    options.ogg_quality = 0.9F;
    const nt::io::ExportResult high =
        nt::io::export_project(project, extras, dir / "nt_q_high.ogg", options);
    INFO(high.error);
    REQUIRE(high.ok);
    CHECK(std::filesystem::file_size(dir / "nt_q_high.ogg") >
          std::filesystem::file_size(dir / "nt_q_low.ogg"));

    const std::vector<std::uint8_t> bytes = file_bytes(dir / "nt_q_high.ogg");
    std::vector<std::string> comments;
    std::string error;
    REQUIRE(nt::audio::codec::decode_ogg_comments(bytes.data(), bytes.size(), comments, error));
    auto has = [&comments](const std::string& entry) {
        return std::find(comments.begin(), comments.end(), entry) != comments.end();
    };
    CHECK(has("TITLE=Suite Title"));
    CHECK(has("ARTIST=Savannah"));
    CHECK(has("DATE=2026"));
    CHECK(has("ENCODER=nanoTracker"));
}

TEST_CASE("export mp3 honours bitrate and id3 tags when lame exists", "[export][suite]") {
    const nt::engine::TrackerProject project = suite_project();
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const nt::io::FtrkWriteExtras extras;

    nt::io::ExportOptions options;
    options.format = nt::io::ExportFormat::kMp3;
    options.sample_rate = kRate;
    options.tail_seconds = 0.25;
    options.end_order = 0;
    options.metadata.title = "Suite Title";
    options.metadata.artist = "Savannah";

    options.mp3_bitrate_kbps = 64;
    const nt::io::ExportResult low =
        nt::io::export_project(project, extras, dir / "nt_br64.mp3", options);
    if (!low.ok) {
        WARN("mp3 export unavailable: " << low.error);
        return;
    }
    options.mp3_bitrate_kbps = 320;
    const nt::io::ExportResult high =
        nt::io::export_project(project, extras, dir / "nt_br320.mp3", options);
    INFO(high.error);
    REQUIRE(high.ok);
    // CBR: the stream size scales with the requested bitrate.
    CHECK(std::filesystem::file_size(dir / "nt_br320.mp3") >
          2 * std::filesystem::file_size(dir / "nt_br64.mp3"));

    // libmp3lame exports id3tag_*, so the v2 tag leads the stream; the
    // frame stays decodable through the app's decoder.
    const std::vector<std::uint8_t> bytes = file_bytes(dir / "nt_br320.mp3");
    REQUIRE(bytes.size() > 3);
    CHECK(std::memcmp(bytes.data(), "ID3", 3) == 0);
    nt::audio::codec::Decoded decoded;
    std::string error;
    CHECK(nt::audio::codec::decode_mp3(bytes.data(), bytes.size(), decoded, error));
}

TEST_CASE("export post fades shape the envelope", "[export][suite]") {
    nt::io::ExportPostOptions options;
    options.fade_in_seconds = 0.5;
    options.fade_out_seconds = 0.5;
    options.fade_in_shape = nt::io::FadeShape::kLinear;
    options.fade_out_shape = nt::io::FadeShape::kEqualPower;

    const auto frames = static_cast<std::size_t>(2 * kRate);
    std::vector<float> buffer(frames * 2, 1.0F);
    std::string error;
    REQUIRE(nt::io::apply_export_post(buffer, kRate, options, error));

    const std::size_t fade = kRate / 2;
    // Linear fade-in: silent start, half amplitude at the midpoint,
    // untouched after the span.
    CHECK(buffer[0] == 0.0F);
    CHECK(std::abs(buffer[(fade / 2) * 2] - 0.5F) < 0.01F);
    CHECK(buffer[(fade + 100) * 2] == 1.0F);
    // Equal-power fade-out: sin curve — ~0.707 at the midpoint, exact
    // zero on the final sample.
    const std::size_t out_mid = frames - (fade / 2);
    CHECK(std::abs(buffer[out_mid * 2] - 0.70710678F) < 0.01F);
    CHECK(buffer[(frames * 2) - 1] == 0.0F);
    CHECK(buffer[(frames - fade - 100) * 2] == 1.0F);
}

TEST_CASE("export post normalises to peak, true-peak and lufs targets", "[export][suite]") {
    std::string error;

    // Known-level fixture: a stereo 997 Hz sine at -20 dBFS reads
    // ~-20 LUFS (BS.1770: full-scale stereo 1 kHz = ~0 LUFS).
    std::vector<float> sine = stereo_sine(997.0F, 0.1F, 5.0);
    nt::io::LoudnessMeasurement measured;
    REQUIRE(nt::io::measure_loudness(sine.data(), sine.size() / 2, kRate, measured, error));
    REQUIRE(measured.has_integrated);
    CHECK(std::abs(measured.integrated_lufs - (-20.0)) < 0.5);
    CHECK(std::abs(db_of(measured.sample_peak) - (-20.0)) < 0.1);

    nt::io::ExportPostOptions options;
    options.normalize = nt::io::NormalizeMode::kLufs;
    options.normalize_target_db = -16.0;
    REQUIRE(nt::io::apply_export_post(sine, kRate, options, error));
    REQUIRE(nt::io::measure_loudness(sine.data(), sine.size() / 2, kRate, measured, error));
    REQUIRE(measured.has_integrated);
    CHECK(std::abs(measured.integrated_lufs - (-16.0)) < 0.5);

    // Peak to -1 dBFS: the largest sample lands exactly on the target.
    std::vector<float> peak_buffer = stereo_sine(997.0F, 0.25F, 1.0);
    options.normalize = nt::io::NormalizeMode::kPeak;
    options.normalize_target_db = -1.0;
    REQUIRE(nt::io::apply_export_post(peak_buffer, kRate, options, error));
    float max_abs = 0.0F;
    for (const float sample : peak_buffer) {
        max_abs = std::max(max_abs, std::abs(sample));
    }
    CHECK(std::abs(db_of(max_abs) - (-1.0)) < 0.02);

    // True peak to -1 dBTP, read back through libebur128.
    std::vector<float> tp_buffer = stereo_sine(997.0F, 0.5F, 1.0);
    options.normalize = nt::io::NormalizeMode::kTruePeak;
    options.normalize_target_db = -1.0;
    REQUIRE(nt::io::apply_export_post(tp_buffer, kRate, options, error));
    REQUIRE(
        nt::io::measure_loudness(tp_buffer.data(), tp_buffer.size() / 2, kRate, measured, error));
    CHECK(std::abs(db_of(measured.true_peak) - (-1.0)) < 0.15);

    // Silence stays silent instead of exploding toward the target.
    std::vector<float> silence(static_cast<std::size_t>(kRate) * 2, 0.0F);
    options.normalize = nt::io::NormalizeMode::kLufs;
    REQUIRE(nt::io::apply_export_post(silence, kRate, options, error));
    for (const float sample : silence) {
        REQUIRE(sample == 0.0F);
    }
}

TEST_CASE("export lufs normalise lands end to end", "[export][suite]") {
    const nt::engine::TrackerProject project = suite_project();
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const nt::io::FtrkWriteExtras extras;

    nt::io::ExportOptions options;
    options.sample_rate = kRate;
    options.tail_seconds = 0.25;
    options.wav_depth = nt::io::WavDepth::kFloat32; // no quantisation in the loop
    options.post.normalize = nt::io::NormalizeMode::kLufs;
    options.post.normalize_target_db = -18.0;

    const nt::io::ExportResult result =
        nt::io::export_project(project, extras, dir / "nt_lufs.wav", options);
    INFO(result.error);
    REQUIRE(result.ok);

    const std::vector<std::uint8_t> bytes = file_bytes(dir / "nt_lufs.wav");
    nt::audio::codec::Decoded decoded;
    std::string error;
    REQUIRE(nt::audio::codec::decode_wav(bytes.data(), bytes.size(), decoded, error));
    REQUIRE(decoded.channels == 2);
    nt::io::LoudnessMeasurement measured;
    REQUIRE(nt::io::measure_loudness(decoded.interleaved.data(), decoded.frames, kRate, measured,
                                     error));
    REQUIRE(measured.has_integrated);
    CHECK(std::abs(measured.integrated_lufs - (-18.0)) < 0.5);
}
