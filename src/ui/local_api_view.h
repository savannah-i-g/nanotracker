// ui/local_api_view — the LOCAL API window: enable toggle, port,
// bearer token (display / copy / regenerate), connected-client table
// and the request-log tail. Ports the settings half of the web
// TrackerLocalApiWindow (Source/.../src/components/
// TrackerLocalApiWindow.tsx); the transport here is the native
// WebSocket server (api/local_api.h), so the web's postMessage gate
// and relay-bridge sections have no counterpart.
//
// The view renders for the app-owned server and mutates it only
// through start()/stop() on the UI thread; enable state, port and
// token persist in io::Settings.
#pragma once

#include "api/local_api.h"
#include "io/settings.h"
#include "ui/theme.h"

namespace nt::ui {

class LocalApiView {
public:
    static void draw(api::LocalApiServer& server, io::Settings& settings, const Theme& theme);

private:
    static void draw_clients(const api::LocalApiServer& server, const Theme& theme);
    static void draw_log(api::LocalApiServer& server, const Theme& theme);

    // Starts with the settings' port/token (generating a token when
    // empty); on failure flips the setting back so the checkbox always
    // reflects reality.
    static void apply_enabled(api::LocalApiServer& server, io::Settings& settings);
};

} // namespace nt::ui
