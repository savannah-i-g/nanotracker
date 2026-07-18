#include "plugins/ntp_voices.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nt::plugins {

namespace {

constexpr std::uint32_t kStereoFloats = kNtpBlockFrames * 2;

float apply_transform(float v, ntp::ModTransform transform) {
    switch (transform) {
    case ntp::ModTransform::kNone:
        return v;
    case ntp::ModTransform::kInvert:
        return -v;
    case ntp::ModTransform::kAbs:
        return std::abs(v);
    case ntp::ModTransform::kSquare:
        return v * std::abs(v);
    case ntp::ModTransform::kUnipolar:
        return (v + 1.0F) * 0.5F;
    case ntp::ModTransform::kBipolar:
        return (2.0F * v) - 1.0F;
    }
    return v;
}

float block_average(const float* stereo, std::uint32_t frames) {
    float sum = 0.0F;
    for (std::uint32_t i = 0; i < frames * 2; ++i) {
        sum += stereo[i];
    }
    return frames > 0 ? sum / (static_cast<float>(frames) * 2.0F) : 0.0F;
}

} // namespace

NtpInstance::NtpInstance(LoadedNtpPlugin& plugin, std::uint32_t rate)
    : plugin_(&plugin), rate_(rate),
      is_instrument_(plugin.manifest.type == ntp::PluginType::kInstrument) {
    const ntp::Manifest& manifest = plugin.manifest;
    const int pool = is_instrument_ ? manifest.voices : 1;

    nodes_.resize(manifest.graph.nodes.size());
    for (std::size_t n = 0; n < manifest.graph.nodes.size(); ++n) {
        const ntp::DspNode& def = manifest.graph.nodes[n];
        GraphNode& node = nodes_[n];
        node.def_index = static_cast<int>(n);
        // Samplers are always shared (their polyphony is internal);
        // native stages too (the C ABI is instance-scoped).
        node.voice_scoped = is_instrument_ && def.scope == ntp::NodeScope::kVoice &&
                            def.type != ntp::NodeType::kSampler &&
                            def.type != ntp::NodeType::kNativeStage;
        const int count = node.voice_scoped ? pool : 1;
        node.slots.resize(static_cast<std::size_t>(count));
        for (int s = 0; s < count; ++s) {
            NodeInstantiation& slot = node.slots[static_cast<std::size_t>(s)];
            slot.runtime = std::make_unique<NodeRuntime>(
                def, plugin, rate,
                static_cast<std::uint32_t>((n * 7919) + (static_cast<std::size_t>(s) * 104729) +
                                           1));
            slot.out.assign(kStereoFloats, 0.0F);
            slot.prev.assign(kStereoFloats, 0.0F);
            slot.in_sum.assign(kStereoFloats, 0.0F);
        }
        if (node.voice_scoped && def.type == ntp::NodeType::kEnvelope) {
            has_voice_envelopes_ = true;
        }
    }

    voices_.resize(static_cast<std::size_t>(pool));
    voice_mix_.assign(kStereoFloats, 0.0F);
    zero_block_.assign(kStereoFloats, 0.0F);

    for (const ntp::ParamDef& param : manifest.params) {
        host_params_.emplace_back(param.key, static_cast<float>(param.def));
    }

    build_topology();

    // Bind dot-path host params onto node bases.
    for (const auto& [key, value] : host_params_) {
        set_param(key, value);
    }

    // Pre-resolve CV→param targets (plugin_set_param_cv writes these
    // ParamSlot pointers on the audio thread; slot storage is fixed
    // after construction, so the pointers stay valid).
    cv_targets_.resize(host_params_.size());
    for (std::size_t p = 0; p < manifest.params.size(); ++p) {
        CvParamTarget& target = cv_targets_[p];
        target.min = static_cast<float>(manifest.params[p].min);
        target.max = static_cast<float>(manifest.params[p].max);
        const std::string& key = manifest.params[p].key;
        const auto dot = key.find('.');
        if (dot == std::string::npos) {
            continue; // bare key: host param value only ("param:" routes)
        }
        const int node_index = node_index_by_id(key.substr(0, dot));
        if (node_index < 0) {
            continue;
        }
        const std::string param_name = key.substr(dot + 1);
        for (NodeInstantiation& slot : nodes_[static_cast<std::size_t>(node_index)].slots) {
            if (ParamSlot* param = slot.runtime->find_param(param_name.c_str())) {
                target.slots.push_back(param);
            }
        }
    }

    // Flatten mod routes.
    for (const ntp::ModRoute& route : manifest.graph.mod_routes) {
        for (const ntp::ModTarget& target : route.targets) {
            ModBinding binding;
            binding.def = target;
            if (route.source == "velocity") {
                binding.source = ModBinding::Source::kVelocity;
            } else if (route.source == "note") {
                binding.source = ModBinding::Source::kNote;
            } else if (route.source == "gate") {
                binding.source = ModBinding::Source::kGate;
            } else if (route.source == "pitch") {
                binding.source = ModBinding::Source::kPitch;
            } else if (route.source.starts_with("param:")) {
                binding.source = ModBinding::Source::kParam;
                const std::string key = route.source.substr(6);
                for (std::size_t p = 0; p < host_params_.size(); ++p) {
                    if (host_params_[p].first == key) {
                        binding.param_index = static_cast<int>(p);
                        break;
                    }
                }
            } else {
                binding.source = ModBinding::Source::kNode;
                binding.source_node = node_index_by_id(route.source);
            }
            const auto dot = target.target.find('.');
            if (dot == std::string::npos) {
                continue;
            }
            binding.target_node = node_index_by_id(target.target.substr(0, dot));
            binding.target_param = target.target.substr(dot + 1);
            if (binding.target_node < 0) {
                continue;
            }
            binding.target_voice_scoped =
                nodes_[static_cast<std::size_t>(binding.target_node)].voice_scoped ? 1 : 0;
            binding.slew_coeff =
                target.slew > 0.0
                    ? 1.0F - std::exp(-static_cast<float>(kNtpBlockFrames) /
                                      (static_cast<float>(target.slew) * static_cast<float>(rate)))
                    : 0.0F;
            mod_bindings_.push_back(std::move(binding));
        }
    }
    shared_mod_states_.assign(mod_bindings_.size(), ModState{});
    for (Voice& voice : voices_) {
        voice.mod_states.assign(mod_bindings_.size(), ModState{});
    }
}

