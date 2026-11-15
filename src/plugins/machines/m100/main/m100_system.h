#pragma once

#include "machine_m100.h"
#include "plugins/machines/coco/main/coco_tape.h"
#include "plugins/devices/hd44102/main/m100_lcd.h"
#include "plugins/devices/hd61830/main/hd61830.h"
#include "plugins/devices/i8155/main/i8155.h"
#include "plugins/devices/i8251/main/i8251.h"
#include "plugins/devices/im6402/main/im6402.h"
#include "plugins/devices/rp5c01/main/rp5c01.h"
#include "plugins/devices/upd1990a/main/upd1990a.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

class I8085;

namespace m100 {

constexpr double CPU_HZ = 2457600.0;     ///< the 4.9152 MHz crystal / 2

/**
 * The keyboard: nine columns of eight keys, the columns strobed low by the
 * 81C55's port A (bits 0-7: columns 0-7) and port B bit 0 (column 8), the
 * rows read (active low) at $E0-$EF. As MAME's kc85 input ports (the Model
 * 100, 102 and 200 share it):
 *
 *   col 0  Z X C V B N M L          col 5  9 0 - = LEFT RIGHT UP DOWN
 *   col 1  A S D F G H J K          col 6  SPACE BKSP TAB ESC PASTE LABEL PRINT ENTER
 *   col 2  Q W E R T Y U I          col 7  F1-F8
 *   col 3  O P [ ; ' , . /          col 8  SHIFT CTRL GRAPH CODE NUM CAPS - PAUSE
 *   col 4  1 2 3 4 5 6 7 8
 *
 * Host keys by the GUI's names (positional: Shift is a real modifier).
 * Esc ("run_stop") is ESC, Alt GRAPH, right Alt CODE, F9-F12 PASTE, LABEL,
 * PRINT and PAUSE/BREAK, Home NUM; CAPS LOCK latches (it is a locking key).
 * Typed text is held a key at a time, with SHIFT where the Model 100 needs it.
 */
class Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0xE0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    void keyDown(int row, int col) override { set(col, row, true); }
    void keyUp(int row, int col) override { set(col, row, false); }
    void clearKeys() override { reset(); }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    void enqueueText(const std::string& text) override;
    IPortDevice* getPort(int) override { return nullptr; }

    /** A key by name: false if the Model 100 has no such key. */
    bool key(std::string name, bool down);
    /** Key `bit` of column `col` (0-8) up or down. */
    void set(int col, int bit, bool down);
    /** What $E0 reads with the 9-bit column strobe `sel` (a 0 bit selects). */
    uint8_t read(uint16_t sel) const;
    bool typing() const { return m_typing >= 0 || !m_queue.empty(); }
    bool capsLock() const { return m_caps; }
    /** Time passes (typing); `hz` the clock `ticks` count. */
    void clock(uint64_t ticks, double hz);

private:
    void press(int code, bool down);
    uint8_t m_cols[9] = {};
    int m_shifts = 0;                   // SHIFT held by typing
    bool m_caps = false;
    std::deque<int> m_queue;            // typed keys: col << 3 | bit, 0x100 with SHIFT
    int m_typing = -1;
    double m_wait = 0;
};

class System;

