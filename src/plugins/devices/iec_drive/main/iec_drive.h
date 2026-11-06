#pragma once

#include "libdevices/main/io_handler.h"
#include "plugins/devices/ieee488/main/cbm_host_dos.h"
#include "plugins/devices/ieee488/main/cbm_image_dos.h"
#include <cstdint>
#include <functional>
#include <string>

/**
 * A Commodore serial-bus (IEC) disk drive at the line level: the drive's side
 * of the serial protocol (ATN, CLK, DATA; see "Commodore Peripheral Bus" /
 * the 1541 ROM), serving a host folder (the default) or a disk image through
 * the same DOS as the PET's IEEE-488 drive (CbmHostDos, CbmImageDos).
 *
 * The machine tells it what the computer drives (`setComputerLines`, true =
 * asserted, i.e. pulled low) and reads back what the bus carries
 * (`clkLine()`/`dataLine()`, true = released/high, as the computer's input
 * pins see them through the bus's wired AND). Time comes from `tick()` in
 * cycles of `setClockHz`.
 *
 * The drive talks back as a 1541 does: each bit is held ~60 us, a byte frame
 * waits up to 1 ms for the listener's acknowledgement, the last byte goes
 * with EOI, and a file that is not there is answered by never sending (the
 * computer's read times out: FILE NOT FOUND).
 *
 * With setFastSerial() it is a 1571 on the C128's fast serial bus (the
 * computer's CIA1 shift register, SRQ as its clock), as the C128 KERNAL
 * expects ($E33B-$E4D1): a computer that shifts out a byte just before
 * asserting ATN is fast; the drive answers with a byte of its own when it
 * is ready for the first command byte, so the KERNAL marks it fast ($0A1C).
 * Then data bytes outside ATN go through the shift registers in the same
 * handshake (the talker pulls CLK and shifts the byte; the listener pulls
 * DATA to acknowledge), and the burst commands ("U0" + bytes: read, write,
 * inquire disk) move whole sectors: a byte for each CLK toggle of the
 * computer (read), or for each CLK toggle of the drive (write), the status
 * when the computer pulls CLK after a written sector.
 */
class IecDrive : public IOHandler {
public:
    explicit IecDrive(int unit = 8);
    ~IecDrive() override = default;

    // IOHandler (no registers)
    const char* name() const override { return m_name.c_str(); }
    void        setName(const std::string& n) override { m_name = n; }
    uint32_t    baseAddr() const override { return 0; }
    uint32_t    addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    /** The machine's clock, for the protocol's microsecond timings. */
    void setClockHz(double hz) { m_cyclesPerUs = hz / 1e6; }
    /** What the computer drives (true = asserted / pulled low). */
    void setComputerLines(bool atn, bool clk, bool data);
    /** The bus levels (true = high, released): what the computer reads. */
    bool clkLine() const  { return !(m_cClk || m_dClk); }
    bool dataLine() const { return !(m_cData || m_dData); }
    bool atnLine() const  { return !m_cAtn; }

    /** Make it a 1571 on fast serial: `toHost` shifts a byte into the
     *  computer's serial register. */
    void setFastSerial(std::function<void(uint8_t)> toHost) { m_fastToHost = std::move(toHost); }
    /** A byte the computer shifted out on fast serial. */
    void fastByteFromHost(uint8_t b);
    /** The computer announced fast serial for the current ATN sequence. */
    bool hostFast() const { return m_hostFast; }
    /** Load address of a `$` listing (Commodore drives send $0401). */
    void setDirectoryLoadAddress(uint16_t a) { m_host.setDirectoryLoadAddress(a); }

private:
    enum class St {
        Idle,            // not addressed: lines released
        RxWaitReady,     // listener: holding DATA, waiting for the talker to release CLK
        RxWaitStart,     // listener: DATA released, waiting for CLK (or EOI timeout)
        RxEoiAck,        // listener: acknowledging EOI with a DATA pulse
        RxBits,          // listener: sampling bits on CLK going high
        RxFrameEnd,      // listener: 8 bits in, waiting for CLK low to acknowledge
        Turnaround,      // to talker: waiting for the computer to release CLK
        TxStart,         // talker: holding CLK, waiting for the listener (DATA low) and Tbb
        TxWaitListener,  // talker: CLK released, waiting for DATA high
        TxEoiWait,       // talker: last byte: waiting for the EOI acknowledge (DATA low)
        TxEoiRelease,    // talker: ... and for DATA high again
        TxBitSetup,      // talker: CLK low, data bit on DATA
        TxBitValid,      // talker: CLK high, the bit valid
        TxFrame,         // talker: 8 bits out, waiting for DATA low (acknowledge)
        Silent,          // talker with nothing to send / after the last byte
        BurstArm,        // burst: waiting for the computer to release CLK
        BurstOut,        // burst read / inquire: a byte for each computer CLK toggle
        BurstIn,         // burst write: a byte for each drive CLK toggle
        BurstStatus,     // burst write: sector in, waiting for the computer to pull CLK
        BurstDone,       // burst write: status sent, waiting for CLK released
    };

    void enter(St s) { m_st = s; m_t0 = m_now; }
    double us() const { return (double)(m_now - m_t0) / m_cyclesPerUs; }
    void step();
    void atnStart();
    void atnEnd();
    void command(uint8_t b);
    void received(uint8_t b);
    bool busClk() const  { return m_cClk || m_dClk; }      // true = asserted
    bool busData() const { return m_cData || m_dData; }

    std::string m_name = "Drive8";
    double m_cyclesPerUs = 1.0;
    uint64_t m_now = 0, m_t0 = 0;
    St m_st = St::Idle;

    // Lines (true = asserted)
    bool m_cAtn = false, m_cClk = false, m_cData = false;   // the computer
    bool m_dClk = false, m_dData = false;                   // the drive
    bool m_prevAtn = false;
    bool m_prevClk = false;

    // Bytes
    uint8_t m_byte = 0;
    int m_bits = 0;
    bool m_eoi = false;            // listener: EOI seen for this byte
    bool m_underAtn = false;       // the byte being received is a command

    // The DOS
    CbmHostDos m_host;
    CbmImageDos m_image;
    ICbmDos* m_dos = &m_host;
    bool m_imageMounted = false;
    bool m_listener = false, m_talker = false;
    bool m_lastWasOurListen = false, m_lastWasOurTalk = false;
    bool m_hasPending = false;
    uint8_t m_pendingByte = 0;
    int m_pendingStatus = 0;
    int m_pendingChannel = -1;
    uint8_t m_txByte = 0;
    bool m_txLast = false;

    // Fast serial (1571)
    std::function<void(uint8_t)> m_fastToHost;
    bool m_fastAnnounce = false;   // a byte from the computer before this ATN
    bool m_hostFast = false;       // this ATN sequence is fast
    bool m_burstClk = false;       // the computer's CLK at the last burst byte
    bool fast() const { return m_fastToHost && m_hostFast; }
    void startBurstSector();
    void fastload(const std::string& name);
};
