#include "ext/vst3_host.h"

#include "ext/vst3_run_loop.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <public.sdk/source/common/memorystream.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <public.sdk/source/vst/hosting/processdata.h>
#include <public.sdk/source/vst/utility/stringconvert.h>

namespace nt::ext {

namespace {

using namespace Steinberg;

// The host context: the SDK's HostApplication surface, extended on
// Linux to answer IRunLoop — the factory-context exposure route
// (Research/07: a plugin must reach the run loop through
// IPluginFactory3::setHostContext even before it has an editor; the
// IPlugFrame route lives in ext/vst3_editor_window).
class Vst3HostContext final : public Vst::HostApplication {
public:
    tresult PLUGIN_API queryInterface(const char* interface_id, void** obj) override {
#ifndef _WIN32
        // COM seam: FUID converts to a TUID char array for iidEqual.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        if (FUnknownPrivate::iidEqual(interface_id, Linux::IRunLoop::iid)) {
            return Vst3RunLoop::instance().queryInterface(interface_id, obj);
        }
#endif
        return Vst::HostApplication::queryInterface(interface_id, obj);
    }
};

// The plugin context every PlugProvider consumes. One per process.
Vst3HostContext& host_application() {
    static Vst3HostContext instance;
    return instance;
}

} // namespace

// ── Vst3Module ───────────────────────────────────────────────────────

struct Vst3Module::Impl {
    VST3::Hosting::Module::Ptr module;
};

std::unique_ptr<Vst3Module> Vst3Module::open(const std::filesystem::path& path,
                                             std::string& error) {
    Vst::PluginContextFactory::instance().setPluginContext(&host_application());

    std::string load_error;
    VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(path.string(), load_error);
    if (!module) {
        error = load_error.empty() ? "module load failed" : load_error;
        return nullptr;
    }
    // PlugProvider only passes the context to component/controller
    // initialize; IPluginFactory3::setHostContext is a separate,
    // documented route plugins use to find the run loop and must be
    // fed explicitly (no-op for factories below IPluginFactory3).
    module->getFactory().setHostContext(&host_application());

    auto self = std::unique_ptr<Vst3Module>(new Vst3Module());
    self->impl_ = std::make_unique<Impl>();
    self->impl_->module = module;
    self->path_ = path;
    for (const VST3::Hosting::ClassInfo& info : module->getFactory().classInfos()) {
        if (info.category() != kVstAudioEffectClass) {
            continue;
        }
        Descriptor descriptor;
        descriptor.id = info.ID().toString();
        descriptor.name = info.name();
        descriptor.vendor = info.vendor();
        descriptor.category = info.subCategoriesString();
        descriptor.is_instrument = descriptor.category.find("Instrument") != std::string::npos;
        self->descriptors_.push_back(std::move(descriptor));
    }
    if (self->descriptors_.empty()) {
        error = "module exposes no audio-effect classes";
        return nullptr;
    }
    return self;
}

Vst3Module::~Vst3Module() = default;

// ── Vst3Plugin ───────────────────────────────────────────────────────

struct Vst3Plugin::Impl {
    VST3::Hosting::Module::Ptr module; // keeps the .so alive
    IPtr<Vst::PlugProvider> provider;
    Vst::IComponent* component = nullptr;
    Vst::IEditController* controller = nullptr;
    FUnknownPtr<Vst::IAudioProcessor> processor;
    Vst::HostProcessData process_data;
    Vst::ProcessContext process_context{};
    Vst::EventList input_events;
    Vst::ParameterChanges input_params;
    Vst::ParameterChanges output_params;
    bool processing = false;
};

