#include "audio/audio_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define NT_HAVE_SSE 1 // NOLINT(cppcoreguidelines-macro-usage) — feature-test macro
#endif

namespace nt::audio {

namespace {

// Denormal protection: flush-to-zero + denormals-are-zero on the
// render thread. Feedback and filter tails otherwise decay into
// denormal territory and burn cycles exactly when headroom matters.
void set_denormal_protection() {
#ifdef NT_HAVE_SSE
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — per-thread state
thread_local bool t_denormals_configured = false;

} // namespace

AudioEngine::AudioEngine() : device_(make_openal_device()) {
    plugin_note_.fill(-1);
    plugin_slot_.fill(0);
    midi_note_.fill(-1);
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start() {
    if (running_) {
        return true;
    }
    const bool ok = device_->start(
        48000, [this](float* interleaved, std::uint32_t frames) { render(interleaved, frames); });
    if (ok) {
        sample_rate_ = device_->info().sample_rate;
        running_ = true;
    }
    return ok;
}

bool AudioEngine::start_offline(std::uint32_t rate) {
    if (running_) {
        return false;
    }
    sample_rate_ = rate;
    running_ = true;
    return true;
}

void AudioEngine::stop() {
    if (running_) {
        device_->stop();
        running_ = false;
    }
}

void AudioEngine::send(const Command& command) {
    if (!commands_.push(command)) {
        dropped_commands_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioEngine::drain_commands() {
    Command command;
    while (commands_.pop(command)) {
        switch (command.type) {
        case Command::Type::kToneOn:
            tone_on_ = true;
            break;
        case Command::Type::kToneOff:
            tone_on_ = false;
            break;
        case Command::Type::kToneFreq:
            tone_freq_ = std::clamp(command.value, 20.0F, 20000.0F);
            break;
        case Command::Type::kSetTempo:
            // Live tempo edit. play_state_.bpm drives the per-tick
            // interval and .speed the ticks-per-row, so both take effect
            // from the next tick without disturbing the transport.
            if (command.aux_int > 0) {
                play_state_.bpm = std::clamp(command.aux_int, 20, 255);
            }
            if (command.aux_int2 > 0) {
                play_state_.speed = std::clamp(command.aux_int2, 1, 31);
            }
            break;
        case Command::Type::kPlaySample:
            voice_sample_ = command.sample;
            voice_pos_ = 0;
            break;
        case Command::Type::kStopSample:
            voice_sample_ = nullptr;
            break;
        case Command::Type::kSetBundle:
            bundle_ = command.bundle;
            // Running max: fenced publishes are FIFO from one sender,
            // but unfenced senders (serial 0) must not rewind the
            // reclamation clock (audio/sample_reclaim.h).
            bundle_serial_ = std::max(bundle_serial_, command.serial);
            transport_playing_ = false;
            transport_end_bound_ = 0;
            channel_mask_ = 0xFFFFFFFFU;
            // A full replacement may retire sample buffers, so every
            // voice class holding a SampleBuffer* resets with it —
            // channel voices, sequence-layer voices, the one-shot
            // preview, and plugin-internal voices (NTP sampler
            // zone/slice overrides) alike; the reclamation fence
            // counts on this happening in the same drain.
            for (ChannelVoice& v : voices_) {
                v.active = false;
            }
            for (SeqVoice& v : seq_voices_) {
                v.active = false;
            }
            voice_sample_ = nullptr;
            if (bundle_ != nullptr) {
                if (bundle_->graph != nullptr) {
                    bundle_->graph->reset_plugins();
                }
                for (GraphPluginBinding* binding : bundle_->plugin_by_slot) {
                    if (binding != nullptr) {
                        binding->plugin_reset();
                    }
                }
            }
            // MIDI carrier state lives as long as the bundle (the web
            // state's per-bus lifetime); held notes reset with it.
            midi_fx_state_.fill(EffectMidiState{});
            midi_note_.fill(-1);
            break;
        case Command::Type::kSwapBundle:
            // Live swap for cable/graph edits: the project pointer and
            // sample set are unchanged (sender contract), so transport
            // position, voices and play state all stay valid.
            bundle_ = command.bundle;
            bundle_serial_ = std::max(bundle_serial_, command.serial);
            break;
        case Command::Type::kTransportPlay:
            if (bundle_ != nullptr && bundle_->project != nullptr) {
                const auto& order_list = bundle_->project->order_list;
                const auto orders = static_cast<int>(order_list.size());
                play_state_ = engine::create_play_state(*bundle_->project);
                if (orders > 0) {
                    const int start = std::clamp(command.aux_int, 0, orders - 1);
                    play_state_.order_pos = start;
                    play_state_.pattern_index = order_list[static_cast<std::size_t>(start)];
                }
                transport_end_bound_ = std::clamp(command.aux_int2, 0, orders);
                play_state_.is_playing = true;
                transport_playing_ = true;
                frames_until_tick_ = 0.0; // first tick fires immediately
            }
            break;
        case Command::Type::kTransportStop:
            transport_playing_ = false;
            transport_end_bound_ = 0;
            for (ChannelVoice& v : voices_) {
                v.active = false;
            }
            break;
        case Command::Type::kSetChannelMask:
            channel_mask_ = static_cast<std::uint32_t>(command.aux_int);
            break;
        case Command::Type::kFxParam:
            if (bundle_ != nullptr && bundle_->fx_rack != nullptr) {
                const int ch = (command.aux_int >> 16) & 0xFFFF;
                const int mod = (command.aux_int >> 8) & 0xFF;
                const int param = command.aux_int & 0xFF;
                bundle_->fx_rack->set_param_index(ch, mod, param, command.value);
            }
            break;
        case Command::Type::kSetModule:
            module_ = command.module;
            module_playing_ = false;
            break;
        case Command::Type::kModulePlay:
            module_playing_ = module_ != nullptr && module_->loaded();
            break;
        case Command::Type::kModuleStop:
            module_playing_ = false;
            break;
        case Command::Type::kPluginNoteOn:
            if (bundle_ != nullptr) {
                const int slot = command.aux_int >> 8;
                const int note = command.aux_int & 0xFF;
                if (slot >= 1 && slot <= engine::kMaxSamples &&
                    bundle_->plugin_by_slot[static_cast<std::size_t>(slot)] != nullptr) {
                    bundle_->plugin_by_slot[static_cast<std::size_t>(slot)]->plugin_note_on(
                        note, command.value);
                }
            }
            break;
        case Command::Type::kPluginNoteOff:
            if (bundle_ != nullptr) {
                const int slot = command.aux_int >> 8;
                const int note = command.aux_int & 0xFF;
                if (slot >= 1 && slot <= engine::kMaxSamples &&
                    bundle_->plugin_by_slot[static_cast<std::size_t>(slot)] != nullptr) {
                    bundle_->plugin_by_slot[static_cast<std::size_t>(slot)]->plugin_note_off(note);
                }
            }
            break;
        case Command::Type::kPreviewNote:
            if (command.sample != nullptr && command.aux_int >= 0 &&
                command.aux_int < engine::kMaxChannels) {
                ChannelVoice& voice = voices_[static_cast<std::size_t>(command.aux_int)];
                voice.sample = command.sample;
                voice.pos = 0.0;
                voice.step = command.value;
                voice.gain = 0.75F;
                voice.pan = 0.5F;
                voice.active = true;
            }
            break;
        }
    }
}

void AudioEngine::handle_seq_triggers() {
    const engine::TrackerProject& project = *bundle_->project;
    for (int t = 0; t < play_state_.seq_trigger_count; ++t) {
        const engine::SequenceTrigger& trigger =
            play_state_.seq_triggers[static_cast<std::size_t>(t)];
        if (trigger.channel_index >= 0 && trigger.channel_index < engine::kMaxChannels &&
            (channel_mask_ >> static_cast<unsigned>(trigger.channel_index) & 1U) == 0U) {
            continue; // masked out (stems export)
        }
        const int slot = trigger.instrument;
        if (slot < 1 || slot > engine::kMaxSamples) {
            continue;
        }
        // Plugin instruments route through their bindings; sample
        // instruments get a layered voice from the pool.
        GraphPluginBinding* binding = bundle_->plugin_by_slot[static_cast<std::size_t>(slot)];
        if (binding != nullptr) {
            if (trigger.is_note_on) {
                binding->plugin_note_on(trigger.pitch,
                                        static_cast<float>(trigger.velocity) / 127.0F);
            } else {
                binding->plugin_note_off(trigger.pitch);
            }
            continue;
        }
        int sample_id = slot;
        if (!project.instrument_table.empty() &&
            slot <= static_cast<int>(project.instrument_table.size())) {
            sample_id = project.instrument_table[static_cast<std::size_t>(slot - 1)].sample_id;
        }
        if (!trigger.is_note_on) {
            for (SeqVoice& voice : seq_voices_) {
                if (voice.active && voice.pitch == trigger.pitch &&
                    voice.channel == trigger.channel_index && voice.layer == trigger.layer_index) {
                    voice.active = false;
                }
            }
            continue;
        }
        const SampleBuffer* sample =
            sample_id >= 1 && sample_id <= engine::kMaxSamples
                ? bundle_->samples_by_id[static_cast<std::size_t>(sample_id)]
                : nullptr;
        if (sample == nullptr || sample->frames == 0) {
            continue;
        }
        int base_note = 60;
        for (const engine::TrackerSample& s : project.samples) {
            if (s.id == sample_id) {
                base_note = s.base_note;
                break;
            }
        }
        SeqVoice* voice = nullptr;
        for (SeqVoice& candidate : seq_voices_) {
            if (!candidate.active) {
                voice = &candidate;
                break;
            }
        }
        if (voice == nullptr) {
            voice = seq_voices_.data();
            for (SeqVoice& candidate : seq_voices_) {
                if (candidate.age < voice->age) {
                    voice = &candidate;
                }
            }
        }
        voice->sample = sample;
        voice->pos = 0.0;
        voice->step = std::pow(2.0, static_cast<double>(trigger.pitch - base_note) / 12.0);
        voice->gain = static_cast<float>(trigger.velocity) / 127.0F;
        voice->pitch = trigger.pitch;
        voice->channel = trigger.channel_index;
        voice->layer = trigger.layer_index;
        voice->age = ++seq_voice_clock_;
        voice->active = true;
    }
}

void AudioEngine::render_seq_voices_span(std::uint32_t offset, std::uint32_t frames) {
    for (SeqVoice& voice : seq_voices_) {
        if (!voice.active || voice.sample == nullptr) {
            continue;
        }
        float* scratch = seq_scratch_.data() + (static_cast<std::size_t>(offset) * 2);
        const SampleBuffer& sample = *voice.sample;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const auto idx = static_cast<std::uint32_t>(voice.pos);
            if (idx + 1 >= sample.frames) {
                voice.active = false;
                break;
            }
            const auto frac = static_cast<float>(voice.pos - idx);
            const std::size_t s0 = static_cast<std::size_t>(idx) * 2;
            const float l = sample.interleaved[s0] +
                            ((sample.interleaved[s0 + 2] - sample.interleaved[s0]) * frac);
            const float r = sample.interleaved[s0 + 1] +
                            ((sample.interleaved[s0 + 3] - sample.interleaved[s0 + 1]) * frac);
            const std::size_t at = static_cast<std::size_t>(i) * 2;
            scratch[at] += l * voice.gain;
            scratch[at + 1] += r * voice.gain;
            voice.pos += voice.step;
        }
    }
}

void AudioEngine::handle_tick_outputs(std::uint32_t frame_offset) {
    const engine::TrackerProject& project = *bundle_->project;
    handle_seq_triggers();

    if (play_state_.fx_automation_cell != nullptr && bundle_->fx_rack != nullptr) {
        bundle_->fx_rack->apply_automation(*play_state_.fx_automation_cell);
    }
    const int channel_count = std::min(project.channels, engine::kMaxChannels);

    for (int ch = 0; ch < channel_count; ++ch) {
        if ((channel_mask_ >> static_cast<unsigned>(ch) & 1U) == 0U) {
            continue; // masked out (stems export): no triggers or follows
        }
        const engine::ChannelPlayState& c = play_state_.channels[static_cast<std::size_t>(ch)];
        ChannelVoice& voice = voices_[static_cast<std::size_t>(ch)];

        // ── Tracker-bus MIDI events ──────────────────────────────────
        // Row events onto the channel's per-block list, frame-stamped
        // at this tick. Order within the tick: release noteOff, then
        // (retrigger noteOff +) noteOn, then the row's effect
        // translation — the merge sort is stable, so this order
        // reaches cables. Channel stamp wraps like the web's
        // `channel & 0x0f`.
        {
            MidiEventList& events = channel_midi_[static_cast<std::size_t>(ch)];
            const auto midi_channel = static_cast<std::uint8_t>(ch & 0x0F);
            int& held = midi_note_[static_cast<std::size_t>(ch)];
            auto push_note_off = [&](int note) {
                events.push({.frame = frame_offset,
                             .type = MidiMessage::Type::kNoteOff,
                             .channel = midi_channel,
                             .data1 = static_cast<std::uint8_t>(note & 0x7F),
                             .data2 = 0});
            };
            if (c.release_note && held >= 0) {
                push_note_off(held);
                held = -1;
            }
            if (c.trigger_note && c.note > 0) {
                if (held >= 0) {
                    push_note_off(held); // pattern lane is monophonic
                }
                // Tracker note 1 = C-0 = MIDI 12 (the plugin path's
                // mapping); velocity from the tick's effective volume.
                const int midi = c.note + 11;
                const int vol = c.volume_override >= 0 ? c.volume_override : c.volume;
                const int velocity = std::clamp(
                    static_cast<int>(std::lround(static_cast<double>(vol) * 127.0 / 64.0)), 1, 127);
                events.push({.frame = frame_offset,
                             .type = MidiMessage::Type::kNoteOn,
                             .channel = midi_channel,
                             .data1 = static_cast<std::uint8_t>(midi & 0x7F),
                             .data2 = static_cast<std::uint8_t>(velocity)});
                held = midi;
            }
            if (play_state_.row_fired) {
                effect_to_midi(c.row_effect, c.row_effect_param, midi_channel, frame_offset,
                               midi_fx_state_[static_cast<std::size_t>(ch)], events);
            }
            // ECx note cut on its matching tick → noteOff (the IT
            // Sxx→noteOff counterpart; the engine mutes the voice, the
            // cable tells hardware to stop the note).
            if (held >= 0 && c.row_effect == 0xE && ((c.row_effect_param >> 4) & 0xF) == 0xC &&
                play_state_.tick == (c.row_effect_param & 0xF) && play_state_.tick > 0) {
                push_note_off(held);
                held = -1;
            }
        }

        if (c.release_note) {
            voice.active = false;
            if (plugin_note_[static_cast<std::size_t>(ch)] >= 0) {
                const int slot = plugin_slot_[static_cast<std::size_t>(ch)];
                GraphPluginBinding* binding =
                    slot >= 1 && slot <= engine::kMaxSamples
                        ? bundle_->plugin_by_slot[static_cast<std::size_t>(slot)]
                        : nullptr;
                if (binding != nullptr) {
                    binding->plugin_note_off(plugin_note_[static_cast<std::size_t>(ch)]);
                }
                plugin_note_[static_cast<std::size_t>(ch)] = -1;
            }
        }

        // Plugin instruments: notes route to the instance; its audio
        // renders inside the graph runner, not the channel voice.
        if (c.trigger_note && c.instrument_type == engine::InstrumentSourceType::kPlugin &&
            c.instrument >= 1 && c.instrument <= engine::kMaxSamples) {
            GraphPluginBinding* binding =
                bundle_->plugin_by_slot[static_cast<std::size_t>(c.instrument)];
            if (binding != nullptr) {
                if (plugin_note_[static_cast<std::size_t>(ch)] >= 0) {
                    binding->plugin_note_off(plugin_note_[static_cast<std::size_t>(ch)]);
                }
                // Tracker note 1 = C-0 = MIDI 12.
                const int midi = c.note + 11;
                const int vol = c.volume_override >= 0 ? c.volume_override : c.volume;
                binding->plugin_note_on(midi, static_cast<float>(vol) / 64.0F);
                plugin_note_[static_cast<std::size_t>(ch)] = midi;
                plugin_slot_[static_cast<std::size_t>(ch)] = c.instrument;
            }
        }

        if (c.trigger_note && c.instrument_type == engine::InstrumentSourceType::kSample) {
            // Resolve the sample id through the instrument table when
            // present (plugin/workspace instruments arrive with their
            // stages and stay silent here).
            int sample_id = c.instrument;
            if (!project.instrument_table.empty() && c.instrument >= 1 &&
                c.instrument <= static_cast<int>(project.instrument_table.size())) {
                sample_id =
                    project.instrument_table[static_cast<std::size_t>(c.instrument - 1)].sample_id;
            }
            const SampleBuffer* sample =
                (sample_id >= 1 && sample_id <= engine::kMaxSamples)
                    ? bundle_->samples_by_id[static_cast<std::size_t>(sample_id)]
                    : nullptr;
            if (sample != nullptr && sample->frames > 0) {
                const engine::TrackerSample* meta = nullptr;
                for (const engine::TrackerSample& s : project.samples) {
                    if (s.id == sample_id) {
                        meta = &s;
                        break;
                    }
                }
                const int base_note = meta != nullptr ? meta->base_note : 60;
                const double stretch = meta != nullptr ? meta->stretch_ratio : 1.0;

                voice.sample = sample;
                // 9xx offsets and loop points are stored in
                // source-rate frames; the resident buffer is
                // device-rate, so scale (fix #9: the voice only ever
                // works in frames of the buffer it plays).
                const double offset_scale =
                    sample->source_rate != 0
                        ? static_cast<double>(sample->rate) / sample->source_rate
                        : 1.0;
                voice.pos = std::min(static_cast<double>(sample->frames - 1),
                                     c.sample_offset * offset_scale);
                voice.step = engine::period_to_playback_rate(c.period, base_note) * stretch;
                if (meta != nullptr && meta->loop_length > 0) {
                    const double meta_scale =
                        meta->sample_rate != 0
                            ? static_cast<double>(sample->rate) / meta->sample_rate
                            : 1.0;
                    voice.loop_start = meta->loop_start * meta_scale;
                    voice.loop_end = std::min(static_cast<double>(sample->frames),
                                              (meta->loop_start + meta->loop_length) * meta_scale);
                } else {
                    voice.loop_start = 0.0;
                    voice.loop_end = 0.0;
                }
                voice.active = true;
            }
        }

        // Volume/pan follow the channel state every tick (slides,
        // tremolo overrides, set-volume).
        const int vol = c.volume_override >= 0 ? c.volume_override : c.volume;
        voice.gain = static_cast<float>(vol) / 64.0F;
        voice.pan = static_cast<float>(c.pan) / 255.0F;
        if (c.period_override > 0 || c.period > 0) {
            const engine::TrackerProject& p = project;
            int sample_id = c.instrument;
            if (!p.instrument_table.empty() && c.instrument >= 1 &&
                c.instrument <= static_cast<int>(p.instrument_table.size())) {
                sample_id =
                    p.instrument_table[static_cast<std::size_t>(c.instrument - 1)].sample_id;
            }
            const engine::TrackerSample* meta = nullptr;
            for (const engine::TrackerSample& s : p.samples) {
                if (s.id == sample_id) {
                    meta = &s;
                    break;
                }
            }
            const int base_note = meta != nullptr ? meta->base_note : 60;
            const double stretch = meta != nullptr ? meta->stretch_ratio : 1.0;
            const int period = c.period_override > 0 ? c.period_override : c.period;
            voice.step = engine::period_to_playback_rate(period, base_note) * stretch;
        }
    }
}

void AudioEngine::render_voices_span(std::uint32_t offset, std::uint32_t frames) {
    // Voices render into channel scratch at the span's block offset;
    // scratch was zeroed for the whole block in render_block.
    const int channel_count = bundle_ != nullptr && bundle_->project != nullptr
                                  ? std::min(bundle_->project->channels, engine::kMaxChannels)
                                  : engine::kMaxChannels;

    for (int ch = 0; ch < channel_count; ++ch) {
        ChannelVoice& voice = voices_[static_cast<std::size_t>(ch)];
        if (!voice.active || voice.sample == nullptr) {
            continue;
        }
        float* scratch = channel_scratch_[static_cast<std::size_t>(ch)].data() +
                         (static_cast<std::size_t>(offset) * 2);
        const SampleBuffer& sample = *voice.sample;
        const float gain_l = voice.gain * (1.0F - voice.pan);
        const float gain_r = voice.gain * voice.pan;

        for (std::uint32_t i = 0; i < frames; ++i) {
            if (voice.loop_end > voice.loop_start && voice.pos >= voice.loop_end) {
                voice.pos = voice.loop_start + (voice.pos - voice.loop_end);
            }
            const auto idx = static_cast<std::uint32_t>(voice.pos);
            if (idx + 1 >= sample.frames) {
                voice.active = false;
                break;
            }
            const auto frac = static_cast<float>(voice.pos - idx);
            const std::size_t s0 = static_cast<std::size_t>(idx) * 2;
            const float l = sample.interleaved[s0] +
                            (sample.interleaved[s0 + 2] - sample.interleaved[s0]) * frac;
            const float r = sample.interleaved[s0 + 1] +
                            (sample.interleaved[s0 + 3] - sample.interleaved[s0 + 1]) * frac;
            const std::size_t out = static_cast<std::size_t>(i) * 2;
            scratch[out] += l * gain_l;
            scratch[out + 1] += r * gain_r;
            voice.pos += voice.step;
        }
    }
}

void AudioEngine::advance_transport_in_block(std::uint32_t frames) {
    // Tick boundaries computed in frames: interval = rate * 2.5 / bpm.
    // A block spanning a boundary is split so triggers land
    // sample-accurately.
    std::uint32_t offset = 0;
    while (offset < frames) {
        if (transport_playing_ && frames_until_tick_ <= 0.0) {
            engine::advance_tick(play_state_, *bundle_->project);
            if (transport_end_bound_ > 0 &&
                (play_state_.song_ended || play_state_.order_pos >= transport_end_bound_)) {
                // Range end: mirror the web offline renderer's
                // break-before-schedule — this tick's outputs never
                // fire; the transport parks and voices ring out (the
                // export tail). song_ended stays latched for readers.
                play_state_.is_playing = false;
                play_state_.song_ended = true;
                transport_playing_ = false;
            } else {
                handle_tick_outputs(offset);
                frames_until_tick_ += static_cast<double>(sample_rate_) * 2.5 / play_state_.bpm;
            }
        }
        const auto span = transport_playing_
                              ? static_cast<std::uint32_t>(std::min<double>(
                                    frames - offset, std::max(1.0, std::ceil(frames_until_tick_))))
                              : frames - offset; // parked: ring out the rest of the block
        render_voices_span(offset, span);
        render_seq_voices_span(offset, span);
        if (transport_playing_) {
            frames_until_tick_ -= span;
        }
        offset += span;
    }
}

void AudioEngine::render(float* interleaved, std::uint32_t frames) {
    if (!t_denormals_configured) {
        set_denormal_protection();
        t_denormals_configured = true;
    }

    drain_commands();

    // Fixed-block interior loop: later stages advance the sequencer and
    // evaluate the patch graph per block, so pulls are split here.
    std::uint32_t offset = 0;
    while (offset < frames) {
        const std::uint32_t chunk = std::min(kBlockFrames, frames - offset);
        render_block(interleaved + (static_cast<std::size_t>(offset) * 2), chunk,
                     stream_frames_ + offset);
        offset += chunk;
    }

    // Publish block peaks for the UI meters.
    float peak_l = 0.0F;
    float peak_r = 0.0F;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i) * 2;
        peak_l = std::max(peak_l, std::abs(interleaved[idx]));
        peak_r = std::max(peak_r, std::abs(interleaved[idx + 1]));
    }

    ++pulls_;
    stream_frames_ += frames;
    EngineSnapshot& snap = snapshot_.write_slot();
    snap.stream_frames = stream_frames_;
    snap.host_time_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    snap.peak_l = peak_l;
    snap.peak_r = peak_r;
    snap.sample_rate = sample_rate_;
    snap.pulls = pulls_;
    snap.bundle_serial = bundle_serial_;
    snap.tone_active = tone_on_;
    snap.voice_active = voice_sample_ != nullptr;
    snap.transport_playing = transport_playing_;
    snap.module_playing = module_playing_;
    snap.song_ended = play_state_.song_ended;
    snap.order_pos = play_state_.order_pos;
    snap.pattern_index = play_state_.pattern_index;
    snap.row = play_state_.row;
    snap.tick = play_state_.tick;
    snap.bpm = play_state_.bpm;
    snap.speed = play_state_.speed;
    snapshot_.publish();
}

void AudioEngine::render_block(float* interleaved, std::uint32_t frames,
                               std::uint64_t block_start) {
    std::fill_n(interleaved, static_cast<std::size_t>(frames) * 2, 0.0F);

    if (tone_on_) {
        constexpr float kToneGain = 0.2F;
        const double step = 2.0 * std::numbers::pi * tone_freq_ / static_cast<double>(sample_rate_);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const auto s = static_cast<float>(std::sin(tone_phase_)) * kToneGain;
            tone_phase_ += step;
            const std::size_t idx = static_cast<std::size_t>(i) * 2;
            interleaved[idx] += s;
            interleaved[idx + 1] += s;
        }
        if (tone_phase_ > 2.0 * std::numbers::pi) {
            tone_phase_ = std::fmod(tone_phase_, 2.0 * std::numbers::pi);
        }
    }

