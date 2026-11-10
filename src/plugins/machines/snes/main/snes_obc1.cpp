// The OBC1 after Mesen2 (Core/SNES/Coprocessors/OBC1/Obc1.cpp), GPL-3.0,
// Sour and the Mesen contributors.

#include "snes_obc1.h"

namespace snes {

uint8_t Obc1::read(uint16_t a) const {
    a &= 0x1FFF;
    switch (a) {
    case 0x1FF0: case 0x1FF1: case 0x1FF2: case 0x1FF3: return ram((uint16_t)(low() + (a - 0x1FF0)));
    case 0x1FF4: return ram(high());
    default: return ram(a);
    }
}

void Obc1::write(uint16_t a, uint8_t v) {
    a &= 0x1FFF;
    switch (a) {
    case 0x1FF0: case 0x1FF1: case 0x1FF2: case 0x1FF3: setRam((uint16_t)(low() + (a - 0x1FF0)), v); break;
    case 0x1FF4: {
        // The sprite's two bits in the high table, the others kept
        const uint8_t shift = (uint8_t)((ram(0x1FF6) & 0x03) << 1);
        setRam(high(), (uint8_t)(((v & 0x03) << shift) | (ram(high()) & ~(0x03 << shift))));
        break;
    }
    default: setRam(a, v); break;
    }
}

}  // namespace snes
