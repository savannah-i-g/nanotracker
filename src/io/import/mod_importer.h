// io/import/mod_importer — classic Amiga ProTracker/SoundTracker .mod
// import (behavioural reference: Source/.../src/lib/modImporter.ts).
//
// Format coverage: "M.K."/"M!K!"/"FLT4"/"4CHN"-family 4-channel tags,
// xCHN/xxCH multi-channel tags, and tagless 15-sample SoundTracker.
// Pitch model: MOD C-2 (period 428) maps to tracker note 73 with
// base_note 84 so absolute pitch is preserved. Samples are upsampled
// 8287→44100 Hz with loop points scaled to the upsampled frames.
// Effects translate 1:1 (the engine's command set is ProTracker's).
#pragma once

#include "io/import/import_common.h"

#include <optional>

namespace nt::io::import {

// Parses a MOD from memory. Truncated pattern data fills with empty
// cells (reported); truncated sample data skips the remainder
// (reported). Returns std::nullopt only when the buffer cannot be a
// MOD at all.
std::optional<engine::TrackerProject> import_mod(const std::uint8_t* data, std::size_t size,
                                                 ImportResults& results);

} // namespace nt::io::import
