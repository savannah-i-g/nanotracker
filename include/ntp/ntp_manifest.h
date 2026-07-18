// ntp/ntp_manifest — the NTP v1 plugin manifest as plain data.
//
// An NTP plugin is a ZIP archive (`.ntins` instrument / `.ntsfx` FX)
// containing `plugin.json` plus assets (samples, wavetables, impulses,
// images). Everything in the archive is data: the DSP is a declarative
// node graph the host interprets, and the UI is declarative controls
// the host renders. Nothing in an archive executes — with one loud
// exception: a `native_stage` node names a compiled binary in the
// archive (ntp_stage_abi.h). Hosts label such archives distinctly so
// the trust decision is the user's, never silent.
//
// NTP has exactly one schema version (`"ntp": 1`). The web plugin
// format's v1-v5 migration debt is not inherited; a converter maps web
// manifests to this schema (tools/ntp-convert).
//
// This header is licensed MIT with the NTP plugin exception (see
// LICENSES/NTP-MIT.txt) so plugin tooling can embed it freely. It
// deliberately depends on the C++ standard library only.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ntp {

inline constexpr int kSchemaVersion = 1;

enum class PluginType : unsigned char { kInstrument, kFx };

// ── Parameters ───────────────────────────────────────────────────────

enum class ParamCurve : unsigned char { kLinear, kExponential, kLogarithmic };

struct ParamDef {
    std::string key;
    std::string label;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    double step = 0.0; // 0 = continuous
    std::string unit;
    int display_decimals = 2;
    std::string group;
    ParamCurve curve = ParamCurve::kLinear;
    bool midi_learnable = true;
};

// ── DSP graph ────────────────────────────────────────────────────────

// The built-in node set. `kNativeStage` is the C-ABI escape hatch
// (ntp_stage_abi.h): the node names a shared library shipped in the
// archive under binaries/<platform-tag>/. It is the one node kind that
// executes code — hosts label such archives distinctly in the UI.
enum class NodeType : unsigned char {
    kGain,
    kDelay,
    kBiquad,
    kCompressor,
    kConvolver,
    kPanner,
    kWaveshaper,
    kMixer,
    kOscillator,
    kNoise,
    kConstant,
    kLfo,
    kEnvelope,
    kGranular,
    kWavetable,
    kSampler,
    kNativeStage,
};

// Voice-scoped nodes are instantiated once per active note; shared
// nodes once per plugin instance. FX plugins are implicitly all-shared.
enum class NodeScope : unsigned char { kVoice, kShared };

struct EnvelopeStage {
    double target = 0.0; // 0-1 level
    double time = 0.0;   // seconds to reach target
    bool exponential = false;
};

enum class LoopMode : unsigned char { kNone, kForward, kPingpong, kRelease };

struct SampleZone {
    std::string file; // archive-relative
    int root_key = 60;
    int key_lo = 0;
    int key_hi = 127;
    int vel_lo = 1;
    int vel_hi = 127;
    LoopMode loop = LoopMode::kNone;
    double loop_start = 0.0; // seconds
    double loop_end = 0.0;
    double start_offset = 0.0;
    double duration = 0.0; // 0 = to end
    bool pitch_tracking = true;
    std::string round_robin_group; // same-group zones rotate
    std::string choke_group;       // same-group zones cut each other
    bool release_trigger = false;  // plays on noteOff

    // ── User-assignable slot (requires "userSamples" in requires[]) ──
    // A user-assignable zone exposes a slot the user drops their own
    // sample into at runtime; overrides persist per instance in the
    // .ftrk POVR block, keyed by `slot_id` and content hash. When no
    // override exists the zone plays `fallback_file` (required — a
    // fresh install must never play silence). `slot_id` must be stable
    // across plugin versions or prior saves cannot locate the slot.
    bool user_assignable = false;
    std::string slot_id;
    std::string slot_label;        // picker display; slot_id if empty
    std::string fallback_file;     // archive-relative default sample
    double max_duration_sec = 0.0; // 0 = no cap on user samples
};

// ── Slice maps (MPC-style chopping) ──────────────────────────────────

