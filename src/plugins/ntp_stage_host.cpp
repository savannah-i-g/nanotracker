#include "plugins/ntp_stage_host.h"

#include "platform/shared_library.h"

#include <atomic>
#include <cctype>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

namespace nt::plugins {

namespace {

// Extracted binaries land in the system temp directory under names
// unique per process run (session token) and per extraction (counter):
// two hosts extracting the same stage name must never collide, and
// Windows keeps a loaded DLL's file locked, so the file lives until
// the library closes.
std::filesystem::path temp_binary_path(const std::string& stage_name) {
    static const std::uint64_t session_token = [] {
        std::random_device rd;
        return (static_cast<std::uint64_t>(rd()) << 32) | rd();
    }();
    static std::atomic<std::uint64_t> counter{0};

    std::string safe;
    for (const char c : stage_name) {
        const auto uc = static_cast<unsigned char>(c);
        safe += std::isalnum(uc) != 0 || c == '-' || c == '_' ? c : '_';
    }
    std::ostringstream name;
    name << "nt_stage_" << std::hex << session_token << '_' << std::dec
         << counter.fetch_add(1, std::memory_order_relaxed) << '_' << safe
         << native_stage_binary_extension();
    return std::filesystem::temp_directory_path() / name.str();
}

} // namespace

const char* native_stage_platform_tag() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#else
    return "windows-x86_64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#else
    return "macos-x86_64";
#endif
#else
#if defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x86_64";
#endif
#endif
}

const char* native_stage_binary_extension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

std::string native_stage_archive_path(const std::string& stage_name, const std::string& tag) {
    return "binaries/" + tag + "/" + stage_name + native_stage_binary_extension();
}

std::unique_ptr<NativeStageBinary>
NativeStageBinary::open(const std::string& stage_name, const std::uint8_t* bytes, std::size_t size,
                        std::uint32_t device_rate, std::vector<std::string>& errors) {
    const std::string context = "native stage \"" + stage_name + "\": ";
    // The unique_ptr owns cleanup from here on: any refusal path just
    // returns null and the destructor closes/removes what exists.
    std::unique_ptr<NativeStageBinary> binary(new NativeStageBinary());

    binary->extracted_path_ = temp_binary_path(stage_name);
    {
        std::ofstream file(binary->extracted_path_, std::ios::binary | std::ios::trunc);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — byte/file seam
        file.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        if (!file) {
            errors.push_back(context + "cannot write the binary to " +
                             binary->extracted_path_.string());
            return nullptr;
        }
    }

    std::string open_error;
    binary->handle_ = platform::library_open(binary->extracted_path_, open_error);
    if (binary->handle_ == nullptr) {
        errors.push_back(context + "binary does not load: " + open_error);
        return nullptr;
    }

    using EntryFn = const ntp_stage_interface_t* (*)();
    void* symbol = platform::library_symbol(binary->handle_, NTP_STAGE_ENTRY_SYMBOL);
    if (symbol == nullptr) {
        errors.push_back(context + "binary does not export " NTP_STAGE_ENTRY_SYMBOL);
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — dlsym contract
    const ntp_stage_interface_t* entry = reinterpret_cast<EntryFn>(symbol)();
    if (entry == nullptr) {
        errors.push_back(context + NTP_STAGE_ENTRY_SYMBOL " returned null");
        return nullptr;
    }

    // Version gate FIRST — on mismatch nothing past abi_version is
    // read (a future layout may differ everywhere else).
    if (entry->abi_version != NTP_STAGE_ABI_VERSION) {
        errors.push_back(context + "ABI version " + std::to_string(entry->abi_version) +
                         " — this host implements ABI version " +
                         std::to_string(NTP_STAGE_ABI_VERSION));
        return nullptr;
    }

    // Descriptor sanity, collected (authors see the whole list).
    const std::size_t before = errors.size();
    const ntp_stage_descriptor_t* desc = entry->descriptor;
    if (desc == nullptr) {
        errors.push_back(context + "null descriptor");
        return nullptr;
    }
    if (desc->name == nullptr || desc->name[0] == '\0') {
        errors.push_back(context + "descriptor needs a non-empty name");
    }
    if (desc->param_count > kNativeStageMaxParams) {
        errors.push_back(context + "declares " + std::to_string(desc->param_count) +
                         " parameters — this host accepts up to " +
                         std::to_string(kNativeStageMaxParams));
    } else if (desc->param_count > 0 && desc->params == nullptr) {
        errors.push_back(context + "param_count is " + std::to_string(desc->param_count) +
                         " but the param table is null");
    } else {
        std::set<std::string> ids;
        for (std::uint32_t p = 0; p < desc->param_count; ++p) {
            const ntp_stage_param_info_t& info = desc->params[p];
            const std::string at = context + "param " + std::to_string(p);
            if (info.id == nullptr || info.id[0] == '\0') {
                errors.push_back(at + " needs a non-empty id");
                continue;
            }
            if (!ids.insert(info.id).second) {
                errors.push_back(at + " duplicates id \"" + info.id + "\"");
            }
            if (info.name == nullptr) {
                errors.push_back(at + " (\"" + info.id + "\") has a null name");
            }
            if (!(info.min_value < info.max_value)) {
                errors.push_back(at + " (\"" + info.id + "\") needs min < max");
            } else if (info.default_value < info.min_value || info.default_value > info.max_value) {
                errors.push_back(at + " (\"" + info.id + "\") default outside range");
            }
        }
    }
    if (entry->create == nullptr || entry->destroy == nullptr || entry->process == nullptr) {
        errors.push_back(context + "create, destroy and process are required");
    }
    const int state_fns = (entry->state_size != nullptr ? 1 : 0) +
                          (entry->state_save != nullptr ? 1 : 0) +
                          (entry->state_load != nullptr ? 1 : 0);
    if (state_fns != 0 && state_fns != 3) {
        errors.push_back(context + "state_size/state_save/state_load are all-or-none");
    }
    if (errors.size() != before) {
        return nullptr;
    }

    // Create/destroy probe: a stage that cannot instantiate at the
    // device rate is refused at load, not discovered as silence later.
    void* probe = entry->create(device_rate, 128);
    if (probe == nullptr) {
        errors.push_back(context + "create() failed at " + std::to_string(device_rate) + " Hz");
        return nullptr;
    }
    entry->destroy(probe);

    binary->entry_ = entry;
    return binary;
}

NativeStageBinary::~NativeStageBinary() {
    platform::library_close(handle_);
    if (!extracted_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(extracted_path_, ec); // best-effort
    }
}

} // namespace nt::plugins
