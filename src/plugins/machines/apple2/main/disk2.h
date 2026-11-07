#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apple2 {

/**
 * A 5.25" disk image (AppleWin DiskImageHelper.cpp): 35 or more tracks of
 * 16 sectors in DOS order (.dsk, .do) or ProDOS order (.po), nibblized a
 * track at a time as DOS 3.3 formats it (volume 254), or a .nib (6656
 * nibbles a track, as recorded).
 */
class Disk2Image {
public:
    enum class Order { ProDOS, DOS, Nib };
    static constexpr int TRACK_BYTES = 4096;        // 16 x 256
    static constexpr int NIB_TRACK = 0x1A00;        // 6656 nibbles
    static constexpr int MAX_NIBBLES = 0x1A18;

    /** Open `path`; the order from the extension and contents. */
    static std::unique_ptr<Disk2Image> open(const std::string& path, std::string* err = nullptr);
    /** A disk from bytes (tests); `ext` picks the format (".dsk", ".po", ".nib"). */
    static std::unique_ptr<Disk2Image> fromBytes(std::vector<uint8_t> data, const std::string& ext,
                                                 std::string* err = nullptr);

    int tracks() const { return m_tracks; }
    Order order() const { return m_order; }
    bool writeProtected() const { return m_protect; }
    void setWriteProtected(bool p) { m_protect = p; }
    const std::string& path() const { return m_path; }
    const std::vector<uint8_t>& data() const { return m_data; }

    /** The track's nibbles into `out` (MAX_NIBBLES), the count returned. */
    int readTrack(int track, uint8_t* out, bool skew = true);
    /** Nibbles back into the image (and its file). */
    void writeTrack(int track, const uint8_t* nibbles, int count);

    // The 6-and-2 code (Code62/Decode62), for the tests
    static void nibblizeSector(const uint8_t* sector, uint8_t* out343);
    static void denibblizeSector(const uint8_t* in343, uint8_t* sector);

private:
    int nibblize(int track, uint8_t* out) const;
    void denibblize(const uint8_t* nibbles, int count, uint8_t* trackOut) const;

    std::vector<uint8_t> m_data;
    std::string m_path;
    Order m_order = Order::DOS;
    int m_tracks = 0;
    bool m_protect = false;
};

/**
 * The Disk II interface card (AppleWin Disk.cpp, the .dsk/.nib path of its
 * "authentic" speed): the P5 boot ROM (16 sectors) at $Cn00, the switches at
 * $C080+n*16: stepper phases 0-3 ($0-$7), motor ($8/$9), drive 1/2 ($A/$B),
 * the data latch ($C), write-protect sense/load ($D), read/write mode
 * ($E/$F). The disk passes a nibble every 32 cycles; reads in quick
 * succession see a partial nibble.
 */
class Disk2Card : public IOHandler {
public:
    static constexpr int SPINNING_CYCLES = 1000 * 1000;   // the motor runs on ~1 s after off
    static constexpr int MOTOR_ON_UNTIL_LSS_STABLE_CYCLES = 0x2EC;

    explicit Disk2Card(std::vector<uint8_t> rom);
    ~Disk2Card() override;

    const char* name() const override { return "Disk II"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { reset(true); }
    void tick(uint64_t) override {}
    /** After each instruction: the bus clock `now`, `cycles` since the last. */
    void update(uint64_t now, uint64_t cycles);

    /** RESET (the switches off); a power cycle also stops the drives. */
    void reset(bool powerCycle);

    /** $C0n0-$C0nF at cycle `now`: the value (even addresses: the latch),
     *  or -1 for the floating bus. `wr` set for a write. */
    int io(uint16_t addr, const uint8_t* wr, uint64_t now);
    uint8_t rom(uint8_t offset) const { return m_rom[offset]; }

    // Drives 1 and 2 are units 1 and 2
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    bool insert(int drive, std::unique_ptr<Disk2Image> image);
    Disk2Image* image(int drive) { return m_drive[drive & 1].disk.image.get(); }
    int phase(int drive) const { return m_drive[drive & 1].phase; }
    bool motorOn() const { return m_motorOn; }
    int currentDrive() const { return m_curr; }

private:
    struct Floppy {
        std::unique_ptr<Disk2Image> image;
        std::vector<uint8_t> track;
        int nibbles = 0;
        int byte = 0;
        bool trackData = false, dirty = false;
    };
    struct Drive {
        Floppy disk;
        int phase = 0;              // half tracks 0-79
        int spinning = 0;
        int writeLight = 0;
        uint64_t motorOnCycle = 0;
    };

    void checkSpinning(bool stateChanged, uint64_t now);
    void controlMotor(uint16_t addr, uint64_t now);
    void controlStepper(uint16_t addr, uint64_t now);
    void stepperDeferred();
    void enable(uint16_t addr, uint64_t now);
    void readWrite(uint64_t now);
    void loadWriteProtect();
    void setSequencer(uint16_t addr);
    void readTrack(int drive);
    void flush(int drive);
    void syncStepper(uint64_t now);

    std::vector<uint8_t> m_rom;
    Drive m_drive[2];
    int m_curr = 0;
    bool m_motorOn = false;
    int m_magnets = 0;
    bool m_writeMode = false, m_loadMode = false;
    uint8_t m_latch = 0;
    uint64_t m_lastCycle = 0, m_lastReadLatch = 0, m_now = 0;
    // A stepper change takes effect 10 cycles later, so two adjacent
    // magnets turned off in quick succession don't move the head (GH#1110)
    bool m_deferred = false;
    uint16_t m_deferredAddr = 0;
    uint64_t m_deferredAt = 0;
    uint32_t m_rand = 0x1234567;
};

}  // namespace apple2
