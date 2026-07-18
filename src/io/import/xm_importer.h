// io/import/xm_importer — FastTracker 2 .xm import (behavioural
// reference: Source/.../src/lib/xmImporter.ts, with the fix-don't-
// retain items applied):
//   - the version field is checked; non-0x0104 files load with a
//     warning instead of silently (fix #1)
//   - every remapped/approximated/dropped effect is counted and
//     reported; nothing degrades silently (fix #2)
//   - sample headers are read at the standard 40 bytes because FT2
//     itself ignores the size field (OpenMPT wiki guidance); this is
//     deliberate, not the web app's accident (fix #3 context)
//   - relNote folds into base_note as MIDI_C5 - relNote (the web app's
//     corrected sign; pinned by a regression test)
#pragma once

#include "io/import/import_common.h"

#include <optional>

namespace nt::io::import {

std::optional<engine::TrackerProject> import_xm(const std::uint8_t* data, std::size_t size,
                                                ImportResults& results);

} // namespace nt::io::import