// The host's default note mapping for slices without an authored note:
// slice index i triggers on MIDI note kSliceBaseNote + i (GM kick + N).
inline constexpr int kSliceBaseNote = 36;

// "oneShot" plays a triggered slice to its end; "gated" cuts the voice
// on noteOff (per-slice `release_on_gate` opts a slice out).
enum class SliceTriggerMode : unsigned char { kOneShot, kGated };

struct SliceEntry {
    double start = 0.0; // seconds into the source buffer
    double end = 0.0;   // seconds; duration = end - start
    int note = -1;      // trigger note; -1 = kSliceBaseNote + index
    int velocity_min = 1;
    std::string choke_group;       // same-group voices cut each other
    std::string round_robin_group; // same-group slices rotate
    bool release_on_gate = true;   // gated mode only: cut on noteOff
};

// One slice map per sampler node. Slices are author-supplied or, via
// `auto_detect` ("grid:N"), expanded by the loader once the source
// duration is known. A node with both zones and a slice map uses both:
// zones handle key/vel-mapped playback, slices per-note chopping.
struct SliceMap {
    // Archive-relative sample path, or "slotId:<id>" to chop a
    // user-assignable slot (overrides then retarget the slices too).
    std::string source;
    std::vector<SliceEntry> slices;
    std::string auto_detect; // "" = author-supplied; "grid:N" expands
    SliceTriggerMode trigger_mode = SliceTriggerMode::kOneShot;
};

struct DspNode {
    std::string id;
    NodeType type = NodeType::kGain;
    NodeScope scope = NodeScope::kShared;

    // Type-specific initial values; unused fields keep defaults.
    double gain = 1.0;                   // gain / mixer / lfo depth scale
    double delay_time = 0.25;            // delay (seconds)
    double max_delay = 2.0;              // delay (seconds)
    std::string filter_type = "lowpass"; // biquad
    double frequency = 1000.0;           // biquad / oscillator (Hz)
    double q = 0.707;                    // biquad
    double threshold = -24.0;            // compressor (dB)
    double ratio = 12.0;
    double attack = 0.003; // compressor / envelope shorthand (seconds)
    double release = 0.25;
    double knee = 30.0;
    double pan = 0.0;                     // panner (-1..1)
    std::string shaper_curve = "sigmoid"; // waveshaper: sigmoid|clip|fold
    double drive = 1.0;
    std::string osc_type = "sine";    // oscillator: sine|square|sawtooth|triangle
    std::string noise_type = "white"; // noise: white|pink|brown
    double constant_value = 0.0;      // constant
    std::string lfo_shape = "sine";   // + sample-and-hold
    double lfo_rate = 1.0;            // Hz
    double lfo_depth = 1.0;
    std::vector<EnvelopeStage> env_stages; // envelope
    std::string sample_file;               // granular source
    std::string playback_mode = "forward"; // granular: +reverse|pingpong|freeze
    std::string grain_envelope = "hann";   // +triangle|rectangular
    std::string table_file;                // wavetable source
    int frame_count = 1;                   // wavetable
    bool interpolation_linear = true;      // wavetable
    std::string impulse;                   // convolver (archive path)
    bool normalize = true;                 // convolver
    std::vector<SampleZone> zones;         // sampler
    std::optional<SliceMap> slice_map;     // sampler
    int polyphony = 16;                    // sampler
    std::string voice_stealing = "oldest"; // sampler: +quietest|none

    // native_stage (JSON "stage"): archive binary basename — the
    // loader resolves binaries/<platform-tag>/<stage_name>.<ext>.
    // Native stages are instance-scoped (the ABI has no voice
    // concept); scope "voice" is refused at parse.
    std::string stage_name;
};

// Endpoints are node ids, or the reserved names "input" / "output"
// (FX bus in/out) and "voiceIn" / "voiceOut" (per-voice stubs in
// instrument graphs). `to_param` connects audio-rate into a parameter
// instead of the node's signal input.
struct Connection {
    std::string from;
    std::string to;
    std::string to_param;
};

enum class ModTransform : unsigned char {
    kNone,
    kInvert,
    kAbs,
    kSquare,
    kUnipolar,
    kBipolar,
};

