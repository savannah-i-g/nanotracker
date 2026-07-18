// graph/graph_wpbr — adoption of the web app's WPBR workspace payload
// (patchbay JSON) into the native graph model, and the reverse
// serialisation. Shape reference: WorkspaceProjectState /
// WorkspaceInstrumentSnapshot / WorkspaceCableSnapshot in
// Source/.../src/lib/trackerEngine.ts:144-201.
//
// Cable endpoints resolve by stable portId when present (web v4.1+
// writes both portId and jackIndex); older snapshots resolve by
// jackIndex against the web's historical port layouts:
//   - typed era (srcKind/dstKind present): the tracker bus exposed
//     3 outs per channel (audio, vol CV, MIDI) plus master.midi in the
//     v5 layout, or 2 per channel in v4 — both are tried and the
//     stored kind arbitrates;
//   - legacy era (no kinds): jackIndex indexed the raw outputs[] array,
//     which for the tracker bus was one gain per channel and for
//     master-in a single input.
// Anything that still fails to resolve is dropped with a warning the
// UI surfaces (fix #13 — the web dropped silently to the console).
//
// Instruments and cables that reference features the native node set
// cannot host (uncatalogued plugins, the web's implicit midi-thru/
// midi-out adapter ports) are not discarded: they are carried verbatim
// as dormant entries so projects round-trip losslessly and later loads
// can wake them. Native-authored builtin nodes (sum, ext-midi-in/out)
// recreate directly from their pluginId.
#pragma once

#include "graph/graph_model.h"

#include <string>
#include <vector>

namespace nt::graph {

// Verbatim JSON of entries the current stages cannot host yet.
struct DormantEntries {
    std::vector<std::string> instruments; // WorkspaceInstrumentSnapshot objects
    std::vector<std::string> cables;      // WorkspaceCableSnapshot objects
};

struct WpbrAdoptResult {
    DormantEntries dormant;
    std::vector<std::string> warnings;
};

// Applies a WPBR JSON payload to a graph already populated with the
// built-in nodes (tracker bus, master in, module player). Window
// placements adopt onto matching nodes; cables reconnect through the
// validated model API. An empty/blank payload is a no-op.
[[nodiscard]] WpbrAdoptResult adopt_wpbr_json(WorkspaceGraph& graph, const std::string& payload);

// Serialises the graph (plus dormant entries) back to the WPBR shape.
// Live cables emit portId (authoritative) alongside jackIndex (the
// index in the native port list). Used by the FTRK writer and by
// round-trip tests.
[[nodiscard]] std::string workspace_to_wpbr_json(const WorkspaceGraph& graph,
                                                 const DormantEntries& dormant);

} // namespace nt::graph
