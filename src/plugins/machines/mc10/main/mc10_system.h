#pragma once

#include "machine_mc10.h"
#include "mc10_cart.h"
#include "plugins/machines/coco/main/coco_tape.h"
#include "plugins/devices/mc6847/main/mc6847.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

class M6801;

namespace mc10 {

constexpr double MASTER_HZ = 14318180.0;      ///< 4x the VDG's 3.579545 MHz; the 6803's E is a sixteenth
constexpr double ALICE_HZ = 14237530.0;       ///< the Alice's PAL board (as XRoar assumes)
constexpr int TICKS_PER_CYCLE = 16;

/**
 * The keyboard: 8 columns (the 6803's port 1, strobed low) by 7 rows,
 * rows 0-5 read at $BFFF bits 0-5, row 6 on port 2's P21:
 *         col 0  1  2  3  4  5  6     7
 *   row 0  @     A  B  C  D  E  F     G
 *   row 1  H     I  J  K  L  M  N     O
 *   row 2  P     Q  R  S  T  U  V     W
 *   row 3  X     Y  Z  -  -  -  ENTER SPACE
 *   row 4  0     1  2  3  4  5  6     7
 *   row 5  8     9  :  ;  ,  -  .     /
 *   row 6  CTRL  -  BREAK -  -  -  -  SHIFT
 * The arrows are CONTROL with A (left), S (right), W (up), Z (down), Q
 * (line delete). The Alice has the same matrix with AZERTY legends: A and
 * Q swapped, Z and W swapped, M where ; is, ; where / is, / where M is
 * (its arrows are the same keys: CONTROL with Q, S, Z, W, A).
 * Host keys by name (positional; Backspace is CONTROL+left, Esc BREAK) and
 * the GUI's shifted-symbol names; typed text by character.
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

    /** A key by name: false if the machine has no such key. */
    bool key(std::string name, bool down);
    void setAlice(bool on) { m_alice = on; }
    void set(int row, int col, bool down);
    /** Rows held in a column (bit per row; SHIFT/CONTROL held by chords and typing included). */
    uint8_t column(int col) const;
    bool typing() const { return m_typing >= 0 || !m_queue.empty(); }
    /** Time passes (typing), in master ticks. */
    void clock(uint64_t ticks);

private:
    void press(int code, bool down);
    bool m_alice = false;
    uint8_t m_matrix[8] = {};
    int m_shifts = 0, m_ctrls = 0;     // SHIFT / CONTROL held by chords and typing
    std::deque<int> m_queue;
    int m_typing = -1;
    uint64_t m_wait = 0;
};

/**
 * The serial port's output (P20, shared with the cassette): bit-banged by
 * BASIC's printer driver (8N1, 600 baud by default), decoded to a host
 * file: CLI `serial file:PATH` (`serial none`), option `baud`. The TP-10
 * thermal printer (or any serial printer, or an RS-232 device on the port)
 * receives the same bytes; with `tp10` the capture is the TP-10's paper:
 * 32 columns, CR ends a line.
 */
