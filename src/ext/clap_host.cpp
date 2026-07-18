#include "ext/clap_host.h"

#include "platform/shared_library.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <poll.h>
#endif

namespace nt::ext {

// C-ABI interop file: CLAP structs carry fixed char arrays and poll(2)
// takes raw arrays — the decay warnings are the API's shape, not ours.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

namespace {

// ── Host callback surface ────────────────────────────────────────────
// The host object handed to every instance. v1 implements the minimum
// honest surface: extension lookup returns null (log/thread-check are
// nice-to-haves the test plugin and typical synths tolerate), and the
// request_* callbacks are recorded flags the UI loop services.

// clap.log — plugin diagnostics onto stderr.
void host_log(const clap_host_t* host, clap_log_severity severity, const char* message) {
    (void)host;
    (void)severity;
    std::fprintf(stderr, "clap: %s\n", message != nullptr ? message : "");
}

const clap_host_log_t s_host_log = {.log = host_log};

// clap.thread-check — the thread that built the host object is "main";
// everything else counts as audio. Good enough for a single-audio-
// thread host and what plugins actually gate on.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — per-thread flag
thread_local bool t_is_main_thread = false;

bool host_is_main_thread(const clap_host_t* host) {
    (void)host;
    return t_is_main_thread;
}

bool host_is_audio_thread(const clap_host_t* host) {
    (void)host;
    return !t_is_main_thread;
}

const clap_host_thread_check_t s_host_thread_check = {
    .is_main_thread = host_is_main_thread,
    .is_audio_thread = host_is_audio_thread,
};

// clap.posix-fd-support / clap.timer-support — forwarded to the
// instance via host_data; GUIs starve on Linux without them. The fd
// half is POSIX-only; on Windows timer-support alone carries plugin
// GUIs (the message pump is the event loop).
#ifndef _WIN32
bool host_fd_register(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) {
    return static_cast<ClapPlugin*>(host->host_data)->host_register_fd(fd, flags);
}

bool host_fd_modify(const clap_host_t* host, int fd, clap_posix_fd_flags_t flags) {
    auto* self = static_cast<ClapPlugin*>(host->host_data);
    self->host_unregister_fd(fd);
    return self->host_register_fd(fd, flags);
}

bool host_fd_unregister(const clap_host_t* host, int fd) {
    return static_cast<ClapPlugin*>(host->host_data)->host_unregister_fd(fd);
}

const clap_host_posix_fd_support_t s_host_posix_fd = {
    .register_fd = host_fd_register,
    .modify_fd = host_fd_modify,
    .unregister_fd = host_fd_unregister,
};
#endif // !_WIN32

bool host_timer_register(const clap_host_t* host, uint32_t period_ms, clap_id* timer_id) {
    return static_cast<ClapPlugin*>(host->host_data)->host_register_timer(period_ms, timer_id);
}

bool host_timer_unregister(const clap_host_t* host, clap_id timer_id) {
    return static_cast<ClapPlugin*>(host->host_data)->host_unregister_timer(timer_id);
}

const clap_host_timer_support_t s_host_timer = {
    .register_timer = host_timer_register,
    .unregister_timer = host_timer_unregister,
};

const void* host_get_extension(const clap_host_t* host, const char* extension_id) {
    (void)host;
    if (std::strcmp(extension_id, CLAP_EXT_LOG) == 0) {
        return &s_host_log;
    }
    if (std::strcmp(extension_id, CLAP_EXT_THREAD_CHECK) == 0) {
        return &s_host_thread_check;
    }
#ifndef _WIN32
    if (std::strcmp(extension_id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) {
        return &s_host_posix_fd;
    }
#endif
    if (std::strcmp(extension_id, CLAP_EXT_TIMER_SUPPORT) == 0) {
        return &s_host_timer;
    }
    return nullptr;
}

void host_request_restart(const clap_host_t* host) {
    (void)host;
}

void host_request_process(const clap_host_t* host) {
    (void)host;
}

void host_request_callback(const clap_host_t* host) {
    static_cast<ClapPlugin*>(host->host_data)->host_request_callback_flag();
}

// ── CLAP event-list shims over std::vector staging ───────────────────

struct EventListContext {
    const std::vector<clap_event_note_t>* notes = nullptr;
    const std::vector<clap_event_param_value_t>* params = nullptr;
    const std::vector<clap_event_midi_t>* midi = nullptr;
};

uint32_t in_events_size(const clap_input_events_t* list) {
    const auto* ctx = static_cast<const EventListContext*>(list->ctx);
    return static_cast<uint32_t>(ctx->notes->size() + ctx->params->size() + ctx->midi->size());
}

const clap_event_header_t* in_events_get(const clap_input_events_t* list, uint32_t index) {
    const auto* ctx = static_cast<const EventListContext*>(list->ctx);
    if (index < ctx->params->size()) {
        return &(*ctx->params)[index].header;
    }
    index -= static_cast<uint32_t>(ctx->params->size());
    if (index < ctx->notes->size()) {
        return &(*ctx->notes)[index].header;
    }
    index -= static_cast<uint32_t>(ctx->notes->size());
    if (index < ctx->midi->size()) {
        return &(*ctx->midi)[index].header;
    }
    return nullptr;
}

bool out_events_try_push(const clap_output_events_t* list, const clap_event_header_t* event) {
    (void)list;
    (void)event;
    return true; // v1 discards plugin→host events (param gestures etc.)
}

} // namespace

// ── ClapLibrary ──────────────────────────────────────────────────────

std::unique_ptr<ClapLibrary> ClapLibrary::open(const std::filesystem::path& path,
                                               std::string& error) {
    void* handle = platform::library_open(path, error);
    if (handle == nullptr) {
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — dlsym contract
    const auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(
        platform::library_symbol(handle, "clap_entry"));
    if (entry == nullptr) {
        error = "no clap_entry symbol — not a CLAP plugin";
        platform::library_close(handle);
        return nullptr;
    }
    const std::string path_str = path.string();
    if (entry->init != nullptr && !entry->init(path_str.c_str())) {
        error = "clap_entry.init refused";
        platform::library_close(handle);
        return nullptr;
    }
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr) {
        error = "plugin exposes no plugin factory";
        if (entry->deinit != nullptr) {
            entry->deinit();
        }
        platform::library_close(handle);
        return nullptr;
    }

    auto library = std::unique_ptr<ClapLibrary>(new ClapLibrary());
    library->handle_ = handle;
    library->entry_ = entry;
    library->factory_ = factory;
    library->path_ = path;
    const uint32_t count = factory->get_plugin_count(factory);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, i);
        if (desc == nullptr) {
            continue;
        }
        Descriptor d;
        d.id = desc->id != nullptr ? desc->id : "";
        d.name = desc->name != nullptr ? desc->name : d.id;
        d.vendor = desc->vendor != nullptr ? desc->vendor : "";
        d.version = desc->version != nullptr ? desc->version : "";
        if (desc->features != nullptr) {
            for (const char* const* f = desc->features; *f != nullptr; ++f) {
                if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) {
                    d.is_instrument = true;
                }
            }
        }
        library->descriptors_.push_back(std::move(d));
    }
    if (library->descriptors_.empty()) {
        error = "plugin factory lists no plugins";
        return nullptr; // destructor deinits + closes the library
    }
    return library;
}