int NtpInstance::node_index_by_id(const std::string& id) const {
    const auto& defs = plugin_->manifest.graph.nodes;
    for (std::size_t n = 0; n < defs.size(); ++n) {
        if (defs[n].id == id) {
            return static_cast<int>(n);
        }
    }
    return -1;
}

void NtpInstance::build_topology() {
    const ntp::Manifest& manifest = plugin_->manifest;

    for (const ntp::Connection& connection : manifest.graph.connections) {
        Edge edge;
        if (connection.from == "input" || connection.from == "voiceIn") {
            edge.src = kSrcExternal;
        } else if (connection.from == "voiceOut") {
            edge.src = kSrcVoiceMix;
        } else {
            edge.src = node_index_by_id(connection.from);
            if (edge.src < 0) {
                continue;
            }
        }
        if (!connection.to_param.empty()) {
            edge.dst = node_index_by_id(connection.to);
            if (edge.dst < 0) {
                continue;
            }
            edge.dst_param_name = connection.to_param;
        } else if (connection.to == "output") {
            edge.dst = kDstOutput;
        } else if (connection.to == "voiceOut") {
            edge.dst = kDstVoiceOut;
        } else {
            edge.dst = node_index_by_id(connection.to);
            if (edge.dst < 0) {
                continue;
            }
        }
        edges_.push_back(std::move(edge));
    }

    // DFS cycle-break (same policy as the workspace compiler): back
    // edges read the source's previous block.
    const std::size_t count = nodes_.size();
    std::vector<std::vector<std::size_t>> adjacency(count);
    for (std::size_t e = 0; e < edges_.size(); ++e) {
        if (edges_[e].src >= 0 && edges_[e].dst >= 0) {
            adjacency[static_cast<std::size_t>(edges_[e].src)].push_back(e);
        }
    }
    enum class Colour : std::uint8_t { kWhite, kGrey, kBlack };
    std::vector<Colour> colour(count, Colour::kWhite);
    std::vector<int> postorder;
    for (std::size_t start = 0; start < count; ++start) {
        if (colour[start] != Colour::kWhite) {
            continue;
        }
        std::vector<std::pair<int, std::size_t>> stack;
        colour[start] = Colour::kGrey;
        stack.emplace_back(static_cast<int>(start), 0);
        while (!stack.empty()) {
            auto& [node, cursor] = stack.back();
            const auto& out = adjacency[static_cast<std::size_t>(node)];
            if (cursor >= out.size()) {
                colour[static_cast<std::size_t>(node)] = Colour::kBlack;
                postorder.push_back(node);
                stack.pop_back();
                continue;
            }
            Edge& edge = edges_[out[cursor]];
            ++cursor;
            switch (colour[static_cast<std::size_t>(edge.dst)]) {
            case Colour::kWhite:
                colour[static_cast<std::size_t>(edge.dst)] = Colour::kGrey;
                stack.emplace_back(edge.dst, 0);
                break;
            case Colour::kGrey:
                edge.delayed = true;
                break;
            case Colour::kBlack:
                break;
            }
        }
    }
    order_.assign(postorder.rbegin(), postorder.rend());
}