class SerialPort : public IOHandler {
public:
    ~SerialPort() override;
    const char* name() const override { return "Serial"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;
    bool setOption(const std::string& key, const std::string& value, std::string& err) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    /** The line's level at `now` (ticks). */
    void txLevel(uint64_t now, bool level);
    void setMasterHz(double hz) { m_hz = hz; }
    void setBaud(int baud) { m_baud = baud > 0 ? baud : 600; }
    int baud() const { return m_baud; }
    /** TP-10 paper (32 columns) instead of the raw bytes. */
    void setTp10(bool on) { m_tp10 = on; m_column = 0; }
    bool tp10() const { return m_tp10; }
    std::vector<uint8_t> sent;            ///< every byte decoded
    std::string paper;                    ///< the TP-10's printout

private:
    void decode(uint64_t now);
    void deliver(uint8_t v);
    double m_hz = MASTER_HZ;
    int m_baud = 600;
    bool m_level = true, m_inFrame = false, m_tp10 = false;
    uint64_t m_frameStart = 0;
    std::vector<std::pair<uint64_t, bool>> m_edges;
    FILE* m_file = nullptr;
    int m_column = 0;
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
    int nativeSampleRate() const override { return (int)(m_hz / TICKS_PER_SAMPLE); }
    void setMasterHz(double hz) { m_hz = hz; }
    int pullSamples(float* buffer, int maxSamples) override;
    void run(uint64_t ticks, float level);
    size_t buffered() const { return m_samples.size(); }
private:
    double m_hz = MASTER_HZ;
    std::deque<float> m_samples;
    double m_sum = 0;
    uint64_t m_acc = 0;
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

class System;

/**
 * The cassette recorder as the frontends drive it. The MC-10 has no motor
 * control, so a tape goes in paused (as XRoar does it): type CLOAD, then
 * `tape play`. The .c10 loader types CLOAD and presses PLAY itself.
 */
class TapeDeck : public IOHandler {
public:
    explicit TapeDeck(System* sys) : m_sys(sys) {}
    const char* name() const override { return "Tape"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool mountTape(const std::string& path) override;
    void controlTape(const std::string& op) override;
    bool startTapeRecord() override;
    void stopTapeRecord() override;
    bool saveTapeRecording(const std::string& path) override;
    bool isTapeRecording() const override;
private:
    System* m_sys;
};

/** The expansion port as an I/O handler (its device's description). */
class ExpansionPort : public IOHandler {
public:
    explicit ExpansionPort(System* sys) : m_sys(sys) {}
    const char* name() const override { return "Expansion"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    void getDeviceInfo(DeviceInfo& out) const override;
private:
    System* m_sys;
};

/**
 * The MC-10 board: the 6803's external bus as the board decodes it (the
 * top two address lines into a 2-to-4 decoder, as XRoar has it):
 *   $0000-$3FFF  nothing (the bus floats: the address's low byte, which
 *                the multiplexed bus left on it)
 *   $4000-$7FFF  the 4 KB of RAM where A12 is low ($4000, mirrored at $6000)
 *   $8000-$BFFF  read: the keyboard rows (bits 0-5); write: the latch
 *                (bit 7 sound, 6 CSS, 5 A/G, 4 GM0, 3 GM1, 2 GM2 and INT/EXT)
 *   $C000-$FFFF  the 8 KB ROM (mirrored at $C000)
 * The expansion port's device sees each cycle first and may take it.
 * Port 1 strobes the keyboard's columns; port 2: P20 out (cassette and
 * serial), P21 in (keyboard row 6), P22 in (RS-232 receive, idle high),
 * P23 in (the printer's ready: low when ready), P24 in (cassette).
 *
 * The VDG fetches the internal RAM itself (its own address counter: the
 * screen is at $4000); D7 is its A/S, D6 INV. Time is kept in ticks of
 * the master clock, 16 per E cycle; external accesses bring the devices up
 * to their cycle first.
 */
class System : public IBus {
public:
    System(Model model, std::vector<uint8_t> rom);
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

    /** Power on (RAM cleared) or the RESET button (RAM kept). */
    void powerOn();
    void resetButton();
    /** One instruction; returns E cycles elapsed. */
    int step();
    void setCpu(M6801* cpu);
    M6801* cpu() const { return m_cpu; }
    uint64_t ticks() const { return m_ticks; }
    uint64_t cycles() const { return m_ticks / TICKS_PER_CYCLE; }

    Model model() const { return m_model; }
    double masterHz() const { return m_masterHz; }
    /** The $BFFF latch's last value. */
    uint8_t latch() const { return m_latch; }
    /** The internal 4 KB of RAM (offset from $4000). */
    uint8_t ram(uint16_t off) const { return m_ram[off & 0x0FFF]; }
    void setRam(uint16_t off, uint8_t v) { m_ram[off & 0x0FFF] = v; }

    // The expansion port
    void setCart(std::unique_ptr<Cart> cart);
    Cart* cart() const { return m_cart.get(); }

    /** The printer's ready line (P23): ready (low) by default. */
    void setPrinterReady(bool ready) { m_printerReady = ready; }
    /** Press PLAY once typing (the loader's CLOAD) has finished. */
    void playAfterTyping() { m_playAfterTyping = true; }
    /** The level the sound circuit outputs now. */
    float soundLevel() const;

    Mc6847 vdg;
    coco::Tape tape;
    Keyboard keyboard;
    SerialPort serial;
    Sound sound;
    Display display{&vdg};
    TapeDeck tapeDeck{this};
    ExpansionPort expansionPort{this};

private:
    std::vector<uint8_t> state() const;
    uint64_t now() const;
    void sync(uint64_t to);
    void port2In();
    uint8_t keyboardRows(uint8_t& row6) const;
    void writeLatch(uint8_t v);
    void fetchVideo(uint16_t addr, int n, uint16_t* out);
    uint8_t decodeRead(uint16_t a);
    void decodeWrite(uint16_t a, uint8_t v);

    Model m_model;
    double m_masterHz = MASTER_HZ;
    BusConfig m_cfg;
    std::vector<uint8_t> m_rom;            // 8 KB
    uint8_t m_ram[0x1000] = {};
    std::unique_ptr<Cart> m_cart;
    M6801* m_cpu = nullptr;
    uint8_t m_latch = 0;
    bool m_printerReady = true;
    bool m_playAfterTyping = false;
    uint64_t m_ticks = 0, m_dev = 0;
    float m_level = 0;
};

/** The System behind a machine made by create() (nullptr if not an MC-10). */
System* systemOf(MachineDescriptor* m);

}  // namespace mc10
