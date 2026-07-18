/* Test CLAP plugin — a minimal but spec-honest instrument used to
 * verify the native host against a real dlopen'd .clap binary.
 *
 * One sine voice per active note, one parameter ("gain", id 1,
 * 0..1 default 0.5), state = the gain value as 8 raw bytes (double),
 * stereo out. Everything the host exercises: entry/factory lifecycle,
 * descriptor, audio-port enumeration, param enumeration + values +
 * event application, note on/off events, process, state save/load.
 */
#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_NOTES 16

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t* host;
    double sample_rate;
    double gain;
    struct {
        int active;
        int key;
        double phase;
    } notes[MAX_NOTES];
} test_plugin_t;

static const clap_plugin_descriptor_t s_descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "nt.test.sine",
    .name = "NT TEST SINE",
    .vendor = "nanoTracker tests",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "1.0.0",
    .description = "host-verification sine instrument",
    .features = (const char*[]){CLAP_PLUGIN_FEATURE_INSTRUMENT, NULL},
};

/* ── audio ports ─────────────────────────────────────────────────── */

static uint32_t ports_count(const clap_plugin_t* plugin, bool is_input) {
    (void)plugin;
    return is_input ? 0 : 1;
}

static bool ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                      clap_audio_port_info_t* info) {
    (void)plugin;
    if (is_input || index != 0) {
        return false;
    }
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", "main");
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

/* ── params ──────────────────────────────────────────────────────── */

static uint32_t params_count(const clap_plugin_t* plugin) {
    (void)plugin;
    return 1;
}

static bool params_get_info(const clap_plugin_t* plugin, uint32_t index,
                            clap_param_info_t* info) {
    (void)plugin;
    if (index != 0) {
        return false;
    }
    info->id = 1;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie = NULL;
    snprintf(info->name, sizeof(info->name), "%s", "gain");
    snprintf(info->module, sizeof(info->module), "%s", "");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.5;
    return true;
}

static bool params_get_value(const clap_plugin_t* plugin, clap_id id, double* out) {
    test_plugin_t* self = plugin->plugin_data;
    if (id != 1) {
        return false;
    }
    *out = self->gain;
    return true;
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

/* ── state ───────────────────────────────────────────────────────── */

static bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    test_plugin_t* self = plugin->plugin_data;
    return stream->write(stream, &self->gain, sizeof(self->gain)) == sizeof(self->gain);
}

static bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    test_plugin_t* self = plugin->plugin_data;
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

static void handle_event(test_plugin_t* self, const clap_event_header_t* header) {
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
        return;
    }
    switch (header->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t* note = (const clap_event_note_t*)header;
        for (int i = 0; i < MAX_NOTES; ++i) {
            if (!self->notes[i].active) {
                self->notes[i].active = 1;
                self->notes[i].key = note->key;
                self->notes[i].phase = 0.0;
                break;
            }
        }
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t* note = (const clap_event_note_t*)header;
        for (int i = 0; i < MAX_NOTES; ++i) {
            if (self->notes[i].active && (note->key < 0 || self->notes[i].key == note->key)) {
                self->notes[i].active = 0;
            }
        }
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t* pv = (const clap_event_param_value_t*)header;
        if (pv->param_id == 1) {
            self->gain = pv->value;
        }
        break;
    }
    default:
        break;
    }
}

static void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                         const clap_output_events_t* out) {
    (void)out;
    test_plugin_t* self = plugin->plugin_data;
    const uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        handle_event(self, in->get(in, i));
    }
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
    test_plugin_t* self = plugin->plugin_data;
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
    test_plugin_t* self = plugin->plugin_data;
    memset(self->notes, 0, sizeof(self->notes));
}

static clap_process_status plugin_process(const clap_plugin_t* plugin,
                                          const clap_process_t* process) {
    test_plugin_t* self = plugin->plugin_data;

    const uint32_t event_count = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < event_count; ++i) {
        handle_event(self, process->in_events->get(process->in_events, i));
    }

    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2) {
        return CLAP_PROCESS_ERROR;
    }
    float* out_l = process->audio_outputs[0].data32[0];
    float* out_r = process->audio_outputs[0].data32[1];
    for (uint32_t f = 0; f < process->frames_count; ++f) {
        double mix = 0.0;
        for (int n = 0; n < MAX_NOTES; ++n) {
            if (!self->notes[n].active) {
                continue;
            }
            const double freq = 440.0 * pow(2.0, (self->notes[n].key - 69) / 12.0);
            mix += sin(self->notes[n].phase * 2.0 * M_PI);
            self->notes[n].phase += freq / self->sample_rate;
            if (self->notes[n].phase >= 1.0) {
                self->notes[n].phase -= 1.0;
            }
        }
        const float v = (float)(mix * self->gain);
        out_l[f] = v;
        out_r[f] = v;
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
    test_plugin_t* self = calloc(1, sizeof(test_plugin_t));
    self->host = host;
    self->gain = 0.5;
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
