/* Deliberately-crashing CLAP effect — the S29c crash→bypass→restart
 * exit-criterion fixture. A real dlopen'd .clap whose process() aborts
 * on command, so the out-of-process bridge can be driven to a genuine
 * child crash from a test and the host proven to survive it.
 *
 * Shape: a stereo insert effect (one audio input, one audio output) that
 * copies input*gain to output, with two params —
 *   index 0 "gain"  (id 1, 0..1, default 1.0)  — state = gain as 8 raw
 *                    bytes (double), the shadow-state restart proves.
 *   index 1 "crash" (id 2, 0..1, default 0.0)  — a value > 0.5 arms the
 *                    crash; the very next process() abort()s the child.
 *
 * The crash is process()-local and deterministic: the host sets the crash
 * param, the child applies it, and process() abort()s that block. The
 * child traps no signals, so the abort tears the whole child process
 * down — exactly the failure the bridge exists to contain. Built as a
 * test MODULE (nt_test_crash), never linked into the app.
 */
#include <clap/clap.h>

#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t* host;
    double sample_rate;
    double gain;
    int crash_armed;
} crash_plugin_t;

static const clap_plugin_descriptor_t s_descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "nt.test.crash",
    .name = "NT TEST CRASH",
    .vendor = "nanoTracker tests",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "host-survival crash-on-command effect",
    .features = (const char*[]){CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL},
};

/* ── audio ports (stereo insert: one in, one out) ─────────────────── */

static uint32_t ports_count(const clap_plugin_t* plugin, bool is_input) {
    (void)plugin;
    (void)is_input;
    return 1;
}

static bool ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                      clap_audio_port_info_t* info) {
    (void)plugin;
    if (index != 0) {
        return false;
    }
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {
    .count = ports_count,
    .get = ports_get,
};

/* ── params (gain + crash trigger) ────────────────────────────────── */

static uint32_t params_count(const clap_plugin_t* plugin) {
    (void)plugin;
    return 2;
}

static bool params_get_info(const clap_plugin_t* plugin, uint32_t index,
                            clap_param_info_t* info) {
    (void)plugin;
    info->cookie = NULL;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value = 0.0;
    info->max_value = 1.0;
    snprintf(info->module, sizeof(info->module), "%s", "");
    if (index == 0) {
        info->id = 1;
        snprintf(info->name, sizeof(info->name), "%s", "gain");
        info->default_value = 1.0;
        return true;
    }
    if (index == 1) {
        info->id = 2;
        snprintf(info->name, sizeof(info->name), "%s", "crash");
        info->default_value = 0.0;
        return true;
    }
    return false;
}

static bool params_get_value(const clap_plugin_t* plugin, clap_id id, double* out) {
    crash_plugin_t* self = plugin->plugin_data;
    if (id == 1) {
        *out = self->gain;
        return true;
    }
    if (id == 2) {
        *out = self->crash_armed ? 1.0 : 0.0;
        return true;
    }
    return false;
}

static bool params_value_to_text(const clap_plugin_t* plugin, clap_id id, double value,
                                 char* out, uint32_t capacity) {
    (void)plugin;
    (void)id;
    snprintf(out, capacity, "%.2f", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t* plugin, clap_id id, const char* text,
                                 double* out) {
    (void)plugin;
    (void)id;
    *out = atof(text);
    return true;
}

static void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                         const clap_output_events_t* out);

static const clap_plugin_params_t s_params = {
    .count = params_count,
    .get_info = params_get_info,
    .get_value = params_get_value,
    .value_to_text = params_value_to_text,
    .text_to_value = params_text_to_value,
    .flush = params_flush,
};

/* ── state (gain as a raw double, matching the sine fixture) ───────── */

static bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    crash_plugin_t* self = plugin->plugin_data;
    return stream->write(stream, &self->gain, sizeof(self->gain)) == sizeof(self->gain);
}

static bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    crash_plugin_t* self = plugin->plugin_data;
    double gain = 0.0;
    if (stream->read(stream, &gain, sizeof(gain)) != sizeof(gain)) {
        return false;
    }
    self->gain = gain;
    return true;
}

static const clap_plugin_state_t s_state = {
    .save = state_save,
    .load = state_load,
};

/* ── event handling ──────────────────────────────────────────────── */

static void handle_event(crash_plugin_t* self, const clap_event_header_t* header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE) {
        return;
    }
    const clap_event_param_value_t* pv = (const clap_event_param_value_t*)header;
    if (pv->param_id == 1) {
        self->gain = pv->value;
    } else if (pv->param_id == 2) {
        self->crash_armed = pv->value > 0.5;
    }
}

