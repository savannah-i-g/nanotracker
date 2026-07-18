// plugins/ntp_stage_host — the host side of the native-stage C ABI
// (include/ntp/ntp_stage_abi.h): platform-tag resolution for the
// binaries/<tag>/ archive layout, library loading through
// platform::shared_library, and strict entry validation. Same load
// philosophy as the rest of the NTP loader: a binary either validates
// completely or is refused with collected errors saying exactly why.
#pragma once

#include "ntp/ntp_stage_abi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nt::plugins {

// Host bound on a descriptor's param_count — native-stage parameters
// bridge into the same ParamSlot array every NodeRuntime carries
// (ntp_graph.h kMaxParamSlots). Raising it later is additive.
inline constexpr std::uint32_t kNativeStageMaxParams = 12;

// This build's platform tag ("linux-x86_64", "windows-x86_64", …) and
// shared-library extension (".so", ".dll", ".dylib").
const char* native_stage_platform_tag();
const char* native_stage_binary_extension();

// Archive-relative binary path for `stage_name` under `tag`:
// binaries/<tag>/<stage_name><extension-for-this-build>.
std::string native_stage_archive_path(const std::string& stage_name, const std::string& tag);

// One loaded stage binary: the archive bytes written to a private temp
// file (dlopen/LoadLibrary need a filesystem path), the library
// handle, and the validated interface. The handle stays open — and the
// interface pointer valid — until destruction; owners must outlive
// every stage instance created from the interface (the registry
// retires replaced plugins instead of destroying them, exactly like
// CLAP libraries).
class NativeStageBinary {
public:
    // Writes, opens and validates a stage binary: entry symbol
    // present, abi_version checked before anything else (mismatch is
    // refused naming both versions), descriptor sanity (name, param
    // table bounds and ranges, required callbacks, all-or-none state
    // trio), and a create/destroy probe at `device_rate`. Returns null
    // with errors collected under the stage's name on any refusal.
    static std::unique_ptr<NativeStageBinary> open(const std::string& stage_name,
                                                   const std::uint8_t* bytes, std::size_t size,
                                                   std::uint32_t device_rate,
                                                   std::vector<std::string>& errors);

    ~NativeStageBinary(); // closes the library, removes the temp file

    NativeStageBinary(const NativeStageBinary&) = delete;
    NativeStageBinary& operator=(const NativeStageBinary&) = delete;
    NativeStageBinary(NativeStageBinary&&) = delete;
    NativeStageBinary& operator=(NativeStageBinary&&) = delete;

    [[nodiscard]] const ntp_stage_interface_t* entry() const { return entry_; }

private:
    NativeStageBinary() = default;

    void* handle_ = nullptr;
    std::filesystem::path extracted_path_;
    const ntp_stage_interface_t* entry_ = nullptr;
};

} // namespace nt::plugins
