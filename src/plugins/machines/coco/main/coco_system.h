#pragma once

#include "coco_cart.h"
#include "coco_tape.h"
#include "machine_coco.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include "plugins/devices/mc6821/main/mc6821.h"
#include "plugins/devices/mc6847/main/mc6847.h"
#include "plugins/devices/mc6883/main/mc6883.h"
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class M6809;
class Acia6551;

namespace coco {

constexpr double MASTER_HZ = 14318180.0;      ///< the crystal; the SAM divides by 16 (0.89 MHz) or 8
constexpr double DRAGON_PAL_HZ = 14218000.0;  ///< the Dragon 32/64's crystal (0.889 MHz)
constexpr double COCO_PAL_HZ = 14237530.0;    ///< a PAL CoCo's
constexpr int TICKS_SLOW = 16;
constexpr int PIA_IRQ_DELAY = 14;             ///< tRS3, 1 us: a PIA's flag to its /IRQ output
/** The hi-res joystick interface's ramp: the comparator rises HIRES_OFFSET_US + the stick's
 *  10-bit position x HIRES_SPAN_US / 1023 after the DAC is written 0 (MAME's figures, tuned to
 *  the stock Multi-Vue). */
constexpr double HIRES_OFFSET_US = 560.0;
constexpr double HIRES_SPAN_US = 4856.0;

/** What a joystick port has on it. */
enum class JoyInterface {
    Normal,     ///< the stick's pots, read through the DAC and comparator (6 bits)
    HiRes,      ///< the Radio Shack 26-3028 Hi-Res Joystick Interface: a timed ramp
};

/**
 * The CoCo's dynamic RAM as chips: 4K x1 (one bank of 4 KB), 16K x1 (16
 * KB), 32K x1 (32 KB: the "64K" chips with one good half) or 64K x1. The
 * SAM's row and column (and RAS1 for a second bank of 4K/16K chips, not
 * fitted) select a byte; a missing bank reads $FF.
 */
class Ram {
public:
    void configure(int kb);
    int kb() const { return m_kb; }
    uint8_t* at(uint8_t bank, uint16_t row, uint16_t col);
    const uint8_t* at(uint8_t bank, uint16_t row, uint16_t col) const;
    std::vector<uint8_t>& bank0() { return m_bank[0]; }
    std::vector<uint8_t> m_bank[2];

private:
    int m_kb = 64;
    unsigned m_rowBits = 8, m_addrBits = 16, m_banks = 1;
};

/**
 * The keyboard: 7 rows (PIA0 PA0-PA6) by 8 columns (PIA0 PB0-PB7, driven
 * low one at a time):
 *   row 0  @ A B C D E F G        row 4  0 1 2 3 4 5 6 7
 *   row 1  H I J K L M N O        row 5  8 9 : ; , - . /
 *   row 2  P Q R S T U V W        row 6  ENTER CLEAR BREAK - - - - SHIFT
 *   row 3  X Y Z UP DOWN LEFT RIGHT SPACE
 * Host keys by name (positional; Backspace is LEFT, Home is CLEAR, Esc
 * BREAK) and the GUI's shifted-symbol names (exclaim, dquote, ...), which
 * press SHIFT with the CoCo key that has the symbol.
 *
 * The Dragon has the same keys and columns, its rows in another order:
 *   row 0  0 1 2 3 4 5 6 7        row 3  H I J K L M N O
 *   row 1  8 9 : ; , - . /        row 4  P Q R S T U V W
 *   row 2  @ A B C D E F G        row 5  X Y Z UP DOWN LEFT RIGHT SPACE
 * (row 6 as the CoCo's): setDragonLayout moves keys pressed by name or
 * typed; keyDown/keyUp are raw matrix positions.
 */
class Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    void keyDown(int col, int row) override { set(row, col, true); }
    void keyUp(int col, int row) override { set(row, col, false); }
    void clearKeys() override { reset(); }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    void enqueueText(const std::string& text) override;
    IPortDevice* getPort(int) override { return nullptr; }

