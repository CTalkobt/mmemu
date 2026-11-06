#pragma once

// Atari 2600 TIA palettes, after Stella (src/common/PaletteHandler.cxx),
// GPL-2.0-or-later, Stella team.

#include <cstdint>

namespace tiacore {

/** 128 colours each (0x00RRGGBB), indexed by the colour register value >> 1. */
extern const uint32_t NTSC_PALETTE[128];
extern const uint32_t PAL_PALETTE[128];
extern const uint32_t SECAM_PALETTE[128];

}  // namespace tiacore
