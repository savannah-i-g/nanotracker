// plugins/plugin_registry — the catalogue of loaded NTP plugins and
// their live instances. Plain code module (the web's singleton
// grab-bag, pluginRegistry.ts, is not ported): the session owns one
// registry; archives arrive from the plugins directory scan, manual
// loads, and project PLGB blocks.
//
// Instance lifetime follows the RCU discipline: instances are created
// off-thread, handed to the audio thread inside the playback bundle,
// and retired (never freed mid-playback) when replaced.
#pragma once

#include "plugins/ntp_loader.h"
#include "plugins/ntp_voices.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nt::plugins {

class PluginRegistry {
public:
    explicit PluginRegistry(std::uint32_t device_rate) : device_rate_(device_rate) {}

    // Loads an archive; replaces any existing catalogue entry with the
    // same manifest id (retiring, not destroying, its plugin so live
    // instances stay valid). Returns the id, or empty with errors.
    std::string load_archive(const std::uint8_t* data, std::size_t size,
                             std::vector<std::string>& errors);
    std::string load_file(const std::filesystem::path& path, std::vector<std::string>& errors);

    // Scans a directory for .ntins/.ntsfx (non-recursive); failures
    // append to `warnings`, successes to the catalogue.
    void scan_directory(const std::filesystem::path& dir, std::vector<std::string>& warnings);

    [[nodiscard]] const LoadedNtpPlugin* find(const std::string& plugin_id) const;

    [[nodiscard]] const std::vector<std::unique_ptr<LoadedNtpPlugin>>& all() const {
        return plugins_;
    }

    // Creates an instance of a catalogued plugin (null if unknown).
    // The registry keeps ownership; the pointer stays valid until
    // shutdown (retired instances are never destroyed mid-session).
    NtpInstance* create_instance(const std::string& plugin_id);

private:
    std::uint32_t device_rate_;
    std::vector<std::unique_ptr<LoadedNtpPlugin>> plugins_;
    std::vector<std::unique_ptr<LoadedNtpPlugin>> retired_plugins_;
    std::vector<std::unique_ptr<NtpInstance>> instances_;
};

} // namespace nt::plugins
