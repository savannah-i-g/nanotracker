#include "audio/graph_runner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace nt::audio {

namespace {

constexpr std::uint32_t kStereoFloats = kGraphBlockFrames * 2;

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
                break; // message transport lands with the MIDI stage
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
            case graph::PortKind::kMidi:
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

    // Edges arrive sorted by dst_node; record each node's range.
    for (NodeSlot& slot : nodes_) {
        slot.first_edge = 0;
        slot.edge_count = 0;
    }
    for (std::size_t e = 0; e < schedule_.edges.size(); ++e) {
        NodeSlot& dst = nodes_[static_cast<std::size_t>(schedule_.edges[e].dst_node)];
        if (dst.edge_count == 0) {
            dst.first_edge = static_cast<int>(e);
        }
        ++dst.edge_count;
    }
}

float* GraphRunner::out_cur(const PortSlot& slot) {
    return pool_.data() + (flip_ ? slot.buf_b : slot.buf_a);
}

const float* GraphRunner::out_prev(const PortSlot& slot) const {
    return pool_.data() + (flip_ ? slot.buf_a : slot.buf_b);
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
    }

    for (int e = node.first_edge; e < node.first_edge + node.edge_count; ++e) {
        const graph::ScheduleEdge& edge = schedule_.edges[static_cast<std::size_t>(e)];
        const PortSlot& src = nodes_[static_cast<std::size_t>(edge.src_node)]
                                  .outputs[static_cast<std::size_t>(edge.src_port)];
        const PortSlot& dst = node.inputs[static_cast<std::size_t>(edge.dst_port)];
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
}

void GraphRunner::run_node(const NodeSlot& node, const GraphBlockContext& ctx,
                           std::uint32_t frames) {
    switch (node.kind) {
    case graph::NodeKind::kTrackerBus:
        for (const PortSlot& out : node.outputs) {
            if (out.buf_a < 0) {
                continue;
            }
            float* cur = out_cur(out);
            const bool have_channel = out.channel_ref >= 0 && out.channel_ref < ctx.channel_count &&
                                      ctx.channel_scratch != nullptr;
            if (out.kind == graph::PortKind::kAudio) {
                if (have_channel) {
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
                const float gain = have_channel && ctx.channel_gains != nullptr
                                       ? ctx.channel_gains[out.channel_ref]
                                       : 0.0F;
                std::fill_n(cur, kGraphBlockFrames, gain);
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
    case graph::NodeKind::kPlugin: {
        if (node.plugin == nullptr) {
            for (const PortSlot& out : node.outputs) {
                if (out.buf_a >= 0 && out.kind == graph::PortKind::kAudio) {
                    std::fill_n(out_cur(out), kStereoFloats, 0.0F);
                }
            }
            break;
        }
        // First audio in feeds the instance; first audio out carries
        // it. Further typed ports stay visible but inert until the
        // stages that consume them (cv → host params, midi transport).
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
        const NodeSlot& node = nodes_[static_cast<std::size_t>(node_index)];
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

} // namespace nt::audio
