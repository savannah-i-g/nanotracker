// io/import/it_importer — Impulse Tracker .it import (behavioural
// reference: Source/.../src/lib/itImporter.ts; sample decompression
// mirrors itsex.c and is cross-checked bit-exact against libopenmpt's
// ITDecompression).
// Fix-don't-retain applied, all counted-and-reported, never silent:
//   - Tremor/Panbrello/global-volume-family effects approximated.
//   - The 2.14 vs 2.15 (double-delta) decompression variant is chosen
//     per sample from the cvt delta bit, not the file-wide cmwt — real
//     files mix the two, and the old global choice decoded half the
//     samples to noise.
//   - Samples past the engine's 31 slots (and pattern cells that name
//     them) are tallied into warnings, not dropped silently.
#pragma once

#include "io/import/import_common.h"

#include <optional>

namespace nt::io::import {

std::optional<engine::TrackerProject> import_it(const std::uint8_t* data, std::size_t size,
                                                ImportResults& results);

} // namespace nt::io::import
