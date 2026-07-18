// io/import/it_importer — Impulse Tracker .it import (behavioural
// reference: Source/.../src/lib/itImporter.ts; decompression mirrors
// libxmp's itsex.c reference implementation of IT_MMTSR.ASM).
// Fix-don't-retain applied: Tremor/Panbrello/global-volume-family
// approximations are counted and reported, never silent (fix #2).
#pragma once

#include "io/import/import_common.h"

#include <optional>

namespace nt::io::import {

std::optional<engine::TrackerProject> import_it(const std::uint8_t* data, std::size_t size,
                                                ImportResults& results);

} // namespace nt::io::import