ClapLibrary::~ClapLibrary() {
    if (entry_ != nullptr && entry_->deinit != nullptr) {
        entry_->deinit();
    }
    platform::library_close(handle_);
}

// ── ClapPlugin ───────────────────────────────────────────────────────

std::unique_ptr<ClapPlugin> ClapPlugin::create(const ClapLibrary& library,
                                               const std::string& plugin_id, std::uint32_t rate,
                                               std::string& error) {
    t_is_main_thread = true; // instantiation defines the main thread
    auto self = std::unique_ptr<ClapPlugin>(new ClapPlugin());
    self->plugin_id_ = plugin_id;
    self->rate_ = rate;
    self->host_ = clap_host_t{
        .clap_version = CLAP_VERSION_INIT,
        .host_data = self.get(),
        .name = "nanoTracker",
        .vendor = "nanoTracker",
        .url = "",
        .version = "1.0",
        .get_extension = host_get_extension,
        .request_restart = host_request_restart,
        .request_process = host_request_process,
        .request_callback = host_request_callback,
    };

    const clap_plugin_factory_t* factory = library.factory();
    self->plugin_ = factory->create_plugin(factory, &self->host_, plugin_id.c_str());
    if (self->plugin_ == nullptr) {
        error = "factory refused to create \"" + plugin_id + "\"";
        return nullptr;
    }
    if (!self->plugin_->init(self->plugin_)) {
        error = "plugin init failed";
        self->plugin_->destroy(self->plugin_);
        self->plugin_ = nullptr;
        return nullptr;
    }
    for (const ClapLibrary::Descriptor& desc : library.descriptors()) {
        if (desc.id == plugin_id) {
            self->name_ = desc.name;
        }
    }

    // Note ports: honour the plugin's preferred dialect. Plugins that
    // prefer (or only accept) MIDI get 3-byte note on/off events on
    // their first note-in port; everyone else gets CLAP note events.
    const auto* note_ports = static_cast<const clap_plugin_note_ports_t*>(
        self->plugin_->get_extension(self->plugin_, CLAP_EXT_NOTE_PORTS));
    if (note_ports != nullptr && note_ports->count(self->plugin_, true) > 0) {
        clap_note_port_info_t info{};
        if (note_ports->get(self->plugin_, 0, true, &info)) {
            self->note_port_id_ = info.id;
            const bool supports_clap = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
            const bool prefers_midi = info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI;
            self->notes_as_midi_ = !supports_clap || prefers_midi;
        }
    }

    // Audio ports: v1 hosts one stereo in (when present) + main out.
    const auto* audio_ports = static_cast<const clap_plugin_audio_ports_t*>(
        self->plugin_->get_extension(self->plugin_, CLAP_EXT_AUDIO_PORTS));
    if (audio_ports != nullptr) {
        self->has_audio_input_ = audio_ports->count(self->plugin_, true) > 0;
        if (audio_ports->count(self->plugin_, false) == 0) {
            error = "plugin has no audio output";
            self->plugin_->destroy(self->plugin_);
            self->plugin_ = nullptr;
            return nullptr;
        }
    }

    // Parameters.
    const auto* params = static_cast<const clap_plugin_params_t*>(
        self->plugin_->get_extension(self->plugin_, CLAP_EXT_PARAMS));
    if (params != nullptr) {
        const uint32_t count = params->count(self->plugin_);
        for (uint32_t i = 0; i < count; ++i) {
            clap_param_info_t info{};
            if (!params->get_info(self->plugin_, i, &info)) {
                continue;
            }
            ClapParamInfo p;
            p.id = info.id;
            p.name = static_cast<const char*>(info.name);
            p.module = static_cast<const char*>(info.module);
            p.min = info.min_value;
            p.max = info.max_value;
            p.def = info.default_value;
            double current = info.default_value;
            params->get_value(self->plugin_, info.id, &current);
            self->param_cache_.emplace_back(info.id, current);
            self->params_.push_back(std::move(p));
        }
    }

    if (!self->plugin_->activate(self->plugin_, rate, 1, audio::kGraphBlockFrames)) {
        error = "plugin refused activation";
        self->plugin_->destroy(self->plugin_);
        self->plugin_ = nullptr;
        return nullptr;
    }
    self->active_ = true;

    self->pending_notes_.reserve(64);
    self->pending_params_.reserve(128);
    self->pending_midi_.reserve(64);
    self->planar_.assign(static_cast<std::size_t>(audio::kGraphBlockFrames) * 4, 0.0F);
    return self;
}

