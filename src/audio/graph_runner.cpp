#include "audio/graph_runner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace nt::audio {

namespace {

constexpr std::uint32_t kStereoFloats = kGraphBlockFrames * 2;

// CV→param smoothing: single-pole slew toward the block average, the
// same smoother shape NTP mod routes use (ntp_voices.cpp slew_coeff).
// Mod-route slew times come from the plugin manifest; a cable has no
// author, so a fixed 10ms applies to every CV param port.
constexpr float kCvParamSlewSeconds = 0.010F;

} // namespace

GraphRunner::GraphRunner(const graph::WorkspaceGraph& model, graph::GraphSchedule schedule)
    : schedule_(std::move(schedule)) {
    const std::vector<graph::Node>& nodes = model.nodes();
    nodes_.resize(nodes.size());

    // Buffer pool layout: every audio output gets 2×stereo blocks
    // (cur/prev), every cv output 2×mono; audio inputs one stereo sum
    // block, cv inputs one mono block. Gate inputs get an event list.
    int floats = 0;
    auto take = [&floats](int count) {
        const int at = floats;
        floats += count;
        return at;
    };

    for (std::size_t n = 0; n < nodes.size(); ++n) {
        NodeSlot& slot = nodes_[n];
        slot.kind = nodes[n].kind;
        slot.volume = nodes[n].volume;
        slot.pan = nodes[n].pan;
        slot.bypass = nodes[n].bypass;
        for (const graph::Port& port : nodes[n].inputs) {
            PortSlot p;
            p.kind = port.kind;
            p.channel_ref = port.channel_ref;
            p.param_ref = port.param_ref;
            switch (port.kind) {
            case graph::PortKind::kAudio:
            case graph::PortKind::kSidechain:
                p.buf_a = take(static_cast<int>(kStereoFloats));
                break;
            case graph::PortKind::kCv:
                p.buf_a = take(static_cast<int>(kGraphBlockFrames));
                break;
            case graph::PortKind::kGate:
                p.gate_list = static_cast<int>(gate_lists_.size());
                gate_lists_.emplace_back();
                break;
            case graph::PortKind::kMidi:
                p.midi_a = static_cast<int>(midi_lists_.size());
                midi_lists_.emplace_back();
                break;
            }
            slot.inputs.push_back(p);
        }
        for (const graph::Port& port : nodes[n].outputs) {
            PortSlot p;
            p.kind = port.kind;
            p.channel_ref = port.channel_ref;
            switch (port.kind) {
            case graph::PortKind::kAudio:
            case graph::PortKind::kSidechain:
                p.buf_a = take(static_cast<int>(kStereoFloats));
                p.buf_b = take(static_cast<int>(kStereoFloats));
                break;
            case graph::PortKind::kCv:
                p.buf_a = take(static_cast<int>(kGraphBlockFrames));
                p.buf_b = take(static_cast<int>(kGraphBlockFrames));
                break;
            case graph::PortKind::kGate:
                break;
            case graph::PortKind::kMidi:
                p.midi_a = static_cast<int>(midi_lists_.size());
                midi_lists_.emplace_back();
                p.midi_b = static_cast<int>(midi_lists_.size());
                midi_lists_.emplace_back();
                break;
            }
            slot.outputs.push_back(p);
        }
    }
    pool_.assign(static_cast<std::size_t>(floats), 0.0F);
    gate_high_.assign(schedule_.edges.size(), false);
    for (const int suppressed : schedule_.suppressed_plugin_nodes) {
        if (suppressed >= 0 && suppressed < static_cast<int>(nodes_.size())) {
            nodes_[static_cast<std::size_t>(suppressed)].master_suppressed = true;
        }
    }

    // Edges arrive sorted by dst_node; record each node's range and
    // mark cabled inputs (CV→param delivery gates on `driven`).
    for (NodeSlot& slot : nodes_) {
        slot.first_edge = 0;
        slot.edge_count = 0;
    }
    for (std::size_t e = 0; e < schedule_.edges.size(); ++e) {
        const graph::ScheduleEdge& edge = schedule_.edges[e];
        NodeSlot& dst = nodes_[static_cast<std::size_t>(edge.dst_node)];
        if (dst.edge_count == 0) {
            dst.first_edge = static_cast<int>(e);
        }
        ++dst.edge_count;
        if (edge.dst_port >= 0 && edge.dst_port < static_cast<int>(dst.inputs.size())) {
            dst.inputs[static_cast<std::size_t>(edge.dst_port)].driven = true;
        }
    }
}

