// ui/instrument_table_view — the INSTRUMENTS window: the unified
// instrument table (web v2 model) mapping 1-based slots to sources,
// with the bound-tracks matrix that powers the pattern editor's
// instrument-free note entry. All three source kinds (sample / plugin
// / workspace) bind end to end through the session; the table's picker
// itself currently offers sample slots only.
// Web reference: TrackerInstrumentTablePanel.tsx + the routing matrix
// from InstrumentWindow.tsx.
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

namespace nt::ui {

class InstrumentTableView {
public:
    static void draw(app::ProjectSession& session, const Theme& theme);
};

} // namespace nt::ui
