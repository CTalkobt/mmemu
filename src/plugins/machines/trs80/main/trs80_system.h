#pragma once

#include "machine_trs80.h"
#include "trs80_serial.h"
#include "trs80_tape.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include <deque>
#include <memory>
#include <string>
#include <vector>

class Z80;

namespace trs80 {

/** The built-in character shapes: 128 characters, 12 rows, bit 5 leftmost of six dots. */
extern const uint8_t BUILTIN_FONT[128][12];

constexpr double M1_MASTER_HZ = 10644480.0;   ///< Model I: the 10.6445 MHz crystal; the Z80 at /6 (1.774 MHz)
constexpr double M3_MASTER_HZ = 20275200.0;   ///< Model III/4: the Z80 at /10 (2.028 MHz) or /5 (4.055 MHz, Model 4)
constexpr int M1_TICKS = 6;
constexpr int M3_TICKS = 10;

/** The Model I's double-density adapter in the Expansion Interface. */
enum class Doubler { None, Percom, Tandy, Both };

/**
 * The keyboard: 8 rows of 8 keys, read at $3800-$3BFF (bit n of the
 * address selects row n; the selected rows' keys are ORed, 1 = down):
 *   row 0  @ A B C D E F G         row 4  0 1 2 3 4 5 6 7
 *   row 1  H I J K L M N O         row 5  8 9 : ; , - . /
 *   row 2  P Q R S T U V W         row 6  ENTER CLEAR BREAK UP DOWN LEFT RIGHT SPACE
 *   row 3  X Y Z                   row 7  SHIFT (L) SHIFT (R, III/4) CTRL CAPS F1 F2 F3 (4)
 * Host keys by name (Backspace is LEFT, Home CLEAR, Esc BREAK) and the
 * GUI's shifted-symbol names (exclaim, dquote...), which press SHIFT with
 * the TRS-80 key that has the symbol; typed text is held a key at a time.
 */
class Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0x3800; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    void keyDown(int row, int col) override { set(row, col, true); }
    void keyUp(int row, int col) override { set(row, col, false); }
    void clearKeys() override { reset(); }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    void enqueueText(const std::string& text) override;
    IPortDevice* getPort(int) override { return nullptr; }

    /** A key by name: false if the TRS-80 has no such key. */
    bool key(std::string name, bool down);
    void set(int row, int bit, bool down);
    /** What $3800 + `addr` (its low 8 bits select rows) reads. */
    uint8_t read(uint16_t addr) const;
    bool typing() const { return m_typing >= 0 || !m_queue.empty(); }
    /** Time passes (typing); `hz` the clock `ticks` count. */
    void clock(uint64_t ticks, double hz);
    /** The Model 4's keys (CTRL, CAPS, F1-F3) and its second SHIFT. */
    void setModel4(bool on) { m_model4 = on; }
    void setModel3(bool on) { m_model3 = on; }

private:
    void press(int code, bool down);
    uint8_t m_rows[8] = {};
    int m_shifts = 0;                   // SHIFT held by symbol keys and typing
    std::deque<int> m_queue;            // typed keys: row << 3 | bit, 0x100 with SHIFT
    int m_typing = -1;
    double m_wait = 0;                  // seconds
    bool m_model3 = false, m_model4 = false;
};

class System;

