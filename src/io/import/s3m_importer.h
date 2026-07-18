// io/import/s3m_importer — Scream Tracker 3 .s3m import (behavioural
// reference: Source/.../src/lib/s3mImporter.ts, with fix-don't-retain
// applied: every unsupported effect (Tremor and friends) and every
// skipped ADPCM sample is counted and reported, never silent).
#pragma once

#include "io/import/import_common.h"

#include <optional>

namespace nt::io::import {

std::optional<engine::TrackerProject> import_s3m(const std::uint8_t* data, std::size_t size,
                                                 ImportResults& results);

} // namespace nt::io::import
