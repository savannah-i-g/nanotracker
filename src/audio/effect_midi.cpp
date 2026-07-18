#include "audio/effect_midi.h"

#include <algorithm>
#include <cmath>

namespace nt::audio {

namespace {

// IT letter codes, matching the web's EFX_* constants (A=1..Z=26).
enum : std::uint8_t {
    kEfxNone = 0,
    kEfxD = 4,  // volume slide → CC 7 delta
    kEfxE = 5,  // portamento down → pitch-bend delta
    kEfxF = 6,  // portamento up → pitch-bend delta
    kEfxG = 7,  // tone portamento → pitch-bend step
    kEfxM = 13, // set channel volume → CC 7 absolute
    kEfxV = 22, // set global volume → CC 7 absolute
    kEfxP = 16, // panning slide → CC 10 delta
    kEfxX = 24, // set panning → CC 10 absolute
};

// ── Exact ports of the web helpers (trackerEffectToMidi.ts) ──────────

// IT volume-slide encoding: high nibble up, low nibble down; 0xFx/0xxF
// are fine slides — same delta magnitude for MIDI either way. The
// branch order matches the web exactly (0xFF lands in "fine down").
int volume_slide_delta(int raw) {
    const int hi = (raw >> 4) & 0x0F;
    const int lo = raw & 0x0F;
    if (hi == 0xF && lo > 0) {
        return -lo; // fine slide down
    }
    if (lo == 0xF && hi > 0) {
        return hi; // fine slide up
    }
    if (hi > 0 && lo == 0) {
        return hi; // up
    }
    if (lo > 0 && hi == 0) {
        return -lo; // down
    }
    return 0;
}

// Gxx step: 0..255 → signed, 32 bend units per step (web comment's
// empirical tuning).
int signed_bend_step(int v) {
    return (v - 128) * 32;
}

int clamp7(int v) {
    return std::clamp(v, 0, 127);
}

int clamp_pb(int v) {
    return std::clamp(v, -8192, 8191);
}

// JS Math.round (half away from zero for the non-negative inputs
// this path sees).
int value_to_cc7(int v, int max) {
    return static_cast<int>(
        std::floor((static_cast<double>(v) / static_cast<double>(max) * 127.0) + 0.5));
}

// The mapper core: exact port of mapEffectToMidi's switch. `code` is
// an IT letter constant above; events append to `list`.
void map_it_effect(int code, int value, std::uint8_t channel, std::uint32_t frame,
                   EffectMidiState& state, MidiEventList& list) {
    auto push_cc = [&](std::uint8_t controller, int cc_value) {
        list.push({.frame = frame,
                   .type = MidiMessage::Type::kControlChange,
                   .channel = channel,
                   .data1 = controller,
                   .data2 = static_cast<std::uint8_t>(cc_value)});
    };

    switch (code) {
    case kEfxD: { // volume slide (Dxx; Nxx/Wxx share the shape)
        const int next = clamp7(state.last_vol_cc + volume_slide_delta(value));
        if (next != state.last_vol_cc) {
            state.last_vol_cc = next;
            push_cc(7, next);
        }
        break;
    }
    case kEfxE: { // portamento down (coarse)
        const int bend = clamp_pb(state.last_pitch_bend - (value * 16));
        if (bend != state.last_pitch_bend) {
            state.last_pitch_bend = bend;
            list.push(make_pitch_bend(frame, channel, bend));
        }
        break;
    }
    case kEfxF: { // portamento up (coarse)
        const int bend = clamp_pb(state.last_pitch_bend + (value * 16));
        if (bend != state.last_pitch_bend) {
            state.last_pitch_bend = bend;
            list.push(make_pitch_bend(frame, channel, bend));
        }
        break;
    }
    case kEfxG: { // tone portamento — single step per row (web Phase 2)
        const int bend = clamp_pb(state.last_pitch_bend + signed_bend_step(value));
        if (bend != state.last_pitch_bend) {
            state.last_pitch_bend = bend;
            list.push(make_pitch_bend(frame, channel, bend));
        }
        break;
    }
    case kEfxM: { // set channel volume, 0..64
        const int next = clamp7(value_to_cc7(value, 64));
        if (next != state.last_vol_cc) {
            state.last_vol_cc = next;
            push_cc(7, next);
        }
        break;
    }
    case kEfxV: { // set global volume, 0..128 (emitted per channel)
        const int next = clamp7(value_to_cc7(value, 128));
        if (next != state.last_vol_cc) {
            state.last_vol_cc = next;
            push_cc(7, next);
        }
        break;
    }
    case kEfxP: { // panning slide
        const int next = clamp7(state.last_pan_cc + volume_slide_delta(value));
        if (next != state.last_pan_cc) {
            state.last_pan_cc = next;
            push_cc(10, next);
        }
        break;
    }
    case kEfxX: { // set panning wide, 0..255 → CC 10 0..127
        const int next =
            clamp7(static_cast<int>(std::floor((static_cast<double>(value) / 2.0) + 0.5)));
        if (next != state.last_pan_cc) {
            state.last_pan_cc = next;
            push_cc(10, next);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

void effect_to_midi(int effect, int param, std::uint8_t channel, std::uint32_t frame,
                    EffectMidiState& state, MidiEventList& list) {
    // Native MOD nibble → IT-letter semantics. Slide-compound effects
    // keep their slide half like the web's Kxx/Lxx entries (LFO /
    // porta halves skipped); 0x0/0x4/0x7/0x9/0xB/0xD and the remaining
    // 0xE subeffects match the mapper's intentionally-unmapped list.
    // 0xF is transport (speed/tempo — see header for the Txx call).
    switch (effect) {
    case 0x1: // porta up → Fxx
        map_it_effect(kEfxF, param, channel, frame, state, list);
        break;
    case 0x2: // porta down → Exx
        map_it_effect(kEfxE, param, channel, frame, state, list);
        break;
    case 0x3: // tone porta → Gxx
        map_it_effect(kEfxG, param, channel, frame, state, list);
        break;
    case 0x5: // tone porta + volume slide → Lxx's slide half (Dxx)
    case 0x6: // vibrato + volume slide → Kxx's slide half (Dxx)
    case 0xA: // volume slide → Dxx
        map_it_effect(kEfxD, param, channel, frame, state, list);
        break;
    case 0x8: // set panning 0..255 → Xxx
        map_it_effect(kEfxX, param, channel, frame, state, list);
        break;
    case 0xC: // set volume 0..64 → Mxx
        map_it_effect(kEfxM, param, channel, frame, state, list);
        break;
    case 0xE: {
        // Extended: fine volume slides re-encode into the IT fine
        // nibbles Dxx already understands. EAx = fine up x → 0xxF |
        // hi=x; EBx = fine down x → 0xFx | lo=x.
        const int sub = (param >> 4) & 0xF;
        const int val = param & 0xF;
        if (sub == 0xA && val > 0) {
            map_it_effect(kEfxD, (val << 4) | 0x0F, channel, frame, state, list);
        } else if (sub == 0xB && val > 0) {
            map_it_effect(kEfxD, 0xF0 | val, channel, frame, state, list);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace nt::audio