static void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                         const clap_output_events_t* out) {
    (void)out;
    crash_plugin_t* self = plugin->plugin_data;
    const uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        handle_event(self, in->get(in, i));
    }
    /* flush is a main-thread call; the crash only fires from process() so
     * it lands on the child's RT thread, the realistic crash site. */
}

/* ── lifecycle + process ─────────────────────────────────────────── */

static bool plugin_init(const clap_plugin_t* plugin) {
    (void)plugin;
    return true;
}

static void plugin_destroy(const clap_plugin_t* plugin) {
    free(plugin->plugin_data);
}

static bool plugin_activate(const clap_plugin_t* plugin, double sample_rate,
                            uint32_t min_frames, uint32_t max_frames) {
    (void)min_frames;
    (void)max_frames;
    crash_plugin_t* self = plugin->plugin_data;
    self->sample_rate = sample_rate;
    return true;
}

static void plugin_deactivate(const clap_plugin_t* plugin) {
    (void)plugin;
}

static bool plugin_start_processing(const clap_plugin_t* plugin) {
    (void)plugin;
    return true;
}

static void plugin_stop_processing(const clap_plugin_t* plugin) {
    (void)plugin;
}

static void plugin_reset(const clap_plugin_t* plugin) {
    (void)plugin;
}

static clap_process_status plugin_process(const clap_plugin_t* plugin,
                                          const clap_process_t* process) {
    crash_plugin_t* self = plugin->plugin_data;

    const uint32_t event_count = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < event_count; ++i) {
        handle_event(self, process->in_events->get(process->in_events, i));
    }

    /* The crash: a real child death (SIGABRT) on the RT thread, on command. */
    if (self->crash_armed) {
#if defined(__unix__) || defined(__APPLE__)
        /* Suppress the core dump — this is a deliberate, expected crash, not
         * a bug to post-mortem, and the child may be a large ASan build. */
        struct rlimit no_core = {0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
#endif
        abort();
    }

    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2) {
        return CLAP_PROCESS_ERROR;
    }
    float* out_l = process->audio_outputs[0].data32[0];
    float* out_r = process->audio_outputs[0].data32[1];
    const int have_in = process->audio_inputs_count >= 1 && process->audio_inputs[0].channel_count >= 2;
    const float* in_l = have_in ? process->audio_inputs[0].data32[0] : NULL;
    const float* in_r = have_in ? process->audio_inputs[0].data32[1] : NULL;
    for (uint32_t f = 0; f < process->frames_count; ++f) {
        out_l[f] = (float)((in_l != NULL ? in_l[f] : 0.0F) * self->gain);
        out_r[f] = (float)((in_r != NULL ? in_r[f] : 0.0F) * self->gain);
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void* plugin_get_extension(const clap_plugin_t* plugin, const char* id) {
    (void)plugin;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &s_audio_ports;
    }
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &s_params;
    }
    if (strcmp(id, CLAP_EXT_STATE) == 0) {
        return &s_state;
    }
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t* plugin) {
    (void)plugin;
}

/* ── factory + entry ─────────────────────────────────────────────── */

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t* factory) {
    (void)factory;
    return 1;
}

static const clap_plugin_descriptor_t*
factory_get_plugin_descriptor(const clap_plugin_factory_t* factory, uint32_t index) {
    (void)factory;
    return index == 0 ? &s_descriptor : NULL;
}

static const clap_plugin_t* factory_create_plugin(const clap_plugin_factory_t* factory,
                                                  const clap_host_t* host,
                                                  const char* plugin_id) {
    (void)factory;
    if (strcmp(plugin_id, s_descriptor.id) != 0) {
        return NULL;
    }
    crash_plugin_t* self = calloc(1, sizeof(crash_plugin_t));
    self->host = host;
    self->gain = 1.0;
    self->crash_armed = 0;
    self->plugin.desc = &s_descriptor;
    self->plugin.plugin_data = self;
    self->plugin.init = plugin_init;
    self->plugin.destroy = plugin_destroy;
    self->plugin.activate = plugin_activate;
    self->plugin.deactivate = plugin_deactivate;
    self->plugin.start_processing = plugin_start_processing;
    self->plugin.stop_processing = plugin_stop_processing;
    self->plugin.reset = plugin_reset;
    self->plugin.process = plugin_process;
    self->plugin.get_extension = plugin_get_extension;
    self->plugin.on_main_thread = plugin_on_main_thread;
    return &self->plugin;
}

static const clap_plugin_factory_t s_factory = {
    .get_plugin_count = factory_get_plugin_count,
    .get_plugin_descriptor = factory_get_plugin_descriptor,
    .create_plugin = factory_create_plugin,
};

static bool entry_init(const char* plugin_path) {
    (void)plugin_path;
    return true;
}

static void entry_deinit(void) {}

static const void* entry_get_factory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) {
        return &s_factory;
    }
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