float* GraphRunner::out_cur(const PortSlot& slot) {
    return pool_.data() + (flip_ ? slot.buf_b : slot.buf_a);
}

const float* GraphRunner::out_prev(const PortSlot& slot) const {
    return pool_.data() + (flip_ ? slot.buf_a : slot.buf_b);
}

MidiEventList& GraphRunner::midi_out_cur(const PortSlot& slot) {
    return midi_lists_[static_cast<std::size_t>(flip_ ? slot.midi_b : slot.midi_a)];
}

const MidiEventList& GraphRunner::midi_out_prev(const PortSlot& slot) const {
    return midi_lists_[static_cast<std::size_t>(flip_ ? slot.midi_a : slot.midi_b)];
}

void GraphRunner::gather_inputs(const NodeSlot& node, std::uint32_t frames) {
    // Clear this node's input buffers, then sum every inbound edge.
    for (const PortSlot& in : node.inputs) {
        if (in.buf_a >= 0) {
            const std::uint32_t count =
                in.kind == graph::PortKind::kCv ? kGraphBlockFrames : kStereoFloats;
            std::fill_n(pool_.data() + in.buf_a, count, 0.0F);
        }
        if (in.gate_list >= 0) {
            gate_lists_[static_cast<std::size_t>(in.gate_list)].count = 0;
        }
        if (in.kind == graph::PortKind::kMidi && in.midi_a >= 0) {
            midi_lists_[static_cast<std::size_t>(in.midi_a)].clear();
        }
    }

    for (int e = node.first_edge; e < node.first_edge + node.edge_count; ++e) {
        const graph::ScheduleEdge& edge = schedule_.edges[static_cast<std::size_t>(e)];
        const PortSlot& src = nodes_[static_cast<std::size_t>(edge.src_node)]
                                  .outputs[static_cast<std::size_t>(edge.src_port)];
        const PortSlot& dst = node.inputs[static_cast<std::size_t>(edge.dst_port)];
        if (dst.kind == graph::PortKind::kMidi) {
            // midi → midi (the only legal pairing): append the source
            // list — fan-in resolves below with one stable frame sort.
            if (src.midi_a < 0 || dst.midi_a < 0) {
                continue;
            }
            const MidiEventList& events = edge.delayed ? midi_out_prev(src) : midi_out_cur(src);
            MidiEventList& sink = midi_lists_[static_cast<std::size_t>(dst.midi_a)];
            for (int i = 0; i < events.count; ++i) {
                sink.push(events.events[static_cast<std::size_t>(i)]);
            }
            sink.dropped += events.dropped; // propagate upstream overflow
            continue;
        }
        if (src.buf_a < 0) {
            continue;
        }
        const float* signal = edge.delayed ? out_prev(src) : out_cur(src);

        if (dst.buf_a >= 0 && dst.kind != graph::PortKind::kCv &&
            src.kind != graph::PortKind::kCv) {
            // audio/sidechain → audio/sidechain: stereo sum.
            float* sum = pool_.data() + dst.buf_a;
            for (std::uint32_t i = 0; i < frames * 2; ++i) {
                sum[i] += signal[i];
            }
        } else if (dst.kind == graph::PortKind::kCv) {
            float* sum = pool_.data() + dst.buf_a;
            if (src.kind == graph::PortKind::kCv) {
                for (std::uint32_t i = 0; i < frames; ++i) {
                    sum[i] += signal[i];
                }
            } else {
                // audio → cv downmix.
                for (std::size_t i = 0; i < frames; ++i) {
                    sum[i] += (signal[i * 2] + signal[(i * 2) + 1]) * 0.5F;
                }
            }
        } else if (dst.kind == graph::PortKind::kGate && dst.gate_list >= 0) {
            // audio → gate: block-accurate edge extraction with a 0.5
            // threshold and release hysteresis at 0.4.
            GateEventList& list = gate_lists_[static_cast<std::size_t>(dst.gate_list)];
            bool high = gate_high_[static_cast<std::size_t>(e)];
            for (std::size_t i = 0; i < frames; ++i) {
                const float mono =
                    src.kind == graph::PortKind::kCv
                        ? signal[i]
                        : std::max(std::abs(signal[i * 2]), std::abs(signal[(i * 2) + 1]));
                const bool next = high ? mono >= 0.4F : mono >= 0.5F;
                if (next != high && list.count < kMaxGateEventsPerBlock) {
                    list.events[static_cast<std::size_t>(list.count)] = {
                        .frame = static_cast<std::uint32_t>(i), .on = next};
                    ++list.count;
                }
                high = next;
            }
            gate_high_[static_cast<std::size_t>(e)] = high;
        }
    }

    // Merge order: sources appended in schedule-edge order, then a
    // stable sort by frame — ties keep edge order (deterministic).
    for (const PortSlot& in : node.inputs) {
        if (in.kind == graph::PortKind::kMidi && in.midi_a >= 0) {
            sort_by_frame(midi_lists_[static_cast<std::size_t>(in.midi_a)]);
        }
    }
}