    if (voice_sample_ != nullptr) {
        const SampleBuffer& sample = *voice_sample_;
        const std::uint32_t remaining = sample.frames - voice_pos_;
        const std::uint32_t n = std::min(frames, remaining);
        const float* src = sample.interleaved.data() + (static_cast<std::size_t>(voice_pos_) * 2);
        for (std::uint32_t i = 0; i < n * 2; ++i) {
            interleaved[i] += src[i];
        }
        voice_pos_ += n;
        if (voice_pos_ >= sample.frames) {
            voice_sample_ = nullptr; // one-shot playback ends
        }
    }

    // ── Channel stage ────────────────────────────────────────────────
    // Voices render into per-channel scratch for the whole block —
    // whether the transport drives them (spans between ticks) or they
    // free-run (note preview, tails after stop). Dry mix, FX sends and
    // the patch graph all read the same scratch afterwards.
    const int channel_count = bundle_ != nullptr && bundle_->project != nullptr
                                  ? std::min(bundle_->project->channels, engine::kMaxChannels)
                                  : engine::kMaxChannels;
    for (int ch = 0; ch < channel_count; ++ch) {
        std::fill_n(channel_scratch_[static_cast<std::size_t>(ch)].data(),
                    static_cast<std::size_t>(frames) * 2, 0.0F);
        channel_midi_[static_cast<std::size_t>(ch)].clear();
    }
    std::fill_n(seq_scratch_.data(), static_cast<std::size_t>(frames) * 2, 0.0F);
    if (transport_playing_ && bundle_ != nullptr && bundle_->project != nullptr) {
        advance_transport_in_block(frames);
    } else {
        render_voices_span(0, frames);
        render_seq_voices_span(0, frames); // tails after stop / previews
    }
    for (int ch = 0; ch < channel_count; ++ch) {
        const ChannelVoice& voice = voices_[static_cast<std::size_t>(ch)];
        channel_gains_[static_cast<std::size_t>(ch)] = voice.active ? voice.gain : 0.0F;
    }

