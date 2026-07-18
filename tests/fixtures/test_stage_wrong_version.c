/* Deliberately wrong-version native stage: the interface reports ABI
 * version 999 so the host's version-first refusal path is assertable
 * (the error must name both versions). Everything past abi_version is
 * absent by design — a host honouring the contract never reads it on
 * a mismatch.
 */
#include <ntp/ntp_stage_abi.h>

static const ntp_stage_interface_t s_interface = {
    .abi_version = 999,
};

NTP_STAGE_EXPORT const ntp_stage_interface_t* ntp_stage_entry(void) {
    return &s_interface;
}