void GraphRunner::run_node(NodeSlot& node, const GraphBlockContext& ctx, std::uint32_t frames) {
    switch (node.kind) {
    case graph::NodeKind::kTrackerBus:
        for (const PortSlot& out : node.outputs) {
            const bool have_channel = out.channel_ref >= 0 && out.channel_ref < ctx.channel_count;
            if (out.kind == graph::PortKind::kMidi) {
                // chNN.midi mirrors the channel's row events;
                // master.midi (channel_ref < 0) merges every channel —
                // events already carry their channel stamp, so
                // concatenate in channel order and sort by frame.
                MidiEventList& cur = midi_out_cur(out);
                cur.clear();
                if (ctx.channel_midi != nullptr) {
                    if (have_channel) {
                        if (ctx.channel_midi[out.channel_ref] != nullptr) {
                            cur = *ctx.channel_midi[out.channel_ref];
                        }
                    } else {
                        for (int ch = 0; ch < ctx.channel_count; ++ch) {
                            const MidiEventList* list = ctx.channel_midi[ch];
                            for (int i = 0; list != nullptr && i < list->count; ++i) {
                                cur.push(list->events[static_cast<std::size_t>(i)]);
                            }
                        }
                        sort_by_frame(cur);
                    }
                }
                continue;
            }
            if (out.buf_a < 0) {
                continue;
            }
            float* cur = out_cur(out);
            const bool have_scratch = have_channel && ctx.channel_scratch != nullptr;
            if (out.kind == graph::PortKind::kAudio) {
                if (have_scratch) {
                    std::memcpy(cur, ctx.channel_scratch[out.channel_ref],
                                static_cast<std::size_t>(frames) * 2 * sizeof(float));
                } else {
                    std::fill_n(cur, frames * 2, 0.0F);
                }
                if (frames < kGraphBlockFrames) {
                    std::fill_n(cur + (static_cast<std::size_t>(frames) * 2),
                                (kGraphBlockFrames - frames) * 2, 0.0F);
                }
            } else if (out.kind == graph::PortKind::kCv) {
                const float gain = have_scratch && ctx.channel_gains != nullptr
                                       ? ctx.channel_gains[out.channel_ref]
                                       : 0.0F;
                std::fill_n(cur, kGraphBlockFrames, gain);
            }
        }
        // master.midi.in → the engine's record/step-entry ring, with
        // absolute stream-frame stamps. Delivery is the whole hook —
        // the web installed no inbound handler either (onInboundMidi
        // had no caller); the UI half consumes the ring later.
        if (ctx.bus_midi_in != nullptr) {
            for (const PortSlot& in : node.inputs) {
                if (in.kind != graph::PortKind::kMidi || in.midi_a < 0) {
                    continue;
                }
                const MidiEventList& list = midi_lists_[static_cast<std::size_t>(in.midi_a)];
                for (int i = 0; i < list.count; ++i) {
                    const MidiMessage& message = list.events[static_cast<std::size_t>(i)];
                    ctx.bus_midi_in->push(
                        {.message = message, .at_frame = ctx.block_start_frame + message.frame});
                    // Ring full = dropped; bounded by construction.
                }
            }
        }
        break;
    case graph::NodeKind::kModulePlayer:
        for (const PortSlot& out : node.outputs) {
            if (out.buf_a < 0 || out.kind != graph::PortKind::kAudio) {
                continue;
            }
            float* cur = out_cur(out);
            if (ctx.module_block != nullptr) {
                std::memcpy(cur, ctx.module_block,
                            static_cast<std::size_t>(frames) * 2 * sizeof(float));
                if (frames < kGraphBlockFrames) {
                    std::fill_n(cur + (static_cast<std::size_t>(frames) * 2),
                                (kGraphBlockFrames - frames) * 2, 0.0F);
                }
            } else {
                std::fill_n(cur, kStereoFloats, 0.0F);
            }
        }
        break;
    case graph::NodeKind::kMasterIn:
        if (ctx.master_accum != nullptr) {
            for (const PortSlot& in : node.inputs) {
                if (in.buf_a < 0 || in.kind != graph::PortKind::kAudio) {
                    continue;
                }
                const float* sum = pool_.data() + in.buf_a;
                for (std::uint32_t i = 0; i < frames * 2; ++i) {
                    ctx.master_accum[i] += sum[i];
                }
            }
        }
        break;
    case graph::NodeKind::kUtilitySum:
        // in → out pass-through; port 0 each side by construction.
        if (!node.inputs.empty() && !node.outputs.empty() && node.inputs[0].buf_a >= 0 &&
            node.outputs[0].buf_a >= 0) {
            float* cur = out_cur(node.outputs[0]);
            std::memcpy(cur, pool_.data() + node.inputs[0].buf_a,
                        static_cast<std::size_t>(frames) * 2 * sizeof(float));
            if (frames < kGraphBlockFrames) {
                std::fill_n(cur + (static_cast<std::size_t>(frames) * 2),
                            (kGraphBlockFrames - frames) * 2, 0.0F);
            }
        }
        break;
    case graph::NodeKind::kExtMidiIn:
        // Every Ext In node mirrors the app's one open input device
        // (the engine drained the ring into ctx.ext_midi_in).
        for (const PortSlot& out : node.outputs) {
            if (out.kind == graph::PortKind::kMidi && out.midi_a >= 0) {
                MidiEventList& cur = midi_out_cur(out);
                if (ctx.ext_midi_in != nullptr) {
                    cur = *ctx.ext_midi_in;
                } else {
                    cur.clear();
                }
            }
        }
        break;
    case graph::NodeKind::kExtMidiOut:
        // Cable events → wire bytes with absolute stream timestamps;
        // the MIDI out thread dispatches them against its PLL clock.
        if (ctx.ext_midi_out != nullptr) {
            for (const PortSlot& in : node.inputs) {
                if (in.kind != graph::PortKind::kMidi || in.midi_a < 0) {
                    continue;
                }
                const MidiEventList& list = midi_lists_[static_cast<std::size_t>(in.midi_a)];
                for (int i = 0; i < list.count; ++i) {
                    const MidiMessage& message = list.events[static_cast<std::size_t>(i)];
                    ExtMidiOutMessage wire;
                    wire.size = encode_midi_message(message, wire.bytes);
                    wire.at_sample = ctx.block_start_frame + message.frame;
                    ctx.ext_midi_out->push(wire); // full ring drops (bounded)
                }
            }
        }
        break;
    case graph::NodeKind::kPlugin: {
        if (node.plugin == nullptr) {
            for (const PortSlot& out : node.outputs) {
                if (out.buf_a >= 0 && out.kind == graph::PortKind::kAudio) {
                    std::fill_n(out_cur(out), kStereoFloats, 0.0F);
                }
            }
            break;
        }
        // Cabled midi-in ports feed the binding's note surface before
        // the block renders. Notes apply at block head (the bindings'
        // note staging carries no sub-block offset — same quantisation
        // as sequencer triggers); CC/pitch-bend have no binding
        // surface yet and are consumed silently here.
        for (const PortSlot& slot : node.inputs) {
            if (slot.kind != graph::PortKind::kMidi || slot.midi_a < 0) {
                continue;
            }
            const MidiEventList& list = midi_lists_[static_cast<std::size_t>(slot.midi_a)];
            for (int i = 0; i < list.count; ++i) {
                const MidiMessage& message = list.events[static_cast<std::size_t>(i)];
                if (message.type == MidiMessage::Type::kNoteOn) {
                    node.plugin->plugin_note_on(message.data1,
                                                static_cast<float>(message.data2) / 127.0F);
                } else if (message.type == MidiMessage::Type::kNoteOff) {
                    node.plugin->plugin_note_off(message.data1);
                }
            }
        }
        // Cabled CV param ports: block average → 10ms slew → binding
        // param setter, normalized 0..1 (each binding maps into its
        // own parameter domain). Uncabled ports never fire.
        for (PortSlot& slot : node.inputs) {
            if (slot.kind != graph::PortKind::kCv || slot.param_ref < 0 || !slot.driven ||
                slot.buf_a < 0 || frames == 0) {
                continue;
            }
            const float* cv = pool_.data() + slot.buf_a;
            float sum = 0.0F;
            for (std::uint32_t i = 0; i < frames; ++i) {
                sum += cv[i];
            }
            const float target = std::clamp(sum / static_cast<float>(frames), 0.0F, 1.0F);
            const auto rate = static_cast<float>(ctx.sample_rate > 0 ? ctx.sample_rate : 48000);
            const float coeff =
                1.0F - std::exp(-static_cast<float>(frames) / (kCvParamSlewSeconds * rate));
            slot.cv_state += (target - slot.cv_state) * coeff;
            node.plugin->plugin_set_param_cv(slot.param_ref, slot.cv_state);
        }
        // First audio in feeds the instance; first audio out carries it.
        const float* in = nullptr;
        for (const PortSlot& slot : node.inputs) {
            if (slot.kind == graph::PortKind::kAudio && slot.buf_a >= 0) {
                in = pool_.data() + slot.buf_a;
                break;
            }
        }
        float* out = nullptr;
        const PortSlot* out_slot = nullptr;
        for (const PortSlot& slot : node.outputs) {
            if (slot.kind == graph::PortKind::kAudio && slot.buf_a >= 0) {
                out_slot = &slot;
                out = out_cur(slot);
                break;
            }
        }
        if (out == nullptr) {
            break;
        }
        node.plugin->process_block(in, out, frames);
        if (frames < kGraphBlockFrames) {
            std::fill_n(out + (static_cast<std::size_t>(frames) * 2),
                        (kGraphBlockFrames - frames) * 2, 0.0F);
        }
        // Default master route with the node's volume/pan strip. Cable
        // taps stay pre-strip, like the web's jack-vs-panel-knob split
        // (trackerEngine.ts:167-173).
        if (ctx.master_accum != nullptr && !node.bypass && !node.master_suppressed &&
            out_slot != nullptr) {
            // Equal-power pan (centre = 0.707 per side), scaled by the
            // strip volume. sqrt(2) restores unity at centre.
            const float angle =
                (std::clamp(node.pan, -1.0F, 1.0F) + 1.0F) * 0.25F * std::numbers::pi_v<float>;
            const float gain_l = node.volume * std::cos(angle) * std::numbers::sqrt2_v<float>;
            const float gain_r = node.volume * std::sin(angle) * std::numbers::sqrt2_v<float>;
            for (std::uint32_t i = 0; i < frames; ++i) {
                ctx.master_accum[static_cast<std::size_t>(i) * 2] +=
                    out[static_cast<std::size_t>(i) * 2] * gain_l;
                ctx.master_accum[(static_cast<std::size_t>(i) * 2) + 1] +=
                    out[(static_cast<std::size_t>(i) * 2) + 1] * gain_r;
            }
        }
        break;
    }
    }
}