ClapPlugin::~ClapPlugin() {
    if (plugin_ != nullptr) {
        if (active_) {
            plugin_->deactivate(plugin_);
        }
        plugin_->destroy(plugin_);
    }
}

double ClapPlugin::param_value(clap_id id) const {
    for (const auto& [existing, value] : param_cache_) {
        if (existing == id) {
            return value;
        }
    }
    return 0.0;
}

void ClapPlugin::set_param(clap_id id, double value) {
    for (auto& [existing, cached] : param_cache_) {
        if (existing == id) {
            cached = value;
        }
    }
    param_changes_.push({.id = id, .value = value});
}

void ClapPlugin::plugin_note_on(int note, float velocity) {
    if (notes_as_midi_) {
        if (pending_midi_.size() >= pending_midi_.capacity()) {
            return;
        }
        clap_event_midi_t event{};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_MIDI;
        event.port_index = static_cast<uint16_t>(note_port_id_);
        event.data[0] = 0x90;
        event.data[1] = static_cast<uint8_t>(note & 0x7F);
        event.data[2] =
            static_cast<uint8_t>(std::clamp(static_cast<int>(velocity * 127.0F), 1, 127));
        pending_midi_.push_back(event);
        return;
    }
    if (pending_notes_.size() >= pending_notes_.capacity()) {
        return; // full block — drop rather than allocate on the RT path
    }
    clap_event_note_t event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_NOTE_ON;
    event.note_id = -1;
    event.port_index = static_cast<int16_t>(note_port_id_);
    event.channel = 0;
    event.key = static_cast<int16_t>(note);
    event.velocity = velocity;
    pending_notes_.push_back(event);
}

