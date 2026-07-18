#include "midi/midi_learn.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace nt::midi {

using nlohmann::json;

MidiLearn::MidiLearn(std::filesystem::path store_path) : store_path_(std::move(store_path)) {
    load();
}

void MidiLearn::arm(const std::string& kind, const std::string& workspace_id,
                    const std::string& param) {
    pending_.kind = kind;
    pending_.workspace_id = workspace_id;
    pending_.param = param;
    armed_ = true;
}

void MidiLearn::remove_mapping(std::size_t index) {
    if (index < mappings_.size()) {
        mappings_.erase(mappings_.begin() + static_cast<std::ptrdiff_t>(index));
        save();
    }
}

void MidiLearn::handle_cc(app::ProjectSession& session, std::uint8_t channel, std::uint8_t cc,
                          std::uint8_t value) {
    if (armed_) {
        pending_.channel = channel;
        pending_.cc = cc;
        // One mapping per (channel, cc): re-learning replaces.
        std::erase_if(mappings_, [this](const MidiMapping& m) {
            return m.channel == pending_.channel && m.cc == pending_.cc;
        });
        mappings_.push_back(pending_);
        armed_ = false;
        save();
        return;
    }
    for (const MidiMapping& mapping : mappings_) {
        if (mapping.channel == channel && mapping.cc == cc) {
            apply(session, mapping, value);
        }
    }
}

void MidiLearn::apply(app::ProjectSession& session, const MidiMapping& mapping,
                      std::uint8_t value) {
    const float normalized = static_cast<float>(value) / 127.0F;
    if (mapping.kind == "ntp") {
        if (plugins::NtpInstance* instance = session.plugin_instance(mapping.workspace_id)) {
            // Scale into the parameter's declared range when known.
            for (const ntp::ParamDef& def : instance->manifest().params) {
                if (def.key == mapping.param) {
                    instance->set_param(def.key,
                                        static_cast<float>(def.min) +
                                            (normalized * static_cast<float>(def.max - def.min)));
                    return;
                }
            }
            instance->set_param(mapping.param, normalized);
        }
    } else if (mapping.kind == "clap") {
        if (ext::ClapPlugin* instance = session.clap_instance(mapping.workspace_id)) {
            const auto id = static_cast<clap_id>(std::strtoul(mapping.param.c_str(), nullptr, 10));
            for (const ext::ClapParamInfo& param : instance->params()) {
                if (param.id == id) {
                    instance->set_param(id, param.min + (normalized * (param.max - param.min)));
                    return;
                }
            }
        }
    } else if (mapping.kind == "vst3") {
        if (ext::Vst3Plugin* instance = session.vst3_instance(mapping.workspace_id)) {
            const auto id =
                static_cast<std::uint32_t>(std::strtoul(mapping.param.c_str(), nullptr, 10));
            instance->set_param(id, normalized); // VST3 is normalized already
        }
    }
}

void MidiLearn::load() {
    std::ifstream file(store_path_);
    if (!file) {
        return;
    }
    const json root = json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        return;
    }
    for (const json& entry : root) {
        if (!entry.is_object()) {
            continue;
        }
        MidiMapping mapping;
        mapping.channel = static_cast<std::uint8_t>(entry.value("channel", 0));
        mapping.cc = static_cast<std::uint8_t>(entry.value("cc", 0));
        mapping.kind = entry.value("kind", "");
        mapping.workspace_id = entry.value("workspaceId", "");
        mapping.param = entry.value("param", "");
        mappings_.push_back(std::move(mapping));
    }
}

void MidiLearn::save() const {
    json root = json::array();
    for (const MidiMapping& mapping : mappings_) {
        root.push_back(json{{"channel", mapping.channel},
                            {"cc", mapping.cc},
                            {"kind", mapping.kind},
                            {"workspaceId", mapping.workspace_id},
                            {"param", mapping.param}});
    }
    std::error_code ec;
    std::filesystem::create_directories(store_path_.parent_path(), ec);
    std::ofstream file(store_path_);
    file << root.dump(2) << '\n';
}

} // namespace nt::midi