    /** A key by name: false if the CoCo has no such key. */
    bool key(std::string name, bool down);
    /** The Dragon's rows (see above). */
    void setDragonLayout(bool on) { m_dragon = on; }
    bool dragonLayout() const { return m_dragon; }
    void set(int row, int col, bool down);
    /** Rows pressed in a column (bit per row, SHIFT held by typing included). */
    uint8_t column(int col) const;
    bool typing() const { return m_typing >= 0 || !m_queue.empty(); }
    /** Time passes (typing). */
    void clock(uint64_t ticks);

private:
    void pressChord(int code, bool down);
    bool m_dragon = false;
    uint8_t m_matrix[8] = {};      // per column: rows held
    int m_shifts = 0;              // SHIFT held by shifted-symbol keys and typing
    std::deque<int> m_queue;       // typed keys: row << 3 | col, 0x100 = with SHIFT
    int m_typing = -1;
    uint64_t m_wait = 0;
};

/**
 * The serial port's transmit line (PIA1 PA1, the bit banger): decodes the
 * frames BASIC sends (8N1 at 600 baud, as BASIC's printer driver, by
 * default) to a host file. CLI `serial file:PATH` (or `serial none`).
 */
class BitBanger : public IOHandler {
public:
    ~BitBanger() override;
    const char* name() const override { return "RS-232"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;
    bool setOption(const std::string& key, const std::string& value, std::string& err) override;

    /** PA1 at time `now` (ticks). */
    void txLevel(uint64_t now, bool level);
    void setBaud(int baud) { m_baud = baud > 0 ? baud : 600; }
    void setMasterHz(double hz) { m_hz = hz; }
    std::vector<uint8_t> sent;            ///< everything decoded

private:
    double m_hz = MASTER_HZ;
    int m_baud = 600;
    bool m_level = true;
    uint64_t m_lastEdge = 0;
    bool m_inFrame = false;
    uint64_t m_frameStart = 0;
    std::vector<std::pair<uint64_t, bool>> m_edges;
    FILE* m_file = nullptr;
    void decodeFrame(uint64_t end);
};

/**
 * The Dragon's parallel printer port: data on PIA0 port B (the keyboard's
 * column lines), STROBE on PIA1 PA1 (a byte is taken as it rises), BUSY
 * in on PIA1 PB0 (never busy here) and /ACK back on PIA1 CA1 (a 7 us low
 * pulse). CLI `serial file:PATH` (or `printer:PATH`, `serial none`).
 */
class Printer : public IOHandler {
public:
    ~Printer() override;
    const char* name() const override { return "Printer"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { m_strobe = true; }
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;

    /** STROBE's level and the data lines: true when a byte was taken (ACK it). */
    bool strobe(bool level, uint8_t data);
    std::vector<uint8_t> printed;         ///< everything printed

private:
    bool m_strobe = true;
    FILE* m_file = nullptr;
};

/** The audio, as the frontends pull it: 44.06 kHz mono (43.75 kHz on a Dragon). */
class Sound : public IOHandler, public IAudioOutput {
public:
    static constexpr int TICKS_PER_SAMPLE = 325;
    const char* name() const override { return "Sound"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    int nativeSampleRate() const override { return (int)(m_hz / TICKS_PER_SAMPLE); }
    void setMasterHz(double hz) { m_hz = hz; }
    int pullSamples(float* buffer, int maxSamples) override;
    /** The output at `level` for `ticks`. */
    void run(uint64_t ticks, float level);
    size_t buffered() const { return m_samples.size(); }

private:
    double m_hz = MASTER_HZ;
    std::deque<float> m_samples;
    double m_sum = 0;
    uint64_t m_acc = 0;
};

class System;

/** The Dragon 64's RS-232 port (its 6551) for the CLI `serial` (modem, loopback, tcp:PORT). */
class AciaPort : public IOHandler {
public:
    explicit AciaPort(System* sys) : m_sys(sys) {}
    const char* name() const override { return "RS-232"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;
    void getDeviceInfo(DeviceInfo& out) const override;
private:
    System* m_sys;
};

/** The screen, as the frontends find it. */
class Display : public IOHandler, public IVideoOutput {
public:
    explicit Display(Mc6847* vdg) : m_vdg(vdg) {}
    const char* name() const override { return "Video"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override { return {Mc6847::OUT_W, Mc6847::OUT_H, Mc6847::OUT_W, Mc6847::OUT_H}; }
    void renderFrame(uint32_t* buffer) override;
private:
    Mc6847* m_vdg;
};

/**
 * The cartridge port as an I/O handler: its device's description, and the
 * CLI `serial` for a Deluxe RS-232 Pak in it.
 */
class CartPort : public IOHandler {
public:
    explicit CartPort(System* sys) : m_sys(sys) {}
    const char* name() const override { return "Cartridge"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;
    /** Disks in the cartridge's controller (an FD-500, alone or in a Multi-Pak slot):
     *  unit 1-4 is Disk BASIC's DRIVE 0-3, as other machines number from 1. */
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;
    void getDeviceInfo(DeviceInfo& out) const override;
private:
    System* m_sys;
};

/**
 * The CoCo: the 6809E's bus as the SAM decodes it, with the VDG, the PIAs,
 * the keyboard, joysticks, sound, cassette, bit banger and cartridge port
 * on it. Time is kept in 14.31818 MHz ticks: each CPU cycle takes what the
 * SAM gives it (16 slow, 8 fast), I/O cycles bring the devices up to time
 * first; the 6809's internal ("dead") cycles put $FFFF on the bus.
 *
 * The Dragons (dragon32, dragon64) are the same board with differences:
 * the printer port instead of the bit banger, the keyboard's rows, PIA1
 * PB2 (RAM size on the 32, the ROM select on the 64), PIA0 decoded at
 * $FF00-$FF03 only (except on an unexpanded Dragon 32), and on the 64 a
 * 6551 at $FF04-$FF07 and a second 16 KB BASIC. PAL boards pad the VDG to
 * 312 lines (Mc6847::setPal) and run from their own crystal (masterHz);
 * a PAL CoCo also inverts HS to PIA0 CA1.
 */
class System : public IBus {
public:
    System(Model model, int ramKb, std::vector<uint8_t> extBasic, std::vector<uint8_t> colorBasic,
           Tv tv = Tv::Default);
    ~System() override;