void ClapPlugin::plugin_note_off(int note) {
    if (notes_as_midi_) {
        if (pending_midi_.size() >= pending_midi_.capacity()) {
            return;
        }
        clap_event_midi_t event{};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_MIDI;
        event.port_index = static_cast<uint16_t>(note_port_id_);
        event.data[0] = 0x80;
        event.data[1] = static_cast<uint8_t>(note & 0x7F);
        event.data[2] = 0;
        pending_midi_.push_back(event);
        return;
    }
    if (pending_notes_.size() >= pending_notes_.capacity()) {
        return;
    }
    clap_event_note_t event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_NOTE_OFF;
    event.note_id = -1;
    event.port_index = static_cast<int16_t>(note_port_id_);
    event.channel = 0;
    event.key = static_cast<int16_t>(note);
    event.velocity = 0.0;
    pending_notes_.push_back(event);
}

void ClapPlugin::process_block(const float* in, float* out, std::uint32_t frames) {
    frames = std::min(frames, audio::kGraphBlockFrames);
    if (plugin_ == nullptr) {
        std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
        return;
    }
    if (!processing_) {
        processing_ = plugin_->start_processing(plugin_);
        if (!processing_) {
            std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
            return;
        }
    }

    // Drain UI param changes into CLAP events.
    if (pending_params_.size() < pending_params_.capacity()) {
        ParamChange change{};
        while (param_changes_.pop(change) && pending_params_.size() < pending_params_.capacity()) {
            clap_event_param_value_t event{};
            event.header.size = sizeof(event);
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.param_id = change.id;
            event.cookie = nullptr;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = change.value;
            pending_params_.push_back(event);
        }
    }

    // Planar staging: [0]/[1] input L/R, [2]/[3] output L/R.
    float* in_l = planar_.data();
    float* in_r = planar_.data() + audio::kGraphBlockFrames;
    float* out_l = planar_.data() + (static_cast<std::size_t>(audio::kGraphBlockFrames) * 2);
    float* out_r = planar_.data() + (static_cast<std::size_t>(audio::kGraphBlockFrames) * 3);
    for (std::uint32_t i = 0; i < frames; ++i) {
        in_l[i] = in != nullptr ? in[static_cast<std::size_t>(i) * 2] : 0.0F;
        in_r[i] = in != nullptr ? in[(static_cast<std::size_t>(i) * 2) + 1] : 0.0F;
        out_l[i] = 0.0F;
        out_r[i] = 0.0F;
    }

    std::array<float*, 2> in_channels = {in_l, in_r};
    std::array<float*, 2> out_channels = {out_l, out_r};
    clap_audio_buffer_t in_buffer{};
    in_buffer.data32 = in_channels.data();
    in_buffer.channel_count = 2;
    clap_audio_buffer_t out_buffer{};
    out_buffer.data32 = out_channels.data();
    out_buffer.channel_count = 2;

    EventListContext events_ctx{
        .notes = &pending_notes_, .params = &pending_params_, .midi = &pending_midi_};
    const clap_input_events_t in_events{
        .ctx = &events_ctx, .size = in_events_size, .get = in_events_get};
    const clap_output_events_t out_events{.ctx = nullptr, .try_push = out_events_try_push};

    clap_process_t process{};
    process.steady_time = -1;
    process.frames_count = frames;
    process.transport = nullptr;
    process.audio_inputs = has_audio_input_ ? &in_buffer : nullptr;
    process.audio_inputs_count = has_audio_input_ ? 1 : 0;
    process.audio_outputs = &out_buffer;
    process.audio_outputs_count = 1;
    process.in_events = &in_events;
    process.out_events = &out_events;

    const clap_process_status status = plugin_->process(plugin_, &process);
    pending_notes_.clear();
    pending_params_.clear();
    pending_midi_.clear();

    if (status == CLAP_PROCESS_ERROR) {
        std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
        return;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        out[static_cast<std::size_t>(i) * 2] = out_l[i];
        out[(static_cast<std::size_t>(i) * 2) + 1] = out_r[i];
    }
}