    bool have_module_block = false;
    if (module_playing_ && module_ != nullptr) {
        std::fill_n(module_scratch_.data(), static_cast<std::size_t>(frames) * 2, 0.0F);
        if (!module_->render(module_scratch_.data(), frames, sample_rate_)) {
            module_playing_ = false; // module reached its end
        }
        have_module_block = true;
    }

    // Hardware MIDI arrivals for Ext MIDI In nodes: drain the input
    // ring once per block (single consumer). Arrival is quantised to
    // this block's start (frame 0) — the render clock runs ahead of
    // wall time by the device latency, so reconstructing sub-block
    // offsets from host timestamps would be fake precision; honest
    // jitter is ≤ one block (~2.7ms at 48k) plus device latency.
    ext_midi_in_events_.clear();
    if (rt::SpscQueue<MidiMessage>* source = ext_midi_source_.load(std::memory_order_acquire)) {
        MidiMessage message;
        while (source->pop(message)) {
            message.frame = 0;
            ext_midi_in_events_.push(message); // overflow counted, not UB
        }
    }

    // ── Patch graph ──────────────────────────────────────────────────
    // Evaluates cables: tracker-bus/module sources → utility nodes →
    // Master In, which accumulates into this block's output. Also
    // yields the reroute suppression decisions for the dry mix below.
    std::uint32_t suppressed_mask = 0;
    bool module_suppressed = false;
    if (bundle_ != nullptr && bundle_->graph != nullptr) {
        std::array<const float*, engine::kMaxChannels> scratch_ptrs{};
        std::array<const MidiEventList*, engine::kMaxChannels> midi_ptrs{};
        for (int ch = 0; ch < channel_count; ++ch) {
            scratch_ptrs[static_cast<std::size_t>(ch)] =
                channel_scratch_[static_cast<std::size_t>(ch)].data();
            midi_ptrs[static_cast<std::size_t>(ch)] = &channel_midi_[static_cast<std::size_t>(ch)];
        }
        const GraphBlockContext ctx{
            .channel_scratch = scratch_ptrs.data(),
            .channel_count = channel_count,
            .channel_gains = channel_gains_.data(),
            .module_block = have_module_block ? module_scratch_.data() : nullptr,
            .master_accum = interleaved,
            .channel_midi = midi_ptrs.data(),
            .ext_midi_in = &ext_midi_in_events_,
            .ext_midi_out = &ext_midi_out_,
            .bus_midi_in = &bus_midi_in_,
            .block_start_frame = block_start,
            .sample_rate = sample_rate_,
        };
        bundle_->graph->process(ctx, frames);
        suppressed_mask = bundle_->graph->suppressed_channel_mask();
        module_suppressed = bundle_->graph->module_suppressed();
    }