void GraphRunner::process(const GraphBlockContext& ctx, std::uint32_t frames) {
    frames = std::min(frames, kGraphBlockFrames);
    for (const int node_index : schedule_.order) {
        NodeSlot& node = nodes_[static_cast<std::size_t>(node_index)];
        gather_inputs(node, frames);
        run_node(node, ctx, frames);
    }
    flip_ = !flip_;
}

void GraphRunner::bind_plugin(int node_index, GraphPluginBinding* binding) {
    if (node_index >= 0 && node_index < static_cast<int>(nodes_.size())) {
        nodes_[static_cast<std::size_t>(node_index)].plugin = binding;
    }
}

const float* GraphRunner::debug_output(int node, int port) const {
    const PortSlot& slot =
        nodes_[static_cast<std::size_t>(node)].outputs[static_cast<std::size_t>(port)];
    // process() flipped at the end; the block just written lives on the
    // "previous" side now.
    return out_prev(slot);
}

const float* GraphRunner::debug_audio_input(int node, int port) const {
    const PortSlot& slot =
        nodes_[static_cast<std::size_t>(node)].inputs[static_cast<std::size_t>(port)];
    return pool_.data() + slot.buf_a;
}

const GateEventList& GraphRunner::debug_gate_input(int node, int port) const {
    const PortSlot& slot =
        nodes_[static_cast<std::size_t>(node)].inputs[static_cast<std::size_t>(port)];
    return gate_lists_[static_cast<std::size_t>(slot.gate_list)];
}

const MidiEventList& GraphRunner::debug_midi_input(int node, int port) const {
    const PortSlot& slot =
        nodes_[static_cast<std::size_t>(node)].inputs[static_cast<std::size_t>(port)];
    return midi_lists_[static_cast<std::size_t>(slot.midi_a)];
}

const MidiEventList& GraphRunner::debug_midi_output(int node, int port) const {
    const PortSlot& slot =
        nodes_[static_cast<std::size_t>(node)].outputs[static_cast<std::size_t>(port)];
    // Same convention as debug_output: the block just written lives on
    // the "previous" side after process() flipped.
    return midi_out_prev(slot);
}

} // namespace nt::audio