namespace {

// Memory-backed CLAP streams for state save/load.
struct OStreamContext {
    std::vector<std::uint8_t>* bytes;
};

int64_t ostream_write(const clap_ostream_t* stream, const void* buffer, uint64_t size) {
    auto* ctx = static_cast<OStreamContext*>(stream->ctx);
    const auto* data = static_cast<const std::uint8_t*>(buffer);
    ctx->bytes->insert(ctx->bytes->end(), data, data + size);
    return static_cast<int64_t>(size);
}

struct IStreamContext {
    const std::vector<std::uint8_t>* bytes;
    std::size_t position;
};

int64_t istream_read(const clap_istream_t* stream, void* buffer, uint64_t size) {
    auto* ctx = static_cast<IStreamContext*>(stream->ctx);
    const std::size_t available = ctx->bytes->size() - ctx->position;
    const std::size_t take = std::min<std::size_t>(available, size);
    std::memcpy(buffer, ctx->bytes->data() + ctx->position, take);
    ctx->position += take;
    return static_cast<int64_t>(take);
}

} // namespace

std::vector<std::uint8_t> ClapPlugin::save_state() const {
    std::vector<std::uint8_t> bytes;
    const auto* state =
        static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    if (state == nullptr) {
        return bytes;
    }
    OStreamContext ctx{.bytes = &bytes};
    const clap_ostream_t stream{.ctx = &ctx, .write = ostream_write};
    if (!state->save(plugin_, &stream)) {
        bytes.clear();
    }
    return bytes;
}

bool ClapPlugin::load_state(const std::vector<std::uint8_t>& bytes) {
    const auto* state =
        static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    if (state == nullptr) {
        return false;
    }
    IStreamContext ctx{.bytes = &bytes, .position = 0};
    const clap_istream_t stream{.ctx = &ctx, .read = istream_read};
    if (!state->load(plugin_, &stream)) {
        return false;
    }
    // Refresh the cached values from the plugin.
    const auto* params =
        static_cast<const clap_plugin_params_t*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    if (params != nullptr) {
        for (auto& [id, value] : param_cache_) {
            params->get_value(plugin_, id, &value);
        }
    }
    return true;
}

bool ClapPlugin::host_register_fd(int fd, clap_posix_fd_flags_t flags) {
    fds_.push_back({.fd = fd, .flags = flags});
    return true;
}