std::unique_ptr<Vst3Plugin> Vst3Module::create(const std::string& class_id, std::uint32_t rate,
                                               std::string& error) {
    // classInfos() returns the list by value — bind it so `chosen`
    // outlives the loop.
    const auto class_infos = impl_->module->getFactory().classInfos();
    const VST3::Hosting::ClassInfo* chosen = nullptr;
    for (const VST3::Hosting::ClassInfo& info : class_infos) {
        if (info.category() == kVstAudioEffectClass && info.ID().toString() == class_id) {
            chosen = &info;
            break;
        }
    }
    if (chosen == nullptr) {
        error = "class \"" + class_id + "\" not in module";
        return nullptr;
    }

    auto self = std::unique_ptr<Vst3Plugin>(new Vst3Plugin());
    self->impl_ = std::make_unique<Vst3Plugin::Impl>();
    self->impl_->module = impl_->module;
    self->impl_->provider =
        owned(new Vst::PlugProvider(impl_->module->getFactory(), *chosen, true));
    if (!self->impl_->provider->initialize()) {
        error = "plug provider initialisation failed (component/controller)";
        return nullptr;
    }
    self->impl_->component = self->impl_->provider->getComponent();
    self->impl_->controller = self->impl_->provider->getController();
    self->impl_->processor = FUnknownPtr<Vst::IAudioProcessor>(self->impl_->component);
    if (!self->impl_->processor) {
        error = "component has no IAudioProcessor";
        return nullptr;
    }
    self->name_ = chosen->name();

    // Bus configuration: main stereo out (+ stereo in when present).
    const int32 in_busses = self->impl_->component->getBusCount(Vst::kAudio, Vst::kInput);
    const int32 out_busses = self->impl_->component->getBusCount(Vst::kAudio, Vst::kOutput);
    if (out_busses < 1) {
        error = "plugin has no audio output bus";
        return nullptr;
    }
    self->has_audio_input_ = in_busses > 0;
    Vst::SpeakerArrangement stereo = Vst::SpeakerArr::kStereo;
    std::vector<Vst::SpeakerArrangement> ins(static_cast<std::size_t>(in_busses), stereo);
    std::vector<Vst::SpeakerArrangement> outs(static_cast<std::size_t>(out_busses), stereo);
    self->impl_->processor->setBusArrangements(ins.data(), in_busses, outs.data(), out_busses);
    for (int32 b = 0; b < in_busses; ++b) {
        self->impl_->component->activateBus(Vst::kAudio, Vst::kInput, b, b == 0);
    }
    for (int32 b = 0; b < out_busses; ++b) {
        self->impl_->component->activateBus(Vst::kAudio, Vst::kOutput, b, b == 0);
    }
    const int32 event_busses = self->impl_->component->getBusCount(Vst::kEvent, Vst::kInput);
    for (int32 b = 0; b < event_busses; ++b) {
        self->impl_->component->activateBus(Vst::kEvent, Vst::kInput, b, b == 0);
    }

    Vst::ProcessSetup setup{};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(audio::kGraphBlockFrames);
    setup.sampleRate = rate;
    if (self->impl_->processor->setupProcessing(setup) != kResultOk) {
        error = "setupProcessing refused";
        return nullptr;
    }
    if (!self->impl_->process_data.prepare(*self->impl_->component,
                                           static_cast<int32>(audio::kGraphBlockFrames),
                                           Vst::kSample32)) {
        error = "process data preparation failed";
        return nullptr;
    }
    self->impl_->process_context.sampleRate = rate;
    self->impl_->process_context.state = Vst::ProcessContext::kPlaying;
    self->impl_->process_data.processContext = &self->impl_->process_context;
    self->impl_->process_data.inputEvents = &self->impl_->input_events;
    self->impl_->process_data.inputParameterChanges = &self->impl_->input_params;
    self->impl_->process_data.outputParameterChanges = &self->impl_->output_params;

    if (self->impl_->component->setActive(true) != kResultOk) {
        error = "setActive refused";
        return nullptr;
    }

    // Parameters from the edit controller (normalized domain).
    if (self->impl_->controller != nullptr) {
        const int32 count = self->impl_->controller->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            Vst::ParameterInfo info{};
            if (self->impl_->controller->getParameterInfo(i, info) != kResultOk) {
                continue;
            }
            if ((info.flags & Vst::ParameterInfo::kIsReadOnly) != 0) {
                continue;
            }
            Vst3ParamInfo param;
            param.id = info.id;
            param.title = VST3::StringConvert::convert(info.title);
            param.def_normalized = info.defaultNormalizedValue;
            param.automatable = (info.flags & Vst::ParameterInfo::kCanAutomate) != 0;
            self->param_cache_.emplace_back(info.id,
                                            self->impl_->controller->getParamNormalized(info.id));
            self->params_.push_back(std::move(param));
        }
    }

    self->pending_notes_.reserve(64);
    self->pending_cv_params_.reserve(64);
    return self;
}