    // ── Dry mix ──────────────────────────────────────────────────────
    // Channels with a reroute cable from their bus jack are suppressed
    // (their audio flows only through cables); same for the module
    // player's direct path.
    for (int ch = 0; ch < channel_count; ++ch) {
        if ((suppressed_mask >> static_cast<unsigned>(ch) & 1U) != 0U) {
            continue;
        }
        const float* scratch = channel_scratch_[static_cast<std::size_t>(ch)].data();
        for (std::uint32_t i = 0; i < frames * 2; ++i) {
            interleaved[i] += scratch[i];
        }
    }
    if (have_module_block && !module_suppressed) {
        for (std::uint32_t i = 0; i < frames * 2; ++i) {
            interleaved[i] += module_scratch_[i];
        }
    }
    // Sequence-layer voices mix straight to master (layered on top of
    // the channel paths, like the web's per-layer instrument routing).
    for (std::uint32_t i = 0; i < frames * 2; ++i) {
        interleaved[i] += seq_scratch_[i];
    }

    // FX sends → rack → mix. Sends read the pre-cable channel scratch
    // (pre-fader send semantics survive rerouting, matching the web's
    // preSendTap position).
    if (bundle_ != nullptr && bundle_->fx_rack != nullptr) {
        std::array<const float*, engine::kMaxChannels> pointers{};
        for (int ch = 0; ch < channel_count; ++ch) {
            pointers[static_cast<std::size_t>(ch)] =
                channel_scratch_[static_cast<std::size_t>(ch)].data();
        }
        bundle_->fx_rack->process(pointers.data(), channel_count, interleaved, frames);
    }

    // Master section: gentle bus compression (WebAudio-default feel)
    // and DC blocking, mirroring the web master chain's always-on tail
    // (trackerAudio.ts master gain→compressor→…→destination).
    if (!master_configured_) {
        master_compressor_.configure(sample_rate_);
        master_compressor_.set(-24.0F, 12.0F, 0.003F, 0.25F, 30.0F);
        master_configured_ = true;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::size_t at = static_cast<std::size_t>(i) * 2;
        const float g = master_compressor_.gain_for(interleaved[at], interleaved[at + 1]);
        interleaved[at] = master_dc_l_.process(interleaved[at] * g);
        interleaved[at + 1] = master_dc_r_.process(interleaved[at + 1] * g);
    }
}

} // namespace nt::audio
