#include "plugins/plugin_registry.h"

#include <algorithm>

namespace nt::plugins {

std::string PluginRegistry::load_archive(const std::uint8_t* data, std::size_t size,
                                         std::vector<std::string>& errors) {
    auto plugin = load_ntp_archive(data, size, device_rate_, errors);
    if (plugin == nullptr) {
        return {};
    }
    const std::string id = plugin->manifest.id;
    for (auto& existing : plugins_) {
        if (existing->manifest.id == id) {
            retired_plugins_.push_back(std::move(existing));
            existing = std::move(plugin);
            return id;
        }
    }
    plugins_.push_back(std::move(plugin));
    return id;
}

std::string PluginRegistry::load_file(const std::filesystem::path& path,
                                      std::vector<std::string>& errors) {
    std::vector<std::string> local;
    auto plugin = load_ntp_file(path.string(), device_rate_, local);
    if (plugin == nullptr) {
        for (const std::string& e : local) {
            errors.push_back(path.filename().string() + ": " + e);
        }
        return {};
    }
    return load_archive(plugin->archive_bytes.data(), plugin->archive_bytes.size(), errors);
}

void PluginRegistry::scan_directory(const std::filesystem::path& dir,
                                    std::vector<std::string>& warnings) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext != ".ntins" && ext != ".ntsfx") {
            continue;
        }
        std::vector<std::string> errors;
        if (load_file(entry.path(), errors).empty()) {
            for (const std::string& e : errors) {
                warnings.push_back("plugin scan: " + e);
            }
        }
    }
}

const LoadedNtpPlugin* PluginRegistry::find(const std::string& plugin_id) const {
    for (const auto& plugin : plugins_) {
        if (plugin->manifest.id == plugin_id) {
            return plugin.get();
        }
    }
    return nullptr;
}

NtpInstance* PluginRegistry::create_instance(const std::string& plugin_id) {
    // Mutable lookup: instances write envelope-stage edits back to the
    // owning plugin's manifest (NtpInstance::set_env_stage).
    for (const auto& plugin : plugins_) {
        if (plugin->manifest.id == plugin_id) {
            instances_.push_back(std::make_unique<NtpInstance>(*plugin, device_rate_));
            return instances_.back().get();
        }
    }
    return nullptr;
}

} // namespace nt::plugins
