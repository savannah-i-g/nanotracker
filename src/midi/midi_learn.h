// midi/midi_learn — the learn state machine and mapping store. A
// mapping binds (channel, CC) to a learnable parameter: NTP param key,
// CLAP param id, or VST3 param id on a workspace node. Mappings
// persist as JSON in the config directory (web parity: midiMappings.ts
// kept them in localStorage).
//
// Flow: the UI arms a target; the next CC that arrives binds it and
// saves. Unarmed CCs apply through their mappings, scaled into the
// target's range.
#pragma once

#include "app/project_session.h"
#include "midi/midi_io.h"

#include <filesystem>
#include <string>
#include <vector>

namespace nt::midi {

struct MidiMapping {
    std::uint8_t channel = 0;
    std::uint8_t cc = 0;
    std::string kind; // "ntp" | "clap" | "vst3"
    std::string workspace_id;
    std::string param; // NTP key, or numeric id for clap/vst3
};

class MidiLearn {
public:
    explicit MidiLearn(std::filesystem::path store_path);

    void arm(const std::string& kind, const std::string& workspace_id, const std::string& param);

    void cancel() { armed_ = false; }

    [[nodiscard]] bool armed() const { return armed_; }

    [[nodiscard]] const std::vector<MidiMapping>& mappings() const { return mappings_; }

    void remove_mapping(std::size_t index);

    // Routes one CC: binds when armed (then saves), else applies every
    // matching mapping through the session.
    void handle_cc(app::ProjectSession& session, std::uint8_t channel, std::uint8_t cc,
                   std::uint8_t value);

private:
    void load();
    void save() const;
    static void apply(app::ProjectSession& session, const MidiMapping& mapping, std::uint8_t value);

    std::filesystem::path store_path_;
    std::vector<MidiMapping> mappings_;
    bool armed_ = false;
    MidiMapping pending_;
};

} // namespace nt::midi
