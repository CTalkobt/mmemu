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

namespace coco {

constexpr double MASTER_HZ = 14318180.0;      ///< the crystal; the SAM divides by 16 (0.89 MHz) or 8
constexpr int TICKS_SLOW = 16;

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
    void set(int row, int col, bool down);
    /** Rows pressed in a column (bit per row, SHIFT held by typing included). */
    uint8_t column(int col) const;
    bool typing() const { return m_typing >= 0 || !m_queue.empty(); }
    /** Time passes (typing). */
    void clock(uint64_t ticks);

private:
    void pressChord(int code, bool down);
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
    std::vector<uint8_t> sent;            ///< everything decoded

private:
    int m_baud = 600;
    bool m_level = true;
    uint64_t m_lastEdge = 0;
    bool m_inFrame = false;
    uint64_t m_frameStart = 0;
    std::vector<std::pair<uint64_t, bool>> m_edges;
    FILE* m_file = nullptr;
    void decodeFrame(uint64_t end);
};

/** The audio, as the frontends pull it: 44.06 kHz mono. */
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
    int nativeSampleRate() const override { return (int)(MASTER_HZ / TICKS_PER_SAMPLE); }
    int pullSamples(float* buffer, int maxSamples) override;
    /** The output at `level` for `ticks`. */
    void run(uint64_t ticks, float level);
    size_t buffered() const { return m_samples.size(); }

private:
    std::deque<float> m_samples;
    double m_sum = 0;
    uint64_t m_acc = 0;
};

class System;

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
 */
class System : public IBus {
public:
    System(Model model, int ramKb, std::vector<uint8_t> extBasic, std::vector<uint8_t> colorBasic);
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

    /** The level the sound circuit outputs now (-1..1). */
    float soundLevel() const;

    Model model() const { return m_model; }
    Mc6847 vdg;
    Mc6883 sam;
    Mc6821 pia0, pia1;
    Keyboard keyboard;
    Tape tape;
    BitBanger rs232;
    Sound sound;
    Display display{&vdg};
    CartPort cartPort{this};

private:
    std::vector<uint8_t> state() const;
    uint8_t access(uint16_t a, bool write, uint8_t v);
    void sync(uint64_t to);
    void updateLines();
    void keyboardUpdate();
    void joystickUpdate();
    void ramSizeUpdate();
    void fetchVideo(int n, uint16_t* out);
    void soundChanged();

    Model m_model;
    BusConfig m_cfg;
    Ram m_ram;
    std::vector<uint8_t> m_rom;                // 16 KB: Extended BASIC at 0, Color BASIC at $2000
    bool m_haveExt = false;
    std::unique_ptr<Cart> m_cart;
    M6809* m_cpu = nullptr;

    uint64_t m_ticks = 0, m_dev = 0, m_reported = 0;
    uint64_t m_instr = 0;                      // ticks into the current instruction
    int m_accesses = 0;
    bool m_halt = false, m_nmi = false;
    bool m_cartLine = true;

    uint16_t m_axis[2][2] = {{32768, 32768}, {32768, 32768}};
    bool m_button[2] = {};
    float m_level = 0;
};

/** The System behind a machine made by create() (nullptr if not a CoCo). */
System* systemOf(MachineDescriptor* m);

}  // namespace coco