Vst3Plugin::~Vst3Plugin() {
    if (impl_ != nullptr && impl_->component != nullptr) {
        if (impl_->processing) {
            impl_->processor->setProcessing(false);
        }
        impl_->component->setActive(false);
    }
    // provider->terminate() runs in PlugProvider's destructor chain.
}

double Vst3Plugin::param_value(std::uint32_t id) const {
    for (const auto& [existing, value] : param_cache_) {
        if (existing == id) {
            return value;
        }
    }
    return 0.0;
}

void Vst3Plugin::set_param(std::uint32_t id, double normalized) {
    for (auto& [existing, cached] : param_cache_) {
        if (existing == id) {
            cached = normalized;
        }
    }
    if (impl_->controller != nullptr) {
        impl_->controller->setParamNormalized(id, normalized);
    }
    param_changes_.push({.id = id, .value = normalized});
}

void Vst3Plugin::plugin_note_on(int note, float velocity) {
    if (pending_notes_.size() < pending_notes_.capacity()) {
        pending_notes_.push_back({.note = note, .velocity = velocity, .on = true});
    }
}

void Vst3Plugin::plugin_set_param_cv(int param_index, float value01) {
    if (param_index < 0 || param_index >= static_cast<int>(params_.size()) ||
        pending_cv_params_.size() >= pending_cv_params_.capacity()) {
        return; // full block staging — drop rather than allocate
    }
    pending_cv_params_.push_back({.id = params_[static_cast<std::size_t>(param_index)].id,
                                  .value = static_cast<double>(value01)});
}

void Vst3Plugin::plugin_note_off(int note) {
    if (pending_notes_.size() < pending_notes_.capacity()) {
        pending_notes_.push_back({.note = note, .velocity = 0.0F, .on = false});
    }
}

void Vst3Plugin::process_block(const float* in, float* out, std::uint32_t frames) {
    frames = std::min(frames, audio::kGraphBlockFrames);
    Impl& impl = *impl_;
    if (!impl.processing) {
        impl.processing = impl.processor->setProcessing(true) == kResultOk;
    }

    impl.input_events.clear();
    for (const NoteEvent& note : pending_notes_) {
        Vst::Event event{};
        event.busIndex = 0;
        event.sampleOffset = 0;
        event.flags = Vst::Event::kIsLive;
        if (note.on) {
            event.type = Vst::Event::kNoteOnEvent;
            event.noteOn.channel = 0;
            event.noteOn.pitch = static_cast<int16>(note.note);
            event.noteOn.velocity = note.velocity;
            event.noteOn.noteId = -1;
        } else {
            event.type = Vst::Event::kNoteOffEvent;
            event.noteOff.channel = 0;
            event.noteOff.pitch = static_cast<int16>(note.note);
            event.noteOff.velocity = 0.0F;
            event.noteOff.noteId = -1;
        }
        impl.input_events.addEvent(event);
    }
    pending_notes_.clear();

    impl.input_params.clearQueue();
    ParamChange change{};
    while (param_changes_.pop(change)) {
        int32 queue_index = 0;
        Vst::IParamValueQueue* queue = impl.input_params.addParameterData(change.id, queue_index);
        if (queue != nullptr) {
            int32 point_index = 0;
            queue->addPoint(0, change.value, point_index);
        }
    }
    for (const ParamChange& cv : pending_cv_params_) {
        int32 queue_index = 0;
        Vst::IParamValueQueue* queue = impl.input_params.addParameterData(cv.id, queue_index);
        if (queue != nullptr) {
            int32 point_index = 0;
            queue->addPoint(0, cv.value, point_index);
        }
    }
    pending_cv_params_.clear();

    // Interleaved → the SDK-managed planar buffers.
    impl.process_data.numSamples = static_cast<int32>(frames);
    if (impl.process_data.numInputs > 0 &&
        impl.process_data.inputs[0].channelBuffers32 != nullptr) {
        float* in_l = impl.process_data.inputs[0].channelBuffers32[0];
        float* in_r = impl.process_data.inputs[0].numChannels > 1
                          ? impl.process_data.inputs[0].channelBuffers32[1]
                          : in_l;
        for (std::uint32_t i = 0; i < frames; ++i) {
            in_l[i] = in != nullptr ? in[static_cast<std::size_t>(i) * 2] : 0.0F;
            in_r[i] = in != nullptr ? in[(static_cast<std::size_t>(i) * 2) + 1] : 0.0F;
        }
    }

    const tresult result = impl.processor->process(impl.process_data);
    impl.output_params.clearQueue();

    if (result != kResultOk || impl.process_data.numOutputs < 1 ||
        impl.process_data.outputs[0].channelBuffers32 == nullptr) {
        std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
        return;
    }
    const float* out_l = impl.process_data.outputs[0].channelBuffers32[0];
    const float* out_r = impl.process_data.outputs[0].numChannels > 1
                             ? impl.process_data.outputs[0].channelBuffers32[1]
                             : out_l;
    for (std::uint32_t i = 0; i < frames; ++i) {
        out[static_cast<std::size_t>(i) * 2] = out_l[i];
        out[(static_cast<std::size_t>(i) * 2) + 1] = out_r[i];
    }
}