NtpInstance::NodeInstantiation& NtpInstance::slot_for(int node_index, int voice_index) {
    GraphNode& node = nodes_[static_cast<std::size_t>(node_index)];
    if (!node.voice_scoped || voice_index < 0) {
        return node.slots[0];
    }
    return node.slots[static_cast<std::size_t>(voice_index)];
}

void NtpInstance::set_param(const std::string& key, float value) {
    for (auto& [existing, stored] : host_params_) {
        if (existing == key) {
            stored = value;
            break;
        }
    }
    const auto dot = key.find('.');
    if (dot == std::string::npos) {
        return;
    }
    const int node_index = node_index_by_id(key.substr(0, dot));
    if (node_index < 0) {
        return;
    }
    const std::string param_name = key.substr(dot + 1);
    for (NodeInstantiation& slot : nodes_[static_cast<std::size_t>(node_index)].slots) {
        if (ParamSlot* param = slot.runtime->find_param(param_name.c_str())) {
            param->base = value;
        }
    }
}

float NtpInstance::param_value(const std::string& key) const {
    for (const auto& [existing, stored] : host_params_) {
        if (existing == key) {
            return stored;
        }
    }
    return 0.0F;
}

void NtpInstance::plugin_set_param_cv(int param_index, float value01) {
    if (param_index < 0 || param_index >= static_cast<int>(cv_targets_.size())) {
        return;
    }
    const CvParamTarget& target = cv_targets_[static_cast<std::size_t>(param_index)];
    const float value = target.min + (value01 * (target.max - target.min));
    host_params_[static_cast<std::size_t>(param_index)].second = value;
    for (ParamSlot* slot : target.slots) {
        slot->base = value;
    }
}

void NtpInstance::apply_preset(const ntp::Preset& preset) {
    for (const auto& [key, value] : preset.values) {
        set_param(key, static_cast<float>(value));
    }
}

float NtpInstance::node_peak(const std::string& node_id) const {
    const int index = node_index_by_id(node_id);
    if (index < 0) {
        return 0.0F;
    }
    float peak = 0.0F;
    for (const NodeInstantiation& slot : nodes_[static_cast<std::size_t>(index)].slots) {
        peak = std::max(peak, slot.peak);
    }
    return peak;
}

bool NtpInstance::set_env_stage(const std::string& node_id, int stage_index, double target,
                                double time) {
    for (ntp::DspNode& node : plugin_->manifest.graph.nodes) {
        if (node.id != node_id || node.type != ntp::NodeType::kEnvelope) {
            continue;
        }
        if (stage_index < 0 || stage_index >= static_cast<int>(node.env_stages.size())) {
            return false;
        }
        ntp::EnvelopeStage& stage = node.env_stages[static_cast<std::size_t>(stage_index)];
        stage.target = std::clamp(target, 0.0, 1.0);
        stage.time = std::max(0.001, time);
        return true;
    }
    return false;
}

void NtpInstance::note_on(int note, float velocity) {
    // Shared samplers trigger regardless of the voice pool.
    for (GraphNode& node : nodes_) {
        if (plugin_->manifest.graph.nodes[static_cast<std::size_t>(node.def_index)].type ==
            ntp::NodeType::kSampler) {
            node.slots[0].runtime->sampler_note_on(note, velocity);
        }
    }
    if (!is_instrument_) {
        return;
    }

    Voice* slot = nullptr;
    for (Voice& voice : voices_) {
        if (!voice.active) {
            slot = &voice;
            break;
        }
    }
    if (slot == nullptr) {
        const std::string& policy = plugin_->manifest.voice_stealing;
        if (policy == "none") {
            return;
        }
        slot = voices_.data();
        for (Voice& voice : voices_) {
            if (voice.age < slot->age) {
                slot = &voice;
            }
        }
    }
    const int voice_index = static_cast<int>(slot - voices_.data());
    slot->active = true;
    slot->note = note;
    slot->age = ++voice_clock_;
    slot->fade = 1.0;
    slot->controls.note = static_cast<float>(note);
    slot->controls.pitch_hz = 440.0F * std::pow(2.0F, (static_cast<float>(note) - 69.0F) / 12.0F);
    slot->controls.velocity = velocity;
    slot->controls.gate = 1.0F;
    for (ModState& state : slot->mod_states) {
        state.current = 0.0F;
    }
    // Restart voice-scoped envelopes.
    for (GraphNode& node : nodes_) {
        if (node.voice_scoped) {
            node.slots[static_cast<std::size_t>(voice_index)].runtime->note_on();
        }
    }
}

