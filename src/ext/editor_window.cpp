#include "ext/editor_window.h"

#include <clap/ext/gui.h>

// The CLAP gui structs are C APIs built on string-literal API ids and
// a native-handle union; the decay/union warnings are the API's
// shape, not ours.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)

namespace nt::ext {

namespace {

// The one platform-conditional seam: which CLAP window API this build
// embeds through, and which union member carries the native handle.
#ifdef _WIN32
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
#endif

clap_window_t make_clap_window(std::uintptr_t handle) {
    clap_window_t window{};
    window.api = kWindowApi;
#ifdef _WIN32
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    window.win32 = reinterpret_cast<void*>(handle);
#else
    window.x11 = handle;
#endif
    return window;
}

} // namespace

std::unique_ptr<ClapEditorWindow> ClapEditorWindow::open(ClapPlugin& plugin, std::string& error) {
    const auto* gui = static_cast<const clap_plugin_gui_t*>(
        plugin.raw()->get_extension(plugin.raw(), CLAP_EXT_GUI));
    if (gui == nullptr) {
        error = "plugin has no editor";
        return nullptr;
    }
    if (!gui->is_api_supported(plugin.raw(), kWindowApi, false)) {
        error = std::string("plugin editor does not support ") + kWindowApi + " embedding";
        return nullptr;
    }
    // Surface first: on a headless run this fails before the plugin
    // gui is ever created.
    auto surface = EditorHostSurface::open(plugin.name(), 640, 480, error);
    if (surface == nullptr) {
        error += " — parameter panel only";
        return nullptr;
    }
    if (!gui->create(plugin.raw(), kWindowApi, false)) {
        error = "plugin editor refused to create";
        return nullptr;
    }
    gui->set_scale(plugin.raw(), 1.0);
    uint32_t width = 640;
    uint32_t height = 480;
    gui->get_size(plugin.raw(), &width, &height);
    surface->set_size(width, height);

    const clap_window_t clap_window = make_clap_window(surface->native_handle());
    if (!gui->set_parent(plugin.raw(), &clap_window)) {
        error = "plugin editor refused the parent window";
        gui->destroy(plugin.raw());
        return nullptr;
    }
    gui->show(plugin.raw());

    auto self = std::unique_ptr<ClapEditorWindow>(new ClapEditorWindow());
    self->plugin_ = &plugin;
    self->surface_ = std::move(surface);
    self->gui_created_ = true;
    return self;
}

ClapEditorWindow::~ClapEditorWindow() {
    // Plugin gui teardown strictly precedes surface destruction — the
    // plugin's embedded window must unparent before its host window
    // dies.
    if (gui_created_) {
        const auto* gui = static_cast<const clap_plugin_gui_t*>(
            plugin_->raw()->get_extension(plugin_->raw(), CLAP_EXT_GUI));
        if (gui != nullptr) {
            gui->hide(plugin_->raw());
            gui->destroy(plugin_->raw());
        }
    }
}

bool ClapEditorWindow::update() {
    if (!surface_->pump()) {
        return false;
    }
    plugin_->pump_main_thread();
    return true;
}

} // namespace nt::ext

// NOLINTEND(cppcoreguidelines-pro-type-union-access)
// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