/** The screen, as the frontends find it. */
class Display : public IOHandler, public IVideoOutput {
public:
    explicit Display(System* s) : m_sys(s) {}
    const char* name() const override { return "Video"; }
    uint32_t baseAddr() const override { return 0x3C00; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override;
    void renderFrame(uint32_t* buffer) override;
private:
    System* m_sys;
};

/** The audio: the cassette output (with the motor off) and the Model 4's port $90. */
class Sound : public IOHandler, public IAudioOutput {
public:
    const char* name() const override { return "Sound"; }
    uint32_t baseAddr() const override { return 0x90; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    int nativeSampleRate() const override { return (int)(m_hz / m_ticksPerSample); }
    int pullSamples(float* buffer, int maxSamples) override;
    void setMasterHz(double hz) { m_hz = hz; m_ticksPerSample = (int)(hz / 44100.0 + 0.5); }
    /** The output at `level` (-1..1) for `ticks`. */
    void run(uint64_t ticks, float level);
    size_t buffered() const { return m_samples.size(); }
private:
    double m_hz = M1_MASTER_HZ;
    int m_ticksPerSample = 241;
    std::deque<float> m_samples;
    double m_sum = 0;
    uint64_t m_acc = 0;
};

/**
 * The floppy drives, as the frontends find them: unit 1-4 is DOS's drive
 * :0-:3. Images: DMK, JV1, JV3 (and .dsk as any of them).
 */
class DiskPort : public IOHandler {
public:
    explicit DiskPort(System* s) : m_sys(s) {}
    const char* name() const override { return "Floppy"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;
    void getDeviceInfo(DeviceInfo& out) const override;
private:
    System* m_sys;
};

/** What a machine has fitted. */
struct Config {
    int ramKb = 48;                 ///< Model I: 4, 16, 32, 48; III: 16, 32, 48; 4: 64, 128
    bool expansion = true;          ///< Model I: the Expansion Interface (RAM over 16K, disk, printer, RTC)
    bool lowercase = false;         ///< Model I: the lower case modification
    Doubler doubler = Doubler::None;///< Model I: a double-density adapter in the EI
    bool rs232 = false;             ///< the RS-232 board
    bool fdc = true;                ///< Model III: the disk controller
};

/**
 * A TRS-80: the Z80's memory and I/O as each model decodes them, the
 * keyboard, video, cassette, printer, RS-232 board, the floppy controller
 * and the interrupt hardware.
 *
 * Model I: ROM $0000-$2FFF (Level I $0000-$0FFF), the Expansion
 * Interface's registers at $37E0-$37EF (interrupt latch / drive select,
 * cassette select, printer, the FD1771), keyboard $3800, video $3C00
 * (1 KB; D6 not stored without the lower case modification: it reads back
 * as NOT (D5 or D7)), RAM from $4000. Port $FF: cassette (bits 1-0 out,
 * bit 7 in), motor (2), 32-column mode (3). The EI's 40 Hz clock and the
 * FDC's INTRQ set the latch at $37E0 (bits 7 and 6) and pull /INT;
 * reading the latch clears the clock's bit. RESET is an NMI.
 *
 * Model III: ROM $0000-$37FF (the printer at $37E8), keyboard $3800, video
 * $3C00, RAM from $4000. Ports: $E0 interrupt mask (write) and latch
 * (read, active low: 7 I/O bus, 6-4 RS-232 error, receive, send, 2 the
 * 30 Hz clock, 1-0 1500 baud cassette fall, rise); $E4 NMI mask (7 FDC
 * INTRQ, 6 motor timeout) and latch (active low, 5 RESET); $EC (read:
 * acknowledge the clock; write: 6 Model 4 fast clock, 3 alternate
 * characters, 2 32 columns, 1 cassette motor); $F0-$F3 the FD1793; $F4
 * drive select (7 double density, 6 WAIT until DRQ or INTRQ, 4 side, 3-0
 * drives); $F8 printer; $FF cassette.
 *
 * Model 4: as the III, plus port $84 (7 the video page, 6-4 the 32K
 * banks, 3 inverse video, 2 80 x 24, 1-0 the memory map: 0 the III's, 1
 * RAM for the ROM, 2 RAM but the keyboard $F400 and video $F800, 3 all
 * RAM), $88/$89 the CRTC's start address, $90 the sound bit, the 60 Hz
 * clock at 4 MHz. The 4P's boot ROM is at $0000-$0FFF until port $9C
 * bit 0 is cleared.
 *
 * Time is kept in master clock ticks (M1_MASTER_HZ, M3_MASTER_HZ); the
 * Z80's T-states take M1_TICKS (6) or 10 (5 at 4 MHz) each.
 */
class System : public IBus {
public:
    System(Model model, const Config& cfg, std::vector<uint8_t> rom);
    ~System() override;

    // IBus: the Z80's memory
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }
    uint8_t read8(uint32_t addr) override;
    void write8(uint32_t addr, uint8_t v) override;
    uint8_t peek8(uint32_t addr) override;
    size_t stateSize() const override;
    void saveState(uint8_t* buf) const override;
    void loadState(const uint8_t* buf) override;
    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

    /** The Z80's I/O ports. */
    uint8_t in(uint8_t port);
    void out(uint8_t port, uint8_t v);
    uint8_t peekPort(uint8_t port);
    /** The I/O space as a bus (made with the System; the machine's descriptor owns it). */
    IBus* ioBus() { return m_io; }

    /** Power on (RAM cleared) or the RESET button (Model I: NMI; III/4: /RESET). */
    void powerOn();
    void resetButton();
    /** One instruction (or a WAIT); returns device cycles (the base CPU clock) elapsed. */
    int step();
    void setCpu(Z80* cpu) { m_cpu = cpu; }
    Z80* cpu() const { return m_cpu; }
    uint64_t ticks() const { return m_ticks; }
    /** Device cycles: master ticks / 6 (Model I) or / 10 (III/4). */
    uint64_t deviceCycles() const { return m_ticks / (uint64_t)m_baseTicks; }

    Model model() const { return m_model; }
    double masterHz() const { return m_masterHz; }
    int baseTicks() const { return m_baseTicks; }
    /** T-states' length now (Model 4: 5 at 4 MHz). */
    int ticksPerT() const { return m_ticksPerT; }
    const Config& cfg() const { return m_config; }
    /** Change what is fitted (a power-on follows). */
    void configure(const Config& cfg);