Steinberg::IPlugView* Vst3Plugin::create_editor_view() const {
    if (impl_->controller == nullptr) {
        return nullptr;
    }
    return impl_->controller->createView(Vst::ViewType::kEditor);
}

std::vector<std::uint8_t> Vst3Plugin::save_state() const {
    std::vector<std::uint8_t> bytes;
    MemoryStream component_stream;
    if (impl_->component->getState(&component_stream) != kResultOk) {
        return bytes;
    }
    MemoryStream controller_stream;
    if (impl_->controller != nullptr) {
        impl_->controller->getState(&controller_stream);
    }
    auto append32 = [&bytes](std::uint32_t v) {
        bytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    };
    const auto component_size = static_cast<std::uint32_t>(component_stream.getSize());
    const auto controller_size = static_cast<std::uint32_t>(controller_stream.getSize());
    append32(component_size);
    const auto* component_data =
        static_cast<const std::uint8_t*>(static_cast<void*>(component_stream.getData()));
    bytes.insert(bytes.end(), component_data, component_data + component_size);
    append32(controller_size);
    const auto* controller_data =
        static_cast<const std::uint8_t*>(static_cast<void*>(controller_stream.getData()));
    bytes.insert(bytes.end(), controller_data, controller_data + controller_size);
    return bytes;
}

bool Vst3Plugin::load_state(const std::vector<std::uint8_t>& bytes) {
    auto read32 = [&bytes](std::size_t at) -> std::uint32_t {
        return static_cast<std::uint32_t>(bytes[at]) |
               (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
    };
    if (bytes.size() < 8) {
        return false;
    }
    const std::uint32_t component_size = read32(0);
    if (bytes.size() < 8 + component_size) {
        return false;
    }
    MemoryStream component_stream;
    component_stream.write(const_cast<std::uint8_t*>(bytes.data() + 4),
                           static_cast<int32>(component_size), nullptr);
    component_stream.seek(0, IBStream::kIBSeekSet, nullptr);
    if (impl_->component->setState(&component_stream) != kResultOk) {
        return false;
    }
    const std::uint32_t controller_size = read32(4 + component_size);
    if (impl_->controller != nullptr && controller_size > 0 &&
        bytes.size() >= 8 + component_size + controller_size) {
        MemoryStream controller_stream;
        controller_stream.write(const_cast<std::uint8_t*>(bytes.data() + 8 + component_size),
                                static_cast<int32>(controller_size), nullptr);
        controller_stream.seek(0, IBStream::kIBSeekSet, nullptr);
        impl_->controller->setState(&controller_stream);
        // Component state also feeds the controller so its view of the
        // parameters matches (the standard two-stream dance).
        component_stream.seek(0, IBStream::kIBSeekSet, nullptr);
        impl_->controller->setComponentState(&component_stream);
    }
    for (auto& [id, value] : param_cache_) {
        if (impl_->controller != nullptr) {
            value = impl_->controller->getParamNormalized(id);
        }
    }
    return true;
}

std::vector<std::filesystem::path> vst3_search_paths() {
    std::vector<std::filesystem::path> paths;
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        paths.emplace_back(std::filesystem::path(home) / ".vst3");
    }
    paths.emplace_back("/usr/lib/vst3");
    paths.emplace_back("/usr/local/lib/vst3");
    return paths;
}

} // namespace nt::ext
