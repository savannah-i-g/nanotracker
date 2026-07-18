// plugins/ntp_loader — NTP archive reading and strict validation.
//
// Load philosophy (fix-don't-retain #10): a plugin either loads
// completely or is refused with messages that say exactly why — there
// is no "degraded" mode (the web fell back to a half-working state,
// pluginInstrumentGraph.ts:562). Warnings are reserved for things that
// change presentation, not behaviour (e.g. a webview-only UI falling
// back to the auto-param panel).
//
// The manifest parser is exposed separately so tools/ntp-convert and
// the tests can run schema validation without a ZIP around it.
#pragma once

#include "audio/sample_buffer.h"
#include "ntp/ntp_manifest.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nt::plugins {

// A fully-loaded plugin: manifest plus decoded assets. Sample-type
// assets are device-rate stereo (ready for the sampler/granular
// runtimes); wavetables keep their source frames verbatim — resampling
// would smear the frame boundaries the oscillator depends on.
struct LoadedNtpPlugin {
    ntp::Manifest manifest;
    std::vector<std::uint8_t> archive_bytes; // original ZIP, for PLGB

    std::map<std::string, std::unique_ptr<audio::SampleBuffer>> samples;

    struct RawTable {
        std::vector<float> mono;
        std::uint32_t rate = 0;
    };

    std::map<std::string, RawTable> tables;

    struct Image {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
    };

    std::map<std::string, Image> images;

    std::vector<std::string> warnings;
};

// Parses and validates a plugin.json payload. Returns true when the
// manifest is fully valid; every problem found lands in `errors`
// (collected, not first-fail, so authors see the whole list).
bool parse_ntp_manifest(const std::string& json_text, ntp::Manifest& manifest,
                        std::vector<std::string>& errors);

// Reads a .ntins/.ntsfx archive from memory: manifest + assets decoded
// against `device_rate`. Returns null with `errors` populated on any
// validation or asset failure.
std::unique_ptr<LoadedNtpPlugin> load_ntp_archive(const std::uint8_t* data, std::size_t size,
                                                  std::uint32_t device_rate,
                                                  std::vector<std::string>& errors);

std::unique_ptr<LoadedNtpPlugin> load_ntp_file(const std::string& path, std::uint32_t device_rate,
                                               std::vector<std::string>& errors);

} // namespace nt::plugins