    /** The byte at a physical RAM address (Model 4: 0-128K; others from $4000 = 0). */
    uint8_t ramByte(uint32_t phys) const { return phys < m_ram.size() ? m_ram[phys] : 0xFF; }
    /** Video RAM (2 KB on the Model 4, 1 KB others). */
    const uint8_t* videoRam() const { return m_video; }
    uint8_t videoByte(int i) const { return m_video[i & 0x7FF]; }
    /** Replace the character generator (a ROM image; empty: built in). */
    void setCharGen(const std::vector<uint8_t>& rom);

    // The display's state
    bool wide() const;                  ///< 32 (40) column mode
    bool mode80() const { return isModel4(m_model) && (m_port84 & 0x04); }
    bool inverse() const { return isModel4(m_model) && (m_port84 & 0x08); }
    bool altChars() const { return !isModel1(m_model) && (m_portEC & 0x08); }
    uint16_t crtcStart() const { return m_crtcStart; }
    /** The glyph row (bit 7 the leftmost dot) of character `c`, line `ra`. */
    uint8_t glyph(int c, int ra) const { return m_font[c & 0xFF][ra % 12]; }

    // The interrupt hardware (tests)
    uint8_t intLatch() const { return m_intLatch; }
    uint8_t intMask() const { return m_intMask; }
    uint8_t nmiLatch() const { return m_nmiLatch; }
    uint8_t port84() const { return m_port84; }
    bool waiting() const { return m_wait; }
    bool fast() const { return m_ticksPerT == 5; }
    /** Model 4: select the 2 MHz or 4 MHz clock as port $EC bit 6 does. */
    void setFast(bool on);

    Wd17xx& fdc() { return *m_fdc; }
    bool hasFdc() const;
    /** The disk image paths per drive. */
    std::string diskPath(int drive) const { return drive >= 0 && drive < 4 ? m_diskPath[drive] : ""; }
    bool mountDisk(int drive, const std::string& path);
    void ejectDisk(int drive);

    Keyboard keyboard;
    Tape tape;
    Sound sound;
    Printer printer;
    Uart uart;
    Display display{this};
    DiskPort disks{this};

private:
    std::vector<uint8_t> state() const;
    void advance(uint64_t ticks);
    void hardwareReset();
    void updateIrq();
    void updateNmi();
    uint8_t access(uint16_t a, bool write, uint8_t v, bool peek);
    uint8_t m1Access(uint16_t a, bool write, uint8_t v, bool peek);
    uint8_t m3Access(uint16_t a, bool write, uint8_t v, bool peek);
    uint8_t m4Access(uint16_t a, bool write, uint8_t v, bool peek);
    uint8_t fdcRead(int reg, bool peek);
    void fdcWrite(int reg, uint8_t v);
    void driveSelect(uint8_t v);
    void videoWrite(uint16_t index, uint8_t v);
    uint8_t m4Ram(uint16_t a) const;
    uint8_t& m4RamRef(uint16_t a);
    void portFFOut(uint8_t v);
    void portECOut(uint8_t v);
    void setDensity(bool dd);
    void buildFont(const std::vector<uint8_t>& charGen);
    float soundLevel() const;
    Tape::Speed tapeSpeed() const;

    Model m_model;
    Config m_config;
    BusConfig m_cfg;
    IBus* m_io = nullptr;
    Z80* m_cpu = nullptr;
    double m_masterHz;
    int m_baseTicks, m_ticksPerT;

    std::vector<uint8_t> m_rom;
    std::vector<uint8_t> m_ram;         // Model I/III: from $4000; Model 4: physical 64/128K
    uint8_t m_video[2048] = {};
    uint8_t m_font[256][12] = {};       // bit 7 the leftmost dot

    // Model I
    uint8_t m_portFF = 0;               // the last write: 32 columns (3), motor (2), cassette (1-0)
    bool m_dd = false;                  // the doubler's double-density controller selected
    // Model III/4
    uint8_t m_portEC = 0, m_port84 = 0;
    bool m_romIn = true;                // 4P: the boot ROM in
    uint8_t m_crtcReg = 0;
    uint16_t m_crtcStart = 0;
    bool m_soundBit = false;
    int m_cassOut = 0;

    // Interrupts
    uint8_t m_intLatch = 0, m_intMask = 0;
    uint8_t m_nmiLatch = 0x01, m_nmiMask = 0x20;
    bool m_resetNmi = false;
    uint64_t m_resetNmiLeft = 0;
    uint64_t m_rtcAcc = 0;

    // The floppies
    std::unique_ptr<Wd17xx> m_fdc;
    std::string m_diskPath[4];
    bool m_drq = false, m_intrq = false;
    bool m_wait = false;
    uint64_t m_motorLeft = 0;           // the drive motors' one-shot
    uint8_t m_select = 0;

    uint64_t m_ticks = 0, m_reported = 0, m_soundAcc = 0;
};

/** The System behind a machine made by create() (nullptr if not a TRS-80). */
System* systemOf(MachineDescriptor* m);

}  // namespace trs80