void NtpInstance::note_off(int note) {
    for (GraphNode& node : nodes_) {
        if (plugin_->manifest.graph.nodes[static_cast<std::size_t>(node.def_index)].type ==
            ntp::NodeType::kSampler) {
            node.slots[0].runtime->sampler_note_off(note);
        }
    }
    for (Voice& voice : voices_) {
        if (voice.active && voice.note == note) {
            voice.controls.gate = 0.0F;
        }
    }
}

void NtpInstance::all_notes_off() {
    for (Voice& voice : voices_) {
        voice.controls.gate = 0.0F;
    }
    for (GraphNode& node : nodes_) {
        if (plugin_->manifest.graph.nodes[static_cast<std::size_t>(node.def_index)].type ==
            ntp::NodeType::kSampler) {
            for (int n = 0; n < 128; ++n) {
                node.slots[0].runtime->sampler_note_off(n);
            }
        }
    }
}

void NtpInstance::plugin_reset() {
    for (Voice& voice : voices_) {
        voice.active = false;
        voice.controls.gate = 0.0F;
    }
    for (GraphNode& node : nodes_) {
        const ntp::NodeType type =
            plugin_->manifest.graph.nodes[static_cast<std::size_t>(node.def_index)].type;
        if (type == ntp::NodeType::kSampler) {
            node.slots[0].runtime->sampler_reset();
        } else if (type == ntp::NodeType::kNativeStage) {
            node.slots[0].runtime->native_stage_reset();
        }
    }
}

std::vector<std::uint8_t> NtpInstance::native_stage_state(const std::string& node_id) {
    const int index = node_index_by_id(node_id);
    if (index < 0 || plugin_->manifest.graph.nodes[static_cast<std::size_t>(index)].type !=
                         ntp::NodeType::kNativeStage) {
        return {};
    }
    return nodes_[static_cast<std::size_t>(index)].slots[0].runtime->native_stage_state();
}

bool NtpInstance::set_native_stage_state(const std::string& node_id,
                                         const std::vector<std::uint8_t>& chunk) {
    const int index = node_index_by_id(node_id);
    if (index < 0 || plugin_->manifest.graph.nodes[static_cast<std::size_t>(index)].type !=
                         ntp::NodeType::kNativeStage) {
        return false;
    }
    return nodes_[static_cast<std::size_t>(index)].slots[0].runtime->set_native_stage_state(
        chunk.data(), chunk.size());
}

std::shared_ptr<audio::SampleBuffer> NtpInstance::set_slot_override(const std::string& slot_id,
                                                                    SlotOverride entry) {
    std::shared_ptr<audio::SampleBuffer> replaced;
    if (const auto it = slot_overrides_.find(slot_id); it != slot_overrides_.end()) {
        replaced = std::move(it->second.buffer);
    }
    // Samplers are always shared (slots[0]); the runtime fans the
    // pointer out to every zone/slice map carrying this slot id.
    for (GraphNode& node : nodes_) {
        if (plugin_->manifest.graph.nodes[static_cast<std::size_t>(node.def_index)].type ==
            ntp::NodeType::kSampler) {
            node.slots[0].runtime->set_slot_override_buffer(slot_id, entry.buffer.get());
        }
    }
    slot_overrides_[slot_id] = std::move(entry);
    return replaced;
}

std::shared_ptr<audio::SampleBuffer> NtpInstance::clear_slot_override(const std::string& slot_id) {
    const auto it = slot_overrides_.find(slot_id);
    if (it == slot_overrides_.end()) {
        return nullptr;
    }
    std::shared_ptr<audio::SampleBuffer> replaced = std::move(it->second.buffer);
    slot_overrides_.erase(it);
    for (GraphNode& node : nodes_) {
        if (plugin_->manifest.graph.nodes[static_cast<std::size_t>(node.def_index)].type ==
            ntp::NodeType::kSampler) {
            node.slots[0].runtime->set_slot_override_buffer(slot_id, nullptr);
        }
    }
    return replaced;
}