bool ClapPlugin::host_unregister_fd(int fd) {
    const auto before = fds_.size();
    std::erase_if(fds_, [fd](const RegisteredFd& r) { return r.fd == fd; });
    return fds_.size() != before;
}

bool ClapPlugin::host_register_timer(std::uint32_t period_ms, clap_id* timer_id) {
    RegisteredTimer timer;
    timer.id = next_timer_id_++;
    timer.period_ms = std::max(15U, period_ms); // spec floor ~15ms
    timer.next_due = 0.0;                       // fire on the first pump
    timers_.push_back(timer);
    if (timer_id != nullptr) {
        *timer_id = timer.id;
    }
    return true;
}

bool ClapPlugin::host_unregister_timer(clap_id timer_id) {
    const auto before = timers_.size();
    std::erase_if(timers_, [timer_id](const RegisteredTimer& t) { return t.id == timer_id; });
    return timers_.size() != before;
}

void ClapPlugin::pump_main_thread() {
    t_is_main_thread = true;
    if (callback_requested_) {
        callback_requested_ = false;
        plugin_->on_main_thread(plugin_);
    }

#ifndef _WIN32
    if (!fds_.empty()) {
        std::array<pollfd, 32> poll_set{};
        const std::size_t count = std::min(fds_.size(), poll_set.size());
        for (std::size_t i = 0; i < count; ++i) {
            poll_set[i].fd = fds_[i].fd;
            poll_set[i].events = POLLIN;
            if ((fds_[i].flags & CLAP_POSIX_FD_WRITE) != 0) {
                poll_set[i].events |= POLLOUT;
            }
        }
        if (poll(poll_set.data(), static_cast<nfds_t>(count), 0) > 0) {
            const auto* fd_support = static_cast<const clap_plugin_posix_fd_support_t*>(
                plugin_->get_extension(plugin_, CLAP_EXT_POSIX_FD_SUPPORT));
            if (fd_support != nullptr) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (poll_set[i].revents != 0) {
                        fd_support->on_fd(plugin_, poll_set[i].fd,
                                          (poll_set[i].revents & POLLOUT) != 0
                                              ? CLAP_POSIX_FD_WRITE
                                              : CLAP_POSIX_FD_READ);
                    }
                }
            }
        }
    }
#endif // !_WIN32

    if (!timers_.empty()) {
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const auto* timer_support = static_cast<const clap_plugin_timer_support_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_TIMER_SUPPORT));
        if (timer_support != nullptr) {
            for (RegisteredTimer& timer : timers_) {
                if (now >= timer.next_due) {
                    timer.next_due = now + (timer.period_ms / 1000.0);
                    timer_support->on_timer(plugin_, timer.id);
                }
            }
        }
    }
}

std::vector<std::filesystem::path> clap_search_paths() {
    std::vector<std::filesystem::path> paths;
    // Standard locations per clap/entry.h, then CLAP_PATH extras
    // (PATH-style list, platform separator).
#ifdef _WIN32
    constexpr char kListSeparator = ';';
    if (const char* common = std::getenv("COMMONPROGRAMFILES"); common != nullptr) {
        paths.emplace_back(std::filesystem::path(common) / "CLAP");
    }
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr) {
        paths.emplace_back(std::filesystem::path(local) / "Programs" / "Common" / "CLAP");
    }
#else
    constexpr char kListSeparator = ':';
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        paths.emplace_back(std::filesystem::path(home) / ".clap");
    }
    paths.emplace_back("/usr/lib/clap");
    paths.emplace_back("/usr/local/lib/clap");
#endif
    if (const char* extra = std::getenv("CLAP_PATH"); extra != nullptr) {
        const std::string list = extra;
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t sep = list.find(kListSeparator, start);
            const std::string part =
                list.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
            if (!part.empty()) {
                paths.emplace_back(part);
            }
            if (sep == std::string::npos) {
                break;
            }
            start = sep + 1;
        }
    }
    return paths;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

} // namespace nt::ext