    // IBus
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

    /** Power on (RAM cleared) or /RESET (the RESET button: RAM kept). */
    void powerOn();
    void resetButton();
    /** One instruction (or a halted cycle); returns CPU cycles at 0.89 MHz elapsed. */
    int step();
    void setCpu(M6809* cpu) { m_cpu = cpu; }
    uint64_t ticks() const { return m_ticks; }
    uint64_t slowCycles() const { return m_ticks / TICKS_SLOW; }

    /** Installed RAM (4, 16, 32 or 64 KB). */
    void setRamKb(int kb);
    int ramKb() const { return m_ram.kb(); }
    /** The byte at a physical RAM address (bank 0), for tests and the debugger. */
    uint8_t ramByte(uint32_t phys) const;

    // The cartridge port
    void setCart(std::unique_ptr<Cart> cart);
    Cart* cart() const { return m_cart.get(); }
    /** Take the cartridge out of the port (to move it into a Multi-Pak). */
    std::unique_ptr<Cart> takeCart() { auto c = std::move(m_cart); setCart(nullptr); return c; }

    // Joysticks: 0 right, 1 left; axis 0 X, 1 Y; 0-65535 (0 left / up)
    void setAxis(int port, int axis, uint16_t v) { m_axis[port & 1][axis & 1] = v; }
    uint16_t axis(int port, int axis) const { return m_axis[port & 1][axis & 1]; }
    void setButton(int port, bool down) { m_button[port & 1] = down; }
    /** The joystick interface on a port (0 right, 1 left). */
    void setJoyInterface(int port, JoyInterface j) { m_joyIf[port & 1] = j; }
    JoyInterface joyInterface(int port) const { return m_joyIf[port & 1]; }
    /** The port the host mouse moves: the first with a hi-res interface, else the right. */
    int mousePort() const { return m_joyIf[0] != JoyInterface::HiRes && m_joyIf[1] == JoyInterface::HiRes ? 1 : 0; }

    /** The level the sound circuit outputs now (-1..1). */
    float soundLevel() const;

    Model model() const { return m_model; }
    bool dragon() const { return isDragon(m_model); }
    bool pal() const { return m_pal; }
    /** The crystal: 14.31818 MHz (NTSC), 14.218 MHz (a PAL Dragon), 14.23753 MHz (a PAL CoCo). */
    double masterHz() const { return m_masterHz; }