void NtpInstance::apply_mod_routes(int voice_index, std::uint32_t frames) {
    for (std::size_t b = 0; b < mod_bindings_.size(); ++b) {
        const ModBinding& binding = mod_bindings_[b];
        const bool voice_target = binding.target_voice_scoped == 1;
        if (voice_target != (voice_index >= 0)) {
            continue;
        }
        float source = 0.0F;
        switch (binding.source) {
        case ModBinding::Source::kNode: {
            if (binding.source_node < 0) {
                break;
            }
            NodeInstantiation& src = slot_for(binding.source_node, voice_index);
            source = block_average(src.prev.data(), frames);
            break;
        }
        case ModBinding::Source::kVelocity:
            source = voice_index >= 0
                         ? voices_[static_cast<std::size_t>(voice_index)].controls.velocity
                         : 1.0F;
            break;
        case ModBinding::Source::kNote:
            source = voice_index >= 0 ? voices_[static_cast<std::size_t>(voice_index)].controls.note
                                      : 60.0F;
            break;
        case ModBinding::Source::kGate:
            source = voice_index >= 0 ? voices_[static_cast<std::size_t>(voice_index)].controls.gate
                                      : 1.0F;
            break;
        case ModBinding::Source::kPitch:
            source = voice_index >= 0
                         ? voices_[static_cast<std::size_t>(voice_index)].controls.pitch_hz
                         : 440.0F;
            break;
        case ModBinding::Source::kParam:
            source = binding.param_index >= 0
                         ? host_params_[static_cast<std::size_t>(binding.param_index)].second
                         : 0.0F;
            break;
        }
        const float value = apply_transform((source * static_cast<float>(binding.def.scale)) +
                                                static_cast<float>(binding.def.offset),
                                            binding.def.transform) *
                            static_cast<float>(binding.def.depth);
        ModState& state = voice_index >= 0
                              ? voices_[static_cast<std::size_t>(voice_index)].mod_states[b]
                              : shared_mod_states_[b];
        if (binding.slew_coeff > 0.0F) {
            state.current += (value - state.current) * binding.slew_coeff;
        } else {
            state.current = value;
        }
        NodeInstantiation& target = slot_for(binding.target_node, voice_index);
        if (ParamSlot* param = target.runtime->find_param(binding.target_param.c_str())) {
            param->mod += state.current;
        }
    }
}

void NtpInstance::process_voice_pass(int voice_index, std::uint32_t frames) {
    const VoiceControls* controls =
        voice_index >= 0 ? &voices_[static_cast<std::size_t>(voice_index)].controls : nullptr;

    // Reset every pass node's param mods, apply the k-rate mod routes
    // once, then run the nodes in topological order.
    for (const int node_index : order_) {
        const GraphNode& node = nodes_[static_cast<std::size_t>(node_index)];
        if (node.voice_scoped != (voice_index >= 0)) {
            continue;
        }
        NodeInstantiation& slot = slot_for(node_index, voice_index);
        for (ParamSlot& param : slot.runtime->params()) {
            param.mod = 0.0F;
            param.audio_mod = nullptr;
        }
    }
    apply_mod_routes(voice_index, frames);

    for (const int node_index : order_) {
        const GraphNode& node = nodes_[static_cast<std::size_t>(node_index)];
        // Shared nodes run in the shared pass only; voice nodes in
        // voice passes only.
        if (node.voice_scoped != (voice_index >= 0)) {
            continue;
        }
        NodeInstantiation& slot = slot_for(node_index, voice_index);
        std::fill_n(slot.in_sum.data(), frames * 2, 0.0F);
        for (const Edge& edge : edges_) {
            if (edge.dst != node_index) {
                continue;
            }
            const float* signal = zero_block_.data();
            if (edge.src >= 0) {
                // A voice node reading a shared source sees its
                // previous block (voice passes run first) — consistent
                // with the mod-route latency model.
                GraphNode& src_node = nodes_[static_cast<std::size_t>(edge.src)];
                NodeInstantiation& src =
                    src_node.voice_scoped ? slot_for(edge.src, voice_index) : src_node.slots[0];
                const bool use_prev = edge.delayed || (voice_index >= 0 && !src_node.voice_scoped);
                signal = use_prev ? src.prev.data() : src.out.data();
            } else if (edge.src == kSrcVoiceMix) {
                // Only meaningful in the shared pass, after voices ran.
                signal = voice_index < 0 ? voice_mix_.data() : zero_block_.data();
            } else if (!is_instrument_) {
                signal = external_in_; // FX "input"; instrument voiceIn = silence
            }
            if (!edge.dst_param_name.empty()) {
                if (ParamSlot* param = slot.runtime->find_param(edge.dst_param_name.c_str())) {
                    param->audio_mod = signal;
                }
                continue;
            }
            for (std::uint32_t i = 0; i < frames * 2; ++i) {
                slot.in_sum[i] += signal[i];
            }
        }

        slot.runtime->process(slot.in_sum.data(), slot.out.data(), frames, controls);

        float peak = 0.0F;
        for (std::uint32_t i = 0; i < frames * 2; ++i) {
            peak = std::max(peak, std::abs(slot.out[i]));
        }
        slot.peak = peak;
    }
}