/** The LCD, as the frontends find it: 240 x 64 (100/102) or 240 x 128 (200), doubled. */
class Display : public IOHandler, public IVideoOutput {
public:
    explicit Display(System* s) : m_sys(s) {}
    const char* name() const override { return "LCD"; }
    uint32_t baseAddr() const override { return 0xF0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override;
    void renderFrame(uint32_t* buffer) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    static constexpr int SCALE = 2, BORDER = 8;
    static constexpr uint32_t BG = 0xFFA7B39A;        ///< the panel: greenish grey
    static constexpr uint32_t FG = 0xFF1E2420;        ///< a dot

private:
    System* m_sys;
};

/** The piezo speaker (the 81C55's TIMER OUT or port B bit 5). */
class Sound : public IOHandler, public IAudioOutput {
public:
    const char* name() const override { return "Sound"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    int nativeSampleRate() const override { return (int)(CPU_HZ / TICKS_PER_SAMPLE); }
    int pullSamples(float* buffer, int maxSamples) override;
    /** The output at `level` for `ticks` CPU clocks. */
    void run(uint64_t ticks, float level);
    size_t buffered() const { return m_samples.size(); }
    /** Samples taken (tests). */
    const std::deque<float>& samples() const { return m_samples; }

    static constexpr int TICKS_PER_SAMPLE = 56;       // 43886 Hz
private:
    std::deque<float> m_samples;
    double m_sum = 0;
    uint64_t m_acc = 0;
    double m_lastIn = 0, m_dc = 0;      // the DC blocker
};

/**
 * The cassette: the CoCo plugin's recorder (`.wav` only: the Model 100's
 * 1200/2400 Hz FSK is played and recorded as audio). SOD drives the output;
 * SID reads the comparator (1 below the threshold, as MAME's kc85_sid_r).
 */
class Tape : public coco::Tape {
public:
    bool mountTape(const std::string& path) override;
    bool saveTapeRecording(const std::string& path) override;
};

/**
 * The parallel printer port: the data on the 81C55's port A, a byte
 * printed as the strobe ($E0 bit 1; the Model 200's bit 0) goes back low
 * (as VirtualT). Always ready. The paper is a host file (CLI `serial
 * printer:PATH`, `printer:none`) and `printed`.
 */
class Printer : public IOHandler {
public:
    ~Printer() override;
    const char* name() const override { return "Printer"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;
    void write(uint8_t b);
    std::vector<uint8_t> printed;
private:
    FILE* m_file = nullptr;
};

/** The RS-232 port as the frontends see it (CLI `serial SPEC`): the IM6402 or the 8251. */
class SerialPort : public IOHandler {
public:
    explicit SerialPort(System* s) : m_sys(s) {}
    const char* name() const override { return "RS-232"; }
    uint32_t baseAddr() const override { return 0xC0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;
    void getDeviceInfo(DeviceInfo& out) const override;
    const std::string& spec() const { return m_spec; }
private:
    System* m_sys;
    std::string m_spec = "none";
};

/**
 * A Model 100, 102 or 200: the 8085's memory and I/O as each decodes them
 * (Model 100 Technical Reference Manual; VirtualT io.c / memory.c; MAME
 * kyocera.cpp), and the wiring between the chips.
 *
 * Model 100/102 memory: $0000-$7FFF the 32 KB ROM, or the option ROM
 * socket when port $E0 bit 0 is set (an empty socket reads $FF); RAM ends
 * at $FFFF and goes down 8 KB a module (8-32 KB; nothing below it).
 * I/O (each block mirrored over its 16 ports):
 *  - $A0 (Model 100) modem control (stored, not emulated);
 *  - $B0-$BF the 81C55 (address & 7). Port A: the LCD chip selects 0-7,
 *    the keyboard columns 0-7 (active low), the uPD1990AC's C0-C2 (bits
 *    0-2), CLK (3) and DATA IN (4), the printer data. Port B: LCD selects
 *    8-9 (bits 0-1; bit 0 also keyboard column 8), 2 /MC (1: the speaker
 *    from bit 5, 0: from TIMER OUT), 3 RS-232 (0) or modem (1), 4 the
 *    auto power-off, 5 BELL, 6 /DTR, 7 /RTS. Port C (input): 0 the RTC's
 *    DATA OUT, 1 the printer's /BUSY, 2 BUSY, 3 the bar code reader, 4
 *    /CTS, 5 /DSR. TIMER OUT is the speaker tone and the IM6402's 16x clock;
 *  - $C0-$CF the IM6402's data (a read also clears DR);
 *  - $D0-$DF write: the IM6402's control (bits 0-4); read: status (0 CD,
 *    1 OE, 2 FE, 3 PE, 4 TBRE, 5 RP, 6 +5 V, 7 /LPS);
 *  - $E0-$EF read: the keyboard rows; write: 0 the option ROM, 1 the
 *    printer /STROBE, 2 the RTC's STB, 3 the cassette motor;
 *  - $F0-$FF the LCD: A0 low the instruction / status, high the data,
 *    to every selected HD44102.
 * Interrupts: RST 7.5 the RTC's TP (the ROM sets 256 Hz: the keyboard scan
 * and clock tick), RST 6.5 the UART's DR, RST 5.5 the bar code reader (not
 * fitted), TRAP the power switch going off.
 *
 * Model 200 memory: $0000-$7FFF a 32 KB bank (port $D0 bits 0-1: 0 the
 * main ROM, 1 the second ROM (MULTIPLAN), 2 the option ROM, 3 nothing),
 * $8000-$9FFF the main ROM's last 8 KB, $A000-$FFFF a 24 KB RAM bank (bits
 * 2-3: banks 0-2; one beyond the RAM fitted reads $FF). I/O: $90-$9F the
 * RP5C01, $A0 the tone dialler (ignored), $B0-$BF the 81C55 (port A the
 * keyboard columns and printer data; port B 0 column 8, 2 /BUZZER, 4 power
 * cut, 5 BELL; port C 0 /LPS, 1-2 printer, 3 bar code, 4 CD), $C0/$C1 the
 * 8251 (data, control/status), $D0 the bank register (read back), $E0
 * keyboard / 0 printer strobe, 1 cassette motor, $F0/$F1 the HD61830
 * (data, instruction/status). RST 7.5 is a 4.9152 MHz / 2 / 8192 square
 * wave (150 Hz rising edges, as MAME), RST 6.5 the 8251's RxRDY.
 *
 * Time is in CPU clocks (2.4576 MHz).
 */
class System : public IBus {
public:
    System(Model model, std::vector<uint8_t> rom);
    ~System() override;

