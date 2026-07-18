/* Test native stage — gain + DC offset, exercising every corner of
 * the version-1 ABI (include/ntp/ntp_stage_abi.h):
 *
 *   out = in * gain + offset   per channel, exact in float for the
 *                              values the tests choose
 *
 * Two parameters ("gain" 0..2 default 1, "offset" -1..1 default 0) so
 * the host's ParamSlot bridge is verifiable; state = the gain of the
 * most recent process() call (4 bytes, little-endian float on every
 * supported platform) so save/load round-trips and reset-to-default
 * are observable; reset restores the descriptor default.
 */
#include <ntp/ntp_stage_abi.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float last_gain;
} gain_stage;

static const ntp_stage_param_info_t s_params[] = {
    {.id = "gain", .name = "GAIN", .min_value = 0.0, .max_value = 2.0, .default_value = 1.0},
    {.id = "offset", .name = "OFFSET", .min_value = -1.0, .max_value = 1.0, .default_value = 0.0},
};

static const ntp_stage_descriptor_t s_descriptor = {
    .name = "NT TEST GAIN",
    .param_count = 2,
    .params = s_params,
};

static void* stage_create(uint32_t sample_rate, uint32_t max_block_frames) {
    (void)sample_rate;
    (void)max_block_frames;
    gain_stage* self = (gain_stage*)calloc(1, sizeof(gain_stage));
    if (self != NULL) {
        self->last_gain = 1.0f; /* the descriptor default */
    }
    return self;
}

static void stage_destroy(void* instance) {
    free(instance);
}

static void stage_process(void* instance, const float* in_l, const float* in_r, float* out_l,
                          float* out_r, uint32_t frames, const float* params) {
    gain_stage* self = (gain_stage*)instance;
    const float gain = params[0];
    const float offset = params[1];
    self->last_gain = gain;
    for (uint32_t i = 0; i < frames; ++i) {
        out_l[i] = in_l[i] * gain + offset;
        out_r[i] = in_r[i] * gain + offset;
    }
}

static uint32_t stage_state_size(void* instance) {
    (void)instance;
    return (uint32_t)sizeof(float);
}

static bool stage_state_save(void* instance, void* buffer, uint32_t size) {
    gain_stage* self = (gain_stage*)instance;
    if (size != (uint32_t)sizeof(float)) {
        return false;
    }
    memcpy(buffer, &self->last_gain, sizeof(float));
    return true;
}

static bool stage_state_load(void* instance, const void* buffer, uint32_t size) {
    gain_stage* self = (gain_stage*)instance;
    if (size != (uint32_t)sizeof(float)) {
        return false;
    }
    memcpy(&self->last_gain, buffer, sizeof(float));
    return true;
}

static void stage_reset(void* instance) {
    gain_stage* self = (gain_stage*)instance;
    self->last_gain = 1.0f;
}

static const ntp_stage_interface_t s_interface = {
    .abi_version = NTP_STAGE_ABI_VERSION,
    .descriptor = &s_descriptor,
    .create = stage_create,
    .destroy = stage_destroy,
    .process = stage_process,
    .state_size = stage_state_size,
    .state_save = stage_state_save,
    .state_load = stage_state_load,
    .reset = stage_reset,
};

NTP_STAGE_EXPORT const ntp_stage_interface_t* ntp_stage_entry(void) {
    return &s_interface;
}
