#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>

namespace apple2 {

/**
 * A peripheral card in an Apple II slot (1-7), fitted at run time: what the
 * bus asks of it. A card is also an IOHandler, registered so it ticks and
 * shows in the device list.
 *
 * - `$C0n0-$C0nF` (n = slot + 8, DEVSEL): deviceIo(), registers 0-15.
 * - `$Cn00-$CnFF` (IOSEL): pageIo(); an access also selects the card's
 *   `$C800-$CFFF` expansion ROM (I/O STROBE) until `$CFFF` is touched.
 * - /IRQ: irq(); sound: sample(), mixed with the speaker.
 * - vbl(): the start of vertical blanking (a mouse card's sync input).
 * Return -1 where the card doesn't drive the bus (it floats).
 */
class SlotCard : public IOHandler {
public:
    virtual int slot() const = 0;
    virtual int deviceIo(int /*reg*/, const uint8_t* /*wr*/, uint64_t /*at*/) { return -1; }
    virtual int devicePeek(int /*reg*/) { return -1; }
    virtual int pageIo(uint8_t off, const uint8_t* wr, uint64_t at) = 0;
    virtual int pagePeek(uint8_t off) = 0;
    /** The 2 KB at $C800 while selected, or null. */
    virtual const uint8_t* expansionRom() const { return nullptr; }
    virtual bool irq() const { return false; }
    virtual bool hasSound() const { return false; }
    virtual void vbl() {}
    /** Its average level up to `until` (cycles) since the last call. */
    virtual float sample(uint64_t /*until*/) { return 0.0f; }

    // IOHandler: the bus decodes the slot's addresses itself
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
};

}  // namespace apple2