    // IBus: the 8085's memory
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

    /** The 8085's I/O ports. */
    uint8_t in(uint8_t port);
    void out(uint8_t port, uint8_t v);
    uint8_t peekPort(uint8_t port);
    IBus* ioBus() { return m_io; }

    /** Power on with RAM cleared. */
    void powerOn();
    /** The reset button: the 8085 restarts, RAM kept. */
    void resetButton();
    /** The power switch: off asserts TRAP (the ROM saves its state and
     *  powers down; after 50 ms it goes off anyway); on restarts, RAM kept. */
    void powerSwitch();
    bool powered() const { return m_powered; }
    /** One instruction; returns CPU clocks elapsed. */
    int step();
    void setCpu(I8085* cpu);
    I8085* cpu() const { return m_cpu; }
    uint64_t ticks() const { return m_ticks; }

    Model model() const { return m_model; }
    bool isT200() const { return m_model == Model::M200; }
    /** RAM in KB (8-32; the 200: 24, 48, 72). Clears it. */
    void setRamKb(int kb);
    int ramKb() const { return (int)(m_ram.size() / 1024); }
    /** The option ROM (empty: none); up to 32 KB. */
    void setOptionRom(std::vector<uint8_t> rom, const std::string& label);
    const std::string& optionRomLabel() const { return m_optLabel; }
    bool hasOptionRom() const { return !m_opt.empty(); }
    /** The $0000-$7FFF bank: 100/102 0 main, 1 option; 200 port $D0 bits 0-1. */
    int romBank() const;
    int ramBank() const { return (m_bankReg >> 2) & 3; }
    uint8_t bankRegister() const { return m_bankReg; }
    uint8_t controlPort() const { return m_ctrl; }

    /** The panel's dot (x, y); 240 x 64 or 240 x 128. */
    bool pixel(int x, int y) const;
    int lcdWidth() const { return 240; }
    int lcdHeight() const { return isT200() ? 128 : 64; }

    /** The 9-bit keyboard strobe (bit 8 from port B bit 0). */
    uint16_t keyStrobe() const { return m_keyLatch; }
    /** The speaker's level now. */
    bool speaker() const;
    bool rst75() const { return m_rst75; }

    I8155 pio;
    M100Lcd lcd;
    Hd61830 lcdc{8192};
    Im6402 uart;
    I8251 usart;
    Upd1990a rtc;
    Rp5c01 rtc200;

    Keyboard keyboard;
    Display display{this};
    Sound sound;
    Tape tape;
    Printer printer;
    SerialPort serial{this};

private:
    std::vector<uint8_t> state() const;
    void advance(uint64_t ticks);
    void hardwareReset();
    void powerDown();
    void portAOut(uint8_t v);
    void portBOut(uint8_t v);
    uint8_t portCIn();
    void controlOut(uint8_t v);
    uint8_t uartStatus() const;
    uint8_t access(uint16_t a, bool write, uint8_t v);
    void setRst75(bool level);
    void setRst65(bool level);

    Model m_model;
    BusConfig m_cfg;
    IBus* m_io = nullptr;
    I8085* m_cpu = nullptr;

    std::vector<uint8_t> m_rom;          // 32 KB (100/102); 72 KB (200: main 40 KB, second 32 KB)
    std::vector<uint8_t> m_opt;          // the option ROM, padded to 32 KB
    std::string m_optLabel;
    std::vector<uint8_t> m_ram;

    uint8_t m_pa = 0xFF, m_pb = 0xFF;    // the 81C55's port pins as last driven
    uint16_t m_keyLatch = 0x1FF;
    uint8_t m_ctrl = 0;                  // the last $E0 write
    uint8_t m_bankReg = 0;               // 200: $D0
    uint8_t m_modem = 0;                 // 100: $A0
    bool m_rst75 = false, m_rst65 = false;
    uint64_t m_tpAcc = 0;                // 200: the RST 7.5 divider
    uint64_t m_frameAcc = 0;             // 200: the HD61830's frames (blink)
    bool m_powered = true;
    bool m_switchOff = false;            // the switch is off (TRAP held)
    uint64_t m_holdup = 0;               // clocks until the power goes after the switch
    uint64_t m_ticks = 0;
};

/** The System behind a machine made by create() (nullptr if not a Model 100/102/200). */
System* systemOf(MachineDescriptor* m);

}  // namespace m100
