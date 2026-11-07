#pragma once

// C128 cartridges at the external function ROM (VICE src/c128/cart/,
// GPL-2.0-or-later, VICE team): the generic function ROM and the
// bank-switching ones (Comal 80, Magic Desk 128, Warp Speed 128,
// Partner 128, GMod2-C128 with its 29F040 flash and M93C86 EEPROM).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace c128 {

/**
 * A cartridge at the C128's expansion port in C128 mode. The bus calls
 * romRead() for $8000-$FFFF where CR picks the external function ROM,
 * romWrite() for writes there (they also go to the RAM underneath), and
 * io1()/io2() for $DE00-$DEFF / $DF00-$DFFF.
 */
class C128Cart {
public:
    virtual ~C128Cart() = default;
    /** VICE's name for the type ("Magic Desk 128", ...). */
    virtual std::string type() const = 0;
    /** A read of $8000-$FFFF (peek: no side effects). */
    virtual uint8_t romRead(uint16_t addr, bool peek) = 0;
    /** A write to $8000-$FFFF while the cartridge is mapped (flash). */
    virtual void romWrite(uint16_t, uint8_t) {}
    /** $DE00-$DEFF: true if the cartridge drives a read (`v` set); writes always reach it. */
    virtual bool io1(uint16_t, const uint8_t*, bool, uint8_t&) { return false; }
    /** $DF00-$DFFF, likewise. */
    virtual bool io2(uint16_t, const uint8_t*, bool, uint8_t&) { return false; }
    /** A write to $D600-$D6FF (Partner 128 sees $D600-$D60F as well as the VDC). */
    virtual void iod6Write(uint16_t, uint8_t) {}
    /** The machine's reset. */
    virtual void reset() {}
    /** The freeze button: true if the cartridge has one (the machine then raises NMI). */
    virtual bool freeze() { return false; }
    /** The KERNAL's NMI vector ($FFFA/$FFFB) is forced to $DEDE (Partner 128's freeze). */
    virtual bool nmiVectorForced() const { return false; }
};

/**
 * A cartridge from an image file: a C128 .crt ("C128 CARTRIDGE  ", the
 * hardware type at $16: 0 generic, 1 Warp Speed 128, 2 Partner 128,
 * 3 Comal 80, 4 Magic Desk 128, 5 GMod2-C128) or a raw image: 1-32 KB is a
 * generic function ROM; 96 KB Comal 80; 64 KB to 1 MB (a power of two)
 * Magic Desk 128 (VICE takes the type from its command line for raw
 * images). nullptr with `err` set if it is none of these.
 */
std::unique_ptr<C128Cart> makeCart(const std::vector<uint8_t>& image, std::string& err);

/** A generic 32 KB function ROM image (also the internal function ROM's format). */
bool functionRomImage(const std::vector<uint8_t>& img, std::vector<uint8_t>& out, std::string& type, std::string& err);

}  // namespace c128