struct ModTarget {
    std::string target; // "nodeId.param"
    double depth = 1.0;
    double offset = 0.0;
    double scale = 1.0;
    double slew = 0.0; // seconds, single-pole smoother
    ModTransform transform = ModTransform::kNone;
    ParamCurve curve = ParamCurve::kLinear;
};

// Sources: a node id (its output, downmixed), or one of the per-voice
// control signals "velocity" | "note" | "gate" | "pitch", or a host
// parameter as "param:<key>".
struct ModRoute {
    std::string source;
    std::vector<ModTarget> targets;
};

struct Graph {
    std::vector<DspNode> nodes;
    std::vector<Connection> connections;
    std::vector<ModRoute> mod_routes;
};

// ── Ports ────────────────────────────────────────────────────────────

enum class PortKind : unsigned char { kAudio, kSidechain, kCv, kGate, kMidi };

struct PortDef {
    std::string id; // stable across plugin versions
    std::string label;
    PortKind kind = PortKind::kAudio;
};

// ── UI ───────────────────────────────────────────────────────────────

enum class ControlType : unsigned char {
    kKnob,
    kSlider,
    kToggle,
    kSelect,
    kNumber,
    kXyPad,
    kEnvelopeEditor,
    kMeter,
    kLabel,
    kGroup,
    kImage,
    kSprite,
};

// A named animation on a sprite control's sheet: an explicit frame
// sequence (row-major indices into the frameW × frameH grid over the
// asset image) at `fps` (web DEFAULT_SPRITE_FPS = 10). Loop = the
// sprite's idle loop (the first looping animation plays untriggered);
// non-loop = a one-shot fired by controls carrying this name as their
// `animation` interaction key. Names are plugin-unique so interaction
// keys resolve unambiguously.
struct SpriteAnimation {
    std::string name;
    std::vector<int> frames;
    double fps = 10.0;
    bool loop = false;
};

struct UiControl {
    ControlType type = ControlType::kKnob;
    std::string parameter;   // bound param key
    std::string parameter_x; // xy_pad
    std::string parameter_y;
    std::string label;
    std::string node;                 // meter: DSP node id whose output it displays
    std::string asset;                // image/sprite id
    std::vector<std::string> options; // select
    std::vector<UiControl> children;  // group
    bool row_layout = true;           // group direction
    float width = 0.0F;               // pixel hints (0 = auto)
    float height = 0.0F;

    // ── Sprite sheets (sprite controls) ──────────────────────────────
    // Frame cell size in pixels; 0 = the whole image is one frame
    // (static sprites need nothing else). Required when animations
    // exist.
    int frame_width = 0;
    int frame_height = 0;
    std::vector<SpriteAnimation> animations;

    // Interaction key on value controls: the named one-shot animation
    // the host triggers when this control is interacted with. Empty =
    // no animation coupling (web pluginTypes.ts `animation`).
    std::string animation;
};

struct UiDef {
    std::vector<UiControl> controls;
    // True when the source manifest declared a webview UI. NTP has no
    // webview; the host renders the auto-param panel and says why.
    bool had_webview = false;
};

// ── Presets ──────────────────────────────────────────────────────────

struct Preset {
    std::string name;
    std::vector<std::pair<std::string, double>> values; // param key → value
};

// ── Manifest root ────────────────────────────────────────────────────

struct Manifest {
    int ntp = kSchemaVersion;
    std::string id; // reverse-DNS style recommended; unique per catalogue
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    PluginType type = PluginType::kFx;

    std::vector<ParamDef> params;

    // Instrument voice pool (ignored for FX).
    int voices = 8;
    std::string voice_stealing = "oldest"; // oldest|quietest|none
    double release_tail = 8.0;             // seconds

    Graph graph;

    std::vector<PortDef> inputs;
    std::vector<PortDef> outputs;

    UiDef ui;
    std::vector<Preset> presets;

    // Capability requirements (JSON "requires"; the field name avoids
    // the C++ keyword). v1 defines "userSamples" (user-assignable
    // sample slots); hosts refuse unknown entries with a clear message
    // (strict load, no degraded mode).
    std::vector<std::string> required_caps;
};

} // namespace ntp
