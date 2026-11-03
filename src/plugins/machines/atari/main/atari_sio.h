#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class POKEY;

namespace atari {

/** A disk image in ATR form: 16-byte header, then the sectors. */
class AtrImage {
public:
    /** Parse an ATR file's bytes. */
    bool load(std::vector<uint8_t> bytes, std::string& err);
    int sectorSize() const { return m_sectorSize; }
    int sectorCount() const { return m_sectorCount; }
    /** Bytes of sector n (1-based): 128 for sectors 1-3, else sectorSize(). */
    int sizeOf(int n) const { return n <= 3 ? 128 : m_sectorSize; }
    bool read(int n, uint8_t* out) const;
    bool write(int n, const uint8_t* data);
    const std::vector<uint8_t>& bytes() const { return m_data; }
    /** File offset of sector n (1-based). */
    size_t offsetOf(int n) const;
private:
    std::vector<uint8_t> m_data;
    int m_sectorSize = 128;
    int m_sectorCount = 0;
};

/**
 * Turn an Atari executable (.xex: $FFFF-headed segments) into a bootable
 * 128-byte-sector ATR: a three-sector 6502 boot loader that reads the
 * segments through the OS's DSKINV, calls INITAD after each and jumps to
 * RUNAD at the end. Returns false with `err` if the file is not an
 * executable.
 */
bool xexToAtr(const std::vector<uint8_t>& xex, std::vector<uint8_t>& atr, std::string& err);

/**
 * The serial (SIO) bus with up to four disk drives (D1:-D4:, devices
 * $31-$34). Command frames arrive from POKEY's serial output while the PIA
 * holds the command line (CB2) low; the drive answers through POKEY's serial
 * input with ACK ('A'), COMPLETE ('C') or ERROR ('E'), the data frame and
 * its checksum, at 19,200 baud. Commands: read sector ('R'), write
 * ('W', 'P'), status ('S'); others are refused ('N').
 *
 * A disk mounted from a file is written through: each sector written goes
 * back into the .atr at once. A file that cannot be opened for writing is
 * write-protected (status bit 3; writes end in ERROR). Disks with no file
 * (a generated .xex boot disk) keep their writes in memory.
 *
 * Drives are units 1-4 to the CLI `disk` command and the GUI's drive pane.
 */
class AtariSio : public IOHandler {
public:
    explicit AtariSio(POKEY* pokey);

    const char* name() const override { return "SIO"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t cycles) override;

    /** The PIA's CB2 output: low while a command frame is sent. */
    ISignalLine* commandLine() { return &m_cmdLine; }
    /** A byte the computer sent (POKEY serial output). */
    void serialOut(uint8_t byte);

    /**
     * Put an image in drive 0-3. `path`: the file it came from, which
     * sector writes are saved to (empty: keep them in memory).
     */
    bool mount(int drive, std::vector<uint8_t> atrBytes, const std::string& label, std::string& err,
               const std::string& path = "");
    void unmount(int drive);
    const std::string& label(int drive) const { return m_drives[drive].label; }
    bool mounted(int drive) const { return m_drives[drive].present; }
    bool writeProtected(int drive) const { return m_drives[drive].readOnly; }
    /** The image as it is now (with any writes). */
    const std::vector<uint8_t>& image(int drive) const { return m_drives[drive].image.bytes(); }
    const std::string& lastError() const { return m_lastError; }

    // IOHandler disk interface (units 1-4 are D1:-D4:)
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    /** Cycles between bytes at 19,200 baud (10 bits). */
    static constexpr int BYTE_CYCLES = 940;

private:
    struct CmdLine : ISignalLine {
        AtariSio* sio = nullptr;
        bool level = true;
        bool get() const override { return level; }
        void set(bool l) override;
        void pulse() override {}
    };
    struct Drive {
        bool present = false;
        AtrImage image;
        std::string label;
        std::string path;       // the file writes are saved to
        bool readOnly = false;
        int lastSector = 0;
    };
    bool writeSector(Drive& d, int sector, const uint8_t* data);
    struct Pending { int delay; uint8_t byte; };

    void command();
    void send(uint8_t byte, int delay);
    void sendFrame(const uint8_t* data, int n, int firstDelay);
    static uint8_t checksum(const uint8_t* d, int n);

    POKEY* m_pokey;
    CmdLine m_cmdLine;
    Drive m_drives[4];
    std::vector<uint8_t> m_frame;      // command frame being received
    bool m_frameReady = false;
    // A write command waits for its data frame
    int m_writeDrive = -1, m_writeSector = 0, m_writeLen = 0;
    std::vector<uint8_t> m_writeData;
    std::deque<Pending> m_out;         // response bytes, each after a delay
    int m_wait = 0;
    int m_activeDrive = -1;            // the drive answering (its LED)
    std::string m_lastError;
};

}  // namespace atari
