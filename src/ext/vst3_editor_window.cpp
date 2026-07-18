#include "ext/vst3_editor_window.h"

#include "ext/vst3_run_loop.h"

#include <pluginterfaces/base/smartpointer.h>
#include <pluginterfaces/gui/iplugview.h>

// The VST3 COM seam is built on TUID char arrays (FUID converts to one
// for every iidEqual call); the decay warnings are the API's shape,
// not ours — same treatment as the CLAP/Xlib C-ABI files.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

namespace nt::ext {

namespace {

using namespace Steinberg;

// The one platform-conditional seam: which IPlugView platform type
// this build attaches through (ext/editor_window's CLAP twin).
#ifdef _WIN32
const FIDString kPlatformType = kPlatformTypeHWND;
#else
const FIDString kPlatformType = kPlatformTypeX11EmbedWindowID;
#endif

// The IPlugFrame handed to the view: acknowledges plugin resize
// requests (resizeView → surface set_size → onSize, the same-callstack
// sequence iplugview.h mandates) and, on Linux, answers IRunLoop —
// the first of the two exposure routes (the factory host context in
// ext/vst3_host is the other).
class PlugFrame final : public IPlugFrame {
public:
    explicit PlugFrame(EditorHostSurface& surface) : surface_(&surface) {}

    PlugFrame(const PlugFrame&) = delete;
    PlugFrame& operator=(const PlugFrame&) = delete;
    PlugFrame(PlugFrame&&) = delete;
    PlugFrame& operator=(PlugFrame&&) = delete;

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* new_size) final {
        if (view == nullptr || new_size == nullptr) {
            return kInvalidArgument;
        }
        if (new_size->getWidth() > 0 && new_size->getHeight() > 0) {
            surface_->set_size(static_cast<std::uint32_t>(new_size->getWidth()),
                               static_cast<std::uint32_t>(new_size->getHeight()));
        }
        return view->onSize(new_size);
    }

    tresult PLUGIN_API queryInterface(const TUID interface_id, void** obj) final {
        if (obj == nullptr) {
            return kInvalidArgument;
        }
        if (FUnknownPrivate::iidEqual(interface_id, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(interface_id, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            addRef();
            return kResultOk;
        }
#ifndef _WIN32
        if (FUnknownPrivate::iidEqual(interface_id, Linux::IRunLoop::iid)) {
            return Vst3RunLoop::instance().queryInterface(interface_id, obj);
        }
#endif
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() final { return ++refs_; }

    uint32 PLUGIN_API release() final {
        const uint32 refs = --refs_;
        if (refs == 0) {
            // COM seam: FUnknown objects self-delete at refcount zero.
            delete this; // NOLINT(cppcoreguidelines-owning-memory)
        }
        return refs;
    }

private:
    ~PlugFrame() = default;

    EditorHostSurface* surface_;
    uint32 refs_ = 1; // UI-thread only, like every editor-window call
};

} // namespace

struct Vst3EditorWindow::Impl {
    IPtr<IPlugView> view;
    IPtr<PlugFrame> frame;
};

std::unique_ptr<Vst3EditorWindow> Vst3EditorWindow::open(Vst3Plugin& plugin, std::string& error) {
    // create_editor_view returns addRef'd; owned() adopts the count.
    const IPtr<IPlugView> view = owned(plugin.create_editor_view());
    if (view.get() == nullptr) {
        error = "plugin has no editor";
        return nullptr;
    }
    if (view->isPlatformTypeSupported(kPlatformType) != kResultTrue) {
        error = std::string("plugin editor does not support ") + kPlatformType + " embedding";
        return nullptr;
    }
    // Surface next: on a headless run this fails before the view is
    // ever attached. Resizability is the view's call.
    const bool resizable = view->canResize() == kResultTrue;
    auto surface = EditorHostSurface::open(plugin.name(), 640, 480, resizable, error);
    if (surface == nullptr) {
        error += " — parameter panel only";
        return nullptr;
    }
    ViewRect rect{};
    if (view->getSize(&rect) == kResultOk && rect.getWidth() > 0 && rect.getHeight() > 0) {
        surface->set_size(static_cast<std::uint32_t>(rect.getWidth()),
                          static_cast<std::uint32_t>(rect.getHeight()));
    }

    auto self = std::unique_ptr<Vst3EditorWindow>(new Vst3EditorWindow());
    self->impl_ = std::make_unique<Impl>();
    // COM seam: the frame is refcounted, release() deletes it.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    self->impl_->frame = owned(new PlugFrame(*surface));
    // Frame before attach: plugins may call resizeView from inside
    // attached().
    view->setFrame(self->impl_->frame);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    if (view->attached(reinterpret_cast<void*>(surface->native_handle()), kPlatformType) !=
        kResultOk) {
        view->setFrame(nullptr);
        error = "plugin editor refused to attach";
        return nullptr;
    }
    self->impl_->view = view;
    self->surface_ = std::move(surface);
    return self;
}

Vst3EditorWindow::~Vst3EditorWindow() {
    // View teardown strictly precedes surface destruction — the
    // plugin's embedded window must unparent before its host window
    // dies (the CLAP teardown order mirrored).
    if (impl_ != nullptr && impl_->view.get() != nullptr) {
        impl_->view->removed();
        impl_->view->setFrame(nullptr);
    }
}

bool Vst3EditorWindow::update() {
    if (!surface_->pump()) {
        return false;
    }
    // User resize → constrain through the view, apply, and ack with
    // onSize. (Plugin-initiated resizes were already answered
    // synchronously by PlugFrame::resizeView.)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (surface_->take_resize(width, height) && impl_ != nullptr && impl_->view.get() != nullptr) {
        ViewRect rect{0, 0, static_cast<int32>(width), static_cast<int32>(height)};
        if (impl_->view->checkSizeConstraint(&rect) == kResultTrue) {
            surface_->set_size(static_cast<std::uint32_t>(rect.getWidth()),
                               static_cast<std::uint32_t>(rect.getHeight()));
        }
        impl_->view->onSize(&rect);
    }
    // The per-frame work left is the Linux run-loop dispatch.
    // Multiple editors dispatching the shared loop in one frame is
    // harmless — timers are deadline-based and fds re-poll.
#ifndef _WIN32
    Vst3RunLoop::instance().dispatch();
#endif
    return true;
}

} // namespace nt::ext

// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
