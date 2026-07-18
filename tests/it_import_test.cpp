// IT importer verification against the authored fixture
// tests/golden/nttest.it (sample mode, 4 channels, one looping signed
// 8-bit square at C5Speed 8363, effects D and M plus a note-off —
// channel volume M is the approximated-but-counted pin).
#include "io/import/it_importer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        return {};
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Minimal reader for the importer's mono PCM16 WAV output (build_wav16
// writes a fixed 44-byte header, data thereafter).
std::vector<std::int16_t> wav16_samples(const std::vector<std::uint8_t>& wav) {
    std::vector<std::int16_t> out;
    if (wav.size() < 44) {
        return out;
    }
    for (std::size_t i = 44; i + 1 < wav.size(); i += 2) {
        out.push_back(static_cast<std::int16_t>(wav[i] | (wav[i + 1] << 8)));
    }
    return out;
}

// Reproduces build_wav_from_float's decoded-PCM → WAV16 mapping exactly
// (pcm/norm, clamped to [-1,1], scaled by 32767, truncated), so a test
// can assert the exact decoded sample value through the WAV container.
std::int16_t expected_wav16(int pcm, float norm) {
    const float f = std::clamp(static_cast<float>(pcm) / norm, -1.0F, 1.0F);
    return static_cast<std::int16_t>(f * 32767.0F);
}