    /** The Dragon's 16 KB BASIC at $8000; on the Dragon 64 also its 64K-mode BASIC. */
    void setDragonRoms(const std::vector<uint8_t>& basic, const std::vector<uint8_t>& basic64k);
    /** Dragon 64: the 64K-mode BASIC is in the map (PIA1 PB2 low). */
    bool rom64kSelected() const { return m_rom64Sel; }
    /** The Dragon 64's 6551 at $FF04-$FF07 (nullptr on others). */
    Acia6551* acia() const { return m_acia.get(); }

    Mc6847 vdg;
    Mc6883 sam;
    Mc6821 pia0, pia1;
    Keyboard keyboard;
    Tape tape;
    BitBanger rs232;
    Sound sound;
    Display display{&vdg};
    CartPort cartPort{this};
    Printer printer;
    AciaPort aciaPort{this};

private:
    std::vector<uint8_t> state() const;
    uint8_t access(uint16_t a, bool write, uint8_t v);
    void sync(uint64_t to);
    void updateLines();
    void keyboardUpdate();
    void joystickUpdate();
    void hiresTrigger(bool selectsChanged = false);
    uint64_t hiresRampTicks(int port, int axis) const;
    uint64_t vdgNow() const;
    void ramSizeUpdate();
    void fetchVideo(int n, uint16_t* out);
    void soundChanged();
    // The Dragons (machine_dragon.cpp)
    void wireDragon();
    void dragonRamSizeUpdate();
    void dragonRomSelect();
    void printerStrobe();
    void dragonClock(uint64_t ticks);
    bool piaDecoded(uint16_t a) const;
    const uint8_t* romByte(uint16_t a) const;

    Model m_model;
    bool m_pal = false;
    double m_masterHz = MASTER_HZ;
    BusConfig m_cfg;
    Ram m_ram;
    std::vector<uint8_t> m_rom;                // 16 KB: Extended BASIC at 0, Color BASIC at $2000
    bool m_haveExt = false;
    std::vector<uint8_t> m_rom64;              // Dragon 64: the 64K-mode BASIC
    bool m_rom64Sel = false;
    std::unique_ptr<Acia6551> m_acia;          // Dragon 64
    uint64_t m_aciaAcc = 0;
    uint64_t m_ackLeft = 0;                    // printer /ACK low for this many more ticks
    std::unique_ptr<Cart> m_cart;
    M6809* m_cpu = nullptr;

    uint64_t m_ticks = 0, m_dev = 0, m_reported = 0;
    uint64_t m_instr = 0;                      // ticks into the current instruction
    int m_accesses = 0;
    bool m_halt = false, m_nmi = false;
    bool m_cartLine = true;

    uint16_t m_axis[2][2] = {{32768, 32768}, {32768, 32768}};
    bool m_button[2] = {};
    JoyInterface m_joyIf[2] = {JoyInterface::Normal, JoyInterface::Normal};
    bool m_ramp = false;                       // the hi-res ramp running (the DAC at 0)
    uint64_t m_rampStart = 0;

    // The VDG's place in the current sync(), to time its HS / FS edges
    uint64_t m_vdgFrom = 0;
    int m_vdgTick0 = 0, m_vdgLines = 0;
    float m_level = 0;
};

/** The System behind a machine made by create() (nullptr if not a CoCo). */
System* systemOf(MachineDescriptor* m);

/**
 * The Dragon's ROMs from dragonFirmwareDir() (machine_dragon.cpp): the
 * 16 KB BASIC and, for the 64, its 64K-mode BASIC. False (and `missing`
 * naming the file) without the BASIC.
 */
bool loadDragonRoms(Model model, std::vector<uint8_t>& basic, std::vector<uint8_t>& basic64k, std::string& missing);
/** Without a Dragon BASIC: a stand-in at $8000 that says so on the screen. */
std::vector<uint8_t> dragonPlaceholderRom();
/** DragonDOS's ROM: `path`, or ddos12a.rom (...) from dragonFirmwareDir(); empty with `err` if none. */
std::vector<uint8_t> loadDragonDosRom(const std::string& path, std::string& err);

}  // namespace coco