void NtpInstance::process(const float* in, float* out, std::uint32_t frames) {
    frames = std::min(frames, kNtpBlockFrames);
    external_in_ = in != nullptr ? in : zero_block_.data();
    std::fill_n(voice_mix_.data(), frames * 2, 0.0F);

    // ── Voice passes ─────────────────────────────────────────────────
    if (is_instrument_) {
        for (std::size_t v = 0; v < voices_.size(); ++v) {
            Voice& voice = voices_[v];
            if (!voice.active) {
                continue;
            }
            process_voice_pass(static_cast<int>(v), frames);

            // Collect this voice's voiceOut edges.
            float voice_gain = 1.0F;
            if (!has_voice_envelopes_ && voice.controls.gate < 0.5F) {
                // Built-in 30ms anti-click fade for envelope-less voices.
                voice.fade = std::max(0.0, voice.fade - (frames / (0.030 * rate_)));
                voice_gain = static_cast<float>(voice.fade);
            }
            for (const Edge& edge : edges_) {
                if (edge.dst != -2 || edge.src < 0) {
                    continue;
                }
                NodeInstantiation& src = slot_for(edge.src, static_cast<int>(v));
                for (std::uint32_t i = 0; i < frames * 2; ++i) {
                    voice_mix_[i] += src.out[i] * voice_gain;
                }
            }

            // Voice-end detection.
            bool envelopes_done = true;
            if (has_voice_envelopes_) {
                for (GraphNode& node : nodes_) {
                    if (node.voice_scoped && node.slots[v].runtime->envelope_active()) {
                        envelopes_done = false;
                        break;
                    }
                }
            } else {
                envelopes_done = voice.fade <= 0.0;
            }
            if (voice.controls.gate < 0.5F && envelopes_done) {
                voice.active = false;
            }
        }
    }

    // ── Shared pass ──────────────────────────────────────────────────
    process_voice_pass(-1, frames);

    // ── Output collection ────────────────────────────────────────────
    std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
    bool any_output_edge = false;
    for (const Edge& edge : edges_) {
        if (edge.dst != kDstOutput) {
            continue;
        }
        any_output_edge = true;
        const float* signal = nullptr;
        if (edge.src >= 0) {
            GraphNode& src_node = nodes_[static_cast<std::size_t>(edge.src)];
            if (src_node.voice_scoped) {
                continue; // voice nodes reach output via voiceOut
            }
            NodeInstantiation& src = src_node.slots[0];
            signal = edge.delayed ? src.prev.data() : src.out.data();
        } else if (edge.src == kSrcVoiceMix) {
            signal = voice_mix_.data();
        } else {
            signal = external_in_;
        }
        for (std::uint32_t i = 0; i < frames * 2; ++i) {
            out[i] += signal[i];
        }
    }
    // An instrument with no explicit output edges emits the voice mix
    // directly (the common oscillator→voiceOut manifest shape).
    if (is_instrument_ && !any_output_edge) {
        for (std::uint32_t i = 0; i < frames * 2; ++i) {
            out[i] += voice_mix_[i];
        }
    }

    // Roll blocks: current becomes previous for the next block.
    for (GraphNode& node : nodes_) {
        for (NodeInstantiation& slot : node.slots) {
            std::memcpy(slot.prev.data(), slot.out.data(),
                        static_cast<std::size_t>(frames) * 2 * sizeof(float));
        }
    }
}

} // namespace nt::plugins
