#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace trs80 {

/**
 * The Exatron Stringy Floppy (Model I): up to eight endless-loop tape
 * wafer drives at ports $F0-$F7 (the port's low 3 bits the unit), driven
 * by the Exatron's ROM at $3000 (`esf.rom`, optional). After xtrs (Tim
 * Mann; trs_stringy.c, MIT-style licence), so that its and TRS32's `.esf`
 * wafers read the same; mmsim's own code.
 *
 * Out: bit 0 the motor, 2 the write gate, 7 the flux written. In: bit 0
 * write protected, 2 end of tape (the leader), 3 no wafer, 7 the flux read.
 * The wafer is a loop of bit cells 124 T-states long; reading shows each
 * cell's bit a cell late, writing records the flux runs as cells (at most
 * three a run), as xtrs does. Starting the motor starts the wafer at its
 * beginning (xtrs: the ROM's @SAVE needs it).
 *
 * `.esf`: "ESF" $1A, the header length (12), flags (bit 0 write
 * protected), the leader in cells (16 bits) and the data's length in
 * bytes (32 bits, little endian), then the cells, least significant bit
 * first. Writes go back to the file (on eject, at exit, or flush()).
 */
class Stringy : public IOHandler {
public:
    static constexpr int UNITS = 8;
    static constexpr uint32_t CELL = 124;              ///< T-states a bit cell

    ~Stringy() override;
    const char* name() const override { return "Stringy Floppy"; }
    uint32_t baseAddr() const override { return 0xF0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    /** Unit 1-8 = wafer drive 0-7; `.esf` wafers. */
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    void setFitted(bool on) { m_fitted = on; }
    /** Called when a wafer is mounted (the machine fits the board). */
    std::function<void()> onMount;
    bool fitted() const { return m_fitted; }
    /** Port $F0-$F7 at `now` (T-states). */
    uint8_t in(uint8_t port, uint64_t now);
    uint8_t peek(uint8_t port) const;
    void out(uint8_t port, uint8_t v, uint64_t now);

    /** Mount a wafer's bytes (an .esf file; path empty: not saved). */
    bool load(int unit, const std::vector<uint8_t>& esf, const std::string& path, std::string* err = nullptr);
    /** A blank wafer: `kb` KB of data (xtrs: 64), the leader `eot` cells (60). */
    static std::vector<uint8_t> create(int kb = 64, int eot = 60, bool writeProtected = false);
    /** The wafer as an .esf file. */
    std::vector<uint8_t> image(int unit) const;
    void flush();

private:
    struct Wafer {
        std::string path;
        std::vector<uint8_t> data;
        bool present = false, dirty = false, writeProtected = false;
        int64_t length = 0, eot = 0;            // T-states round the loop; the leader
        int64_t pos = 0;                        // T-states into the loop
        uint64_t posTime = 0;
        int64_t changePos = 0;
        int changeTo = 1;
        uint8_t inPort = 0x08, outPort = 0;
        // The .esf cursor (as xtrs's file position)
        size_t bytePos = 0, filePos = 0;
        uint8_t byteBuf = 0, bitPos = 0;
        uint16_t eotCells = 0;
    };
    static int state(uint8_t out);
    void rewind(Wafer& w);
    void update(Wafer& w, uint64_t now);
    int readBit(Wafer& w);
    void writeBit(Wafer& w, int bit);
    void writeFlux(Wafer& w, int flux, int64_t delta);
    void byteFlush(Wafer& w);
    uint8_t fileByte(const Wafer& w, size_t at) const { return at < w.data.size() ? w.data[at] : 0; }

    bool m_fitted = false;
    Wafer m_unit[UNITS];
};

}  // namespace trs80