bool any_warning_contains(const nt::io::import::ImportResults& r, const std::string& needle) {
    return std::any_of(r.warnings.begin(), r.warnings.end(),
                       [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("IT fixture imports faithfully", "[import][it]") {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/nttest.it", std::ios::binary);
    REQUIRE(file.good());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());

    nt::io::import::ImportResults results;
    const auto project = nt::io::import::import_it(bytes.data(), bytes.size(), results);
    REQUIRE(project.has_value());
    CHECK(results.errors.empty());

    CHECK(project->name == "NTTEST");
    CHECK(project->channels == 4);
    CHECK(project->bpm == 150);
    CHECK(project->speed == 4);
    REQUIRE(project->patterns.size() == 1);
    REQUIRE(project->order_list == std::vector<int>{0});

    const auto& rows = project->patterns[0].rows;
    REQUIRE(rows.size() == 64);
    // IT notes are already 1-based tracker notes: byte 61 = C-5.
    CHECK(rows[0][0].note == 61);
    CHECK(rows[0][0].instrument == 1);
    CHECK(rows[0][0].volume == 32);
    // D12 volume slide → engine Axy.
    CHECK(rows[0][1].effect == 0xA);
    CHECK(rows[0][1].effect_param == 0x12);
    // M30 channel volume lands as set-volume Cxx, tallied as an
    // approximation in warnings rather than converted silently.
    CHECK(rows[8][2].effect == 0xC);
    CHECK(rows[8][2].effect_param == 0x30);
    CHECK(rows[16][0].note == nt::engine::kNoteOff); // IT note byte 255
    CHECK(rows[32][3].note == 49);                   // C-4
    CHECK(rows[32][3].instrument == 1);
    REQUIRE(results.warnings.size() == 1);
    CHECK(results.warnings[0].find("approximated") != std::string::npos);

    // Sample stays at source rate (no resample on the IT path).
    REQUIRE(project->samples.size() == 1);
    const auto& sample = project->samples[0];
    CHECK(sample.id == 1);
    CHECK(sample.name == "SQUARE");
    CHECK(sample.frames == 32);
    CHECK(sample.sample_rate == 8363);
    CHECK(sample.loop_start == 0);
    CHECK(sample.loop_length == 32);
    CHECK(sample.base_note == 72); // MIDI C-5 at C5Speed
    CHECK(sample.volume == 48);
    CHECK(sample.pan == 128);
    // The embedded bytes are a well-formed WAV (RIFF magic).
    REQUIRE(sample.original_data.size() > 44);
    CHECK(sample.original_data[0] == 'R');
    CHECK(sample.original_data[1] == 'I');
}

TEST_CASE("IT importer rejects non-IT data", "[import][it]") {
    const std::vector<std::uint8_t> junk(200, 0x55);
    nt::io::import::ImportResults results;
    CHECK_FALSE(nt::io::import::import_it(junk.data(), junk.size(), results).has_value());
    CHECK_FALSE(results.errors.empty());
}

// tests/golden/nttest_compressed.it: authored fixture (generator port
// of libopenmpt's public-domain ITCompression, cross-checked so the
// bytes decode back to these waveforms). 33 samples, sample mode, one
// channel. Slots 0-3 hold the SAME 48-point wave8 / wave16 compressed
// four ways — 8/16-bit × IT2.14 (cvt=0x01, single delta) / IT2.15
// (cvt=0x05, double delta); slot 4 is that wave8 as uncompressed
// delta-PCM; slots 5-32 are filler that pushes the count past the 31
// slot cap. Crucially the file header is cmwt=0x0214, so any decoder
// that keys the double-delta variant off cmwt (the old bug) decodes the
// cvt=0x05 slots to noise — the per-sample cvt bit is the only correct
// signal. Pattern row 0 names sample 32 (dropped) to exercise the
// reference warning.
TEST_CASE("IT compressed samples decode: 2.14 and 2.15, 8 and 16 bit", "[import][it]") {
    const auto bytes = read_file(std::string(NT_GOLDEN_DIR) + "/nttest_compressed.it");
    REQUIRE_FALSE(bytes.empty());

    nt::io::import::ImportResults results;
    const auto project = nt::io::import::import_it(bytes.data(), bytes.size(), results);
    REQUIRE(project.has_value());
    CHECK(results.errors.empty());

    // The 48-point source waveforms (small runs + sharp jumps so the
    // compressor exercises all three width-change encodings). Every
    // compressed slot must decode back to these, bit for bit.
    static const std::vector<int> kWave8 = {
        0,   1,   0,   2,   3,   3,   2,   0,   1,   4, 4, 3, 113, 121, 115, 125,
        113, 120, 117, 126, 121, 125, 117, 123, 121, 1, 3, 2, 3,   1,   2,   2,
        1,   2,   1,   3,   1,   2,   2,   3,   2,   2, 4, 3, 4,   3,   3,   4};
    static const std::vector<int> kWave16 = {
        0,     40,    10,    70,    90,    80,    30,    60,    140,   120,   130,   90,
        22090, 22990, 21490, 23690, 20690, 21890, 21090, 22790, 22190, 22590, 20690, 21390,
        21090, -6910, -6790, -6850, -6650, -6790, -6700, -6730, -6680, -6750, -6690, -6710,
        -6700, -6790, -6750, -6670, -6720, -6690, -6700, -6630, -6670, -6650, -6680, -6670};

    const auto check_pcm = [](const nt::engine::TrackerSample& s, const std::vector<int>& want,
                              float norm) {
        const auto got = wav16_samples(s.original_data);
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            INFO("sample point " << i);
            CHECK(got[i] == expected_wav16(want[i], norm));
        }
    };

    // 31-slot cap: 33 valid samples in, 31 kept, 32-33 reported dropped.
    REQUIRE(project->samples.size() == 31);
    CHECK(any_warning_contains(results, "samples 32-33 dropped"));
    CHECK(any_warning_contains(results, "native slot limit"));
    // Pattern names sample 32 (a dropped slot): must warn, not go silent.
    CHECK(any_warning_contains(results, "references dropped sample(s) 32"));
    // No spurious decompressor-error/truncation warning on clean data.
    CHECK_FALSE(any_warning_contains(results, "truncated"));

    check_pcm(project->samples[0], kWave8, 128.0F);    // 8-bit  IT2.14
    check_pcm(project->samples[1], kWave8, 128.0F);    // 8-bit  IT2.15
    check_pcm(project->samples[2], kWave16, 32768.0F); // 16-bit IT2.14
    check_pcm(project->samples[3], kWave16, 32768.0F); // 16-bit IT2.15
    check_pcm(project->samples[4], kWave8, 128.0F);    // uncompressed delta-PCM
}

// End-to-end guard on the owner's real file (35 double-/single-delta
// compressed samples). Absent on CI, so WARN-skip to stay green. The
// noise metric: garbage double-delta output is uncorrelated, so its
// first-difference RMS meets or exceeds the signal RMS; a real waveform
// is smooth, so first-difference RMS is a fraction of it. Pre-fix,
// sample 1 decoded with ratio ~1.29 (noise); the correct per-sample
// decode gives ~0.07.
TEST_CASE("IT owner file imports as signal, not noise", "[import][it][owner]") {
    std::string path;
    if (const char* env = std::getenv("NT_IT_OWNER_FILE"); env != nullptr) {
        path = env;
    } else {
        path = "/home/federated-industrial/Downloads/02fd_-_lumifluidity.it";
    }
    const auto bytes = read_file(path);
    if (bytes.empty()) {
        SKIP("owner IT file not present (" << path << "); set NT_IT_OWNER_FILE to run");
    }

    nt::io::import::ImportResults results;
    const auto project = nt::io::import::import_it(bytes.data(), bytes.size(), results);
    REQUIRE(project.has_value());
    CHECK(results.errors.empty());
    // 35 samples → 31 kept, 32-35 reported dropped; zero decode errors.
    CHECK(project->samples.size() == 31);
    CHECK(any_warning_contains(results, "samples 32-35 dropped"));
    CHECK_FALSE(any_warning_contains(results, "truncated"));

    // First kept sample is slot 1 (8-bit, cvt=0x01 → single delta); the
    // slot most obviously corrupted before the fix.
    const auto pcm = wav16_samples(project->samples[0].original_data);
    REQUIRE(pcm.size() > 1000);
    double sq = 0.0;
    double dsq = 0.0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        sq += static_cast<double>(pcm[i]) * pcm[i];
        if (i > 0) {
            const double d = static_cast<double>(pcm[i]) - pcm[i - 1];
            dsq += d * d;
        }
    }
    const double signal_rms = std::sqrt(sq / static_cast<double>(pcm.size()));
    const double diff_rms = std::sqrt(dsq / static_cast<double>(pcm.size() - 1));
    INFO("signal_rms=" << signal_rms << " diff_rms=" << diff_rms);
    CHECK(signal_rms > 500.0);          // not silence
    CHECK(diff_rms < 0.5 * signal_rms); // smooth waveform, not white noise
}
