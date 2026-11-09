// Apple IIgs (#12), after KEGS by Kent Dickey (GPL-3.0; the GSplus tree,
// upstream/kegs/src): moremem.c (the bank map, shadowing, the $C0xx soft
// switches, the language card), adb.c (the ADB microcontroller's command
// interface and the keyboard), clock.c (the clock chip and battery RAM),
// doc.c (the Ensoniq DOC's registers), sim65816.c (the VBL, quarter-second
// and one-second interrupts, Control-Reset).
//
// Stage 1: ROM 01, the 65C816 at 2.8 MHz with 1 MHz slow-side accesses,
// banks $00/$01 shadowed into $E0/$E1 (the Mega II), the Apple II video
// modes through Apple2Video, ADB in high-level emulation, the clock, the
// DOC's registers and RAM (no sound yet).
//
// Stage 2: Super Hi-Res (video.c), 320 and 640 pixels, fill mode, the
// scan-line interrupt (sim65816.c do_scan_int).
//
// Drives: the 5.25" drives (slot 6) through Disk2Card, the 3.5" drives
// (slot 5) through Gs35Drive (gs_drive35.cpp).
//
// Not yet: DOC sound, the mouse, the serial ports (the SCC
// answers as idle).

#include "machine_apple2.h"
#include "apple2_parts.h"
#include "disk2.h"
#include "gs_drive35.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/65816/main/cpu65816.h"
#include "plugins/devices/apple2_video/main/apple2_video.h"
#include <cctype>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace apple2 {

namespace {

// 14.318 MHz ticks: a fast cycle is 5 (2.86 MHz), a slow one 14 (1.02 MHz)
constexpr int FAST_TICKS = 5, SLOW_TICKS = 14;
constexpr int FAST_BANKS = 0x40;       // 4 MB of fast RAM: banks $00-$3F

// ---------------------------------------------------------------------------
// ADB (adb.c): the microcontroller's command/data port at $C026, its status
// at $C027, the keyboard at $C000/$C010 and the modifier keys at $C025. The
// keyboard is autopolled: keys go straight to the $C000 latch.
// ---------------------------------------------------------------------------
class GsAdb : public IOHandler, public IKeyboardMatrix {
public:
    std::function<void(bool cmd, bool ctrl)> onReset;     // Control-Reset (Command: cold start)

    const char* name() const override { return "ADB"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }

    // adb_reset
    void reset() override {
        m_c027 = 0;
        m_keyDown = m_hardKeyDown = false;
        m_chars = 0;
        m_kbdDev = 2; m_mouseDev = 3; m_kbdCtl = 2; m_mouseCtl = 3;
        m_dataIrq = false;
        m_pending = 0;
        m_intByte = 0;
        m_state = IDLE;
        m_kbdReg3 = 0x602;
    }
    void powerOn() { reset(); m_c025 = 0; m_mode = 0; std::memset(m_ram, 0, sizeof m_ram); m_queue.clear(); }

    bool irq() const { return m_dataIrq; }
    void vbl() { ++m_vbl; }

    // $C000 (adb_read_c000): the key, with the auto-repeat
    uint8_t readC000() {
        if (!(m_buf[0] & 0x80) && !m_keyDown) return (uint8_t)m_buf[0];
        if (m_buf[0] & 0x80) {
            if (m_readNoUpdate++ > 5 && m_chars > 1) accessC010();   // read 5 times, keys waiting
        } else if (m_keyDown && m_vbl >= m_repeatVbl) {
            m_c025 |= 0x08;
            push(m_downChar);
            m_repeatVbl = m_vbl + m_repeatRate;
        }
        return (uint8_t)m_buf[0];
    }
    // $C010 (adb_access_c010): clear the strobe; any key down in bit 7
    uint8_t accessC010() {
        m_readNoUpdate = 0;
        int tmp = m_buf[0] & 0x7F;
        m_buf[0] = tmp;
        tmp |= m_hardKeyDown ? 0x80 : 0;
        if (m_chars) {
            for (int i = 1; i < m_chars; ++i) m_buf[i - 1] = m_buf[i];
            --m_chars;
        }
        m_c025 &= ~0x08;
        return (uint8_t)tmp;
    }
    uint8_t c025() const { return (uint8_t)m_c025; }
    bool cmdDown() const { return m_c025 & 0x80; }
    bool optionDown() const { return m_c025 & 0x40; }

    // $C026 read (adb_read_c026)
    uint8_t readC026() {
        uint32_t ret = 0;
        switch (m_state) {
            case IDLE:
                ret = m_intByte;
                m_intByte = 0;
                if (m_pending == 0) m_dataIrq = false;
                else m_state = SENDING;
                break;
            case IN_CMD: ret = 0; break;
            case SENDING:
                ret = m_data[0];
                for (int i = 1; i < m_pending; ++i) m_data[i - 1] = m_data[i];
                if (--m_pending <= 0) { m_pending = 0; m_state = IDLE; m_dataIrq = false; }
                break;
        }
        return (uint8_t)ret;
    }
    // $C026 write (adb_write_c026)
    void writeC026(uint8_t val) {
        if (m_state == IN_CMD) {
            m_cmdData[m_cmdSoFar++] = val;
            if (m_cmdSoFar >= m_cmdLen) doCmd();
            return;
        }
        if (m_state == SENDING) { m_state = IDLE; return; }
        m_cmd = val;
        m_cmdSoFar = m_cmdLen = 0;
        const int dev = val & 0x0F;
        auto inCmd = [&](int len) { m_state = IN_CMD; m_cmdLen = len; };
        switch (val) {
            case 0x01: case 0x03: case 0x73: break;      // abort, flush, disable mouse SRQ
            case 0x04: case 0x05: inCmd(1); break;      // set / clear modes
            case 0x06: inCmd(3); break;                 // set config
            case 0x07: inCmd(4); break;                 // sync (ROM 01: 4 bytes)
            case 0x08: case 0x09: inCmd(2); break;      // write / read memory
            case 0x0A: send(1, m_mode << 24); break;    // read modes
            case 0x0B:                                  // read config
                send(4, (0x82u << 24) | (uint32_t)m_mouseCtl << 20 | (uint32_t)m_kbdCtl << 16 | m_repeatInfo);
                break;
            case 0x0D: send(1, 5u << 24); break;        // version: ROM 01 = 5
            case 0x0E: send(2, 0x08000000); break;      // character sets
            case 0x0F: send(2, 0x0A000000); break;      // keyboard layouts
            case 0x10: break;                           // reset the system: not taken
            case 0x11: inCmd(1); break;                 // send ADB keycodes
            default:
                if ((val & 0xF0) == 0xB0) inCmd(2);     // listen, register 3
                else if ((val & 0xF0) == 0xC0) {        // talk, register 0
                    if (dev == m_kbdDev) response(0, 0);    // autopolled: nothing queued
                } else if ((val & 0xF0) == 0xF0) {      // talk, register 3
                    if (dev == m_kbdDev) response(2, m_kbdReg3);
                }
                break;
        }
    }
    // $C027 (adb_read_c027 / adb_write_c027)
    uint8_t readC027() const {
        uint32_t r = m_c027 & NEG_MASK;
        if (m_intByte != 0 || (m_pending > 0 && m_state != IN_CMD)) r |= 0x20;   // data valid
        return (uint8_t)r;
    }
    void writeC027(uint8_t val) {
        m_c027 = val & NEG_MASK;
        if (!(m_c027 & 0x10)) m_dataIrq = false;
    }

    // Keys by name (the frontends' US layout)
    bool key(std::string n, bool down) {
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        auto mod = [&](uint32_t bit) { if (down) m_c025 |= bit; else m_c025 &= ~bit; latchMods(); return true; };
        if (n == "left_shift" || n == "right_shift" || n == "shift") return mod(0x01);
        if (n == "control" || n == "ctrl") return mod(0x02);
        if (n == "alt" || n == "left_alt" || n == "open_apple" || n == "command") return mod(0x80);
        if (n == "right_alt" || n == "altgr" || n == "option" || n == "closed_apple") return mod(0x40);
        if (n == "caps_lock") { if (down) m_c025 ^= 0x04; latchMods(); return true; }
        if (n == "reset") { if (down && onReset) onReset(cmdDown(), m_c025 & 0x02); return true; }
        static const std::map<std::string, uint8_t> special = {
            {"return", 0x0D}, {"enter", 0x0D}, {"left", 0x08}, {"backspace", 0x7F}, {"delete", 0x7F},
            {"right", 0x15}, {"up", 0x0B}, {"down", 0x0A}, {"run_stop", 0x1B}, {"escape", 0x1B}, {"esc", 0x1B},
            {"tab", 0x09}, {"space", 0x20}};
        static const std::map<std::string, char> punct = {
            {"semicolon", ';'}, {"kpeq", '='}, {"equal", '='}, {"equals", '='}, {"minus", '-'}, {"comma", ','},
            {"period", '.'}, {"slash", '/'}, {"at", '@'}, {"bracket_left", '['}, {"bracket_right", ']'},
            {"squote", '\''}, {"backslash", '\\'}, {"grave", '`'}, {"plus", '+'}, {"asterisk", '*'}};
        int c = -1;
        if (auto it = special.find(n); it != special.end()) c = it->second;
        else if (auto p = punct.find(n); p != punct.end()) c = shifted(p->second);
        else if (n.size() == 1 && std::isalnum((unsigned char)n[0])) c = shifted(n[0]);
        if (c < 0) return false;
        if (!down) {
            if (n == m_downName) { m_hardKeyDown = false; m_keyDown = false; }
            latchMods();
            return true;
        }
        if (c == 0x1B && (m_c025 & 0x82) == 0x82) {     // Control-Command-Escape: the Desk Manager
            m_intByte |= 0x20;
            addDataIrq();
        }
        if ((m_c025 & 0x02) && std::isalpha(c)) c &= 0x1F;
        else if ((m_c025 & 0x02) && (c == '@' || c == '2')) c = 0;
        keyPress((uint8_t)c, n);
        return true;
    }

    // IKeyboardMatrix: names and typed text
    void keyDown(int, int) override {}
    void keyUp(int, int) override {}
    void clearKeys() override { m_queue.clear(); m_c025 &= ~0xC3u; }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    IPortDevice* getPort(int) override { return nullptr; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\r'; ++i; }
            if (c == '\n') c = '\r';
            m_queue.push_back((uint8_t)c & 0x7F);
        }
    }
    // Typed text: the next character once the program took the last one
    void tick(uint64_t cycles) override {
        if (m_queue.empty() || m_chars || (m_buf[0] & 0x80)) return;
        if (m_wait > cycles) { m_wait -= cycles; return; }
        push(m_queue.front());
        m_queue.pop_front();
        m_wait = 2000;
    }

private:
    enum State { IDLE, IN_CMD, SENDING };
    static constexpr uint32_t NEG_MASK = ~(0x80u | 0x20u | 0x08u | 0x02u | 0x01u);

    int shifted(char c) const {
        static const char base[] = "1234567890-=;',./[]\\`";
        static const char up[] = "!@#$%^&*()_+:\"<>?{}|~";
        const bool shift = m_c025 & 0x01;
        if (shift)
            if (const char* p = std::strchr(base, c)) c = up[p - base];
        if (std::isalpha((unsigned char)c))
            return (shift || (m_c025 & 0x04)) ? std::toupper((unsigned char)c) : std::tolower((unsigned char)c);
        return c;
    }
    void latchMods() { if (m_keyDown) m_c025 &= ~0x20u; else m_c025 |= 0x20u; }
    // adb_key_event for a key with a character
    void keyPress(uint8_t c, const std::string& n) {
        push(c);
        m_keyDown = true;
        m_hardKeyDown = true;
        m_downChar = c;
        m_downName = n;
        m_repeatVbl = m_vbl + m_repeatDelay;
        if (m_repeatDelay == 0) m_keyDown = false;
        latchMods();
    }
    void push(uint8_t c) {
        m_buf[m_chars] = (c & 0x7F) | 0x80;
        if (++m_chars >= 8) m_chars = 7;
    }
    void addDataIrq() { if (m_c027 & 0x10) m_dataIrq = true; }
    // adb_send_bytes: bytes from the top of val0
    void send(int n, uint32_t val0) {
        m_state = SENDING;
        m_pending = n;
        addDataIrq();
        for (int i = 0; i < n && i < 4; ++i) m_data[i] = (val0 >> (8 * (3 - i))) & 0xFF;
    }
    // adb_response_packet: a device's answer, low byte first
    void response(int n, uint32_t val) {
        m_state = IDLE;
        m_pending = n;
        for (int i = 0; i < 4; ++i) m_data[i] = (val >> (8 * i)) & 0xFF;
        m_intByte |= n ? 0x80u + (uint32_t)n - 1 : 0x80u;
        addDataIrq();
    }
    // adb_set_config
    void setConfig(uint32_t v0, uint32_t, uint32_t v2) {
        m_mouseCtl = (int)(v0 >> 4);
        m_kbdCtl = (int)(v0 & 0xF);
        const uint32_t d = v2 >> 4;
        if (d == 4) m_repeatDelay = 0;
        else if (d < 4) m_repeatDelay = (d + 1) * 15;
        static const uint32_t RATE[] = {1, 2, 3, 3, 4, 5, 7, 15, 30, 60};
        if ((v2 & 0xF) < 10) m_repeatRate = RATE[v2 & 0xF];
    }
    uint8_t readRam(uint32_t a) const {
        if (a >= 0x100) {
            if (a == 0x1400) return 0x72;               // the ROM's self-test checksum
            if (a == 0x1401) return 0xF7;
            return 0;
        }
        if (a == 0x0B) {                                // ROM 01: the special keys' state
            uint32_t v = (m_c025 >> 1) & 0x43;
            v |= (m_c025 << 2) & 0x4;
            v |= (m_c025 >> 2) & 0x10;
            return (uint8_t)v;
        }
        return m_ram[a];
    }
    // do_adb_cmd
    void doCmd() {
        m_state = IDLE;
        const int dev = (int)(m_cmd & 0xF);
        switch (m_cmd) {
            case 0x04: m_mode |= m_cmdData[0]; break;
            case 0x05: m_mode &= ~m_cmdData[0]; break;
            case 0x06: setConfig(m_cmdData[0], m_cmdData[1], m_cmdData[2]); break;
            case 0x07: m_mode = m_cmdData[0]; setConfig(m_cmdData[1], m_cmdData[2], m_cmdData[3]); break;
            case 0x08: if (m_cmdData[0] < 0x100) m_ram[m_cmdData[0]] = (uint8_t)m_cmdData[1]; break;
            case 0x09: send(1, (uint32_t)readRam(m_cmdData[1] << 8 | m_cmdData[0]) << 24); break;
            case 0x11: break;                           // keycodes from the program: ignored
            default:
                if ((m_cmd & 0xF0) == 0xB0 && dev == m_kbdDev) {    // listen keyboard register 3
                    if (m_cmdData[1] == 0xFE) m_kbdDev = (int)(m_cmdData[0] & 0xF);
                    m_kbdReg3 = ((m_cmdData[0] & 0xF) << 12) | (m_kbdReg3 & 0x0FFF);
                }
                break;
        }
    }

    uint32_t m_c025 = 0, m_c027 = 0, m_mode = 0, m_intByte = 0;
    State m_state = IDLE;
    uint32_t m_cmd = 0, m_cmdData[16] = {};
    int m_cmdLen = 0, m_cmdSoFar = 0;
    uint32_t m_data[16] = {};
    int m_pending = 0;
    bool m_dataIrq = false;
    int m_kbdDev = 2, m_mouseDev = 3, m_kbdCtl = 2, m_mouseCtl = 3;
    uint32_t m_kbdReg3 = 0x602, m_repeatInfo = 0x23;
    uint32_t m_repeatDelay = 45, m_repeatRate = 3;
    uint8_t m_ram[256] = {};
    int m_buf[8] = {};
    int m_chars = 0, m_readNoUpdate = 0;
    bool m_keyDown = false, m_hardKeyDown = false;
    uint8_t m_downChar = 0;
    std::string m_downName;
    uint64_t m_vbl = 0, m_repeatVbl = 0;
    std::deque<uint8_t> m_queue;
    uint64_t m_wait = 0;
};

// ---------------------------------------------------------------------------
// The clock chip (clock.c): commands through $C033 (data) and $C034
// (control: bit 7 start, bit 6 read), the seconds since 1904 and 256 bytes
// of battery RAM
// ---------------------------------------------------------------------------
class GsClock {
public:
    uint8_t data() const { return m_data; }
    void setData(uint8_t v) { m_data = v; }
    uint8_t ctl() const { return m_ctl; }
    void setCtl(uint8_t v) {
        m_ctl = v & 0x7F;
        if (v & 0x80) transfer();
    }
    uint8_t* bram() { return m_bram; }

private:
    enum Mode { IDLE, TIME, INTERNAL, BRAM1, BRAM2 };
    // Seconds since 1 January 1904, local time
    static uint32_t hostTime() {
        const time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        const int64_t local = (int64_t)now + lt.tm_gmtoff;
        return (uint32_t)(local + ((66 * 365) + 17) * 86400LL);
    }
    uint32_t now() const { return hostTime() + m_offset; }
    void transfer() {
        const bool read = m_ctl & 0x40;
        switch (m_mode) {
            case IDLE: {
                if (read) break;
                m_read = (m_data >> 7) & 1;
                m_reg = (m_data >> 2) & 3;
                const int op = (m_data >> 4) & 7;
                switch (op) {
                    case 0: m_mode = TIME; break;
                    case 3:
                        m_mode = INTERNAL;
                        if (m_reg & 2) { m_mode = BRAM2; m_reg = (m_data & 7) << 5; }
                        break;
                    case 2: m_mode = BRAM1; m_reg += 0x10; break;
                    default: m_mode = BRAM1; m_reg = (m_data >> 2) & 0xF; break;
                }
                return;
            }
            case BRAM2:
                if (!read && (m_data & 0x83) == 0) { m_reg |= (m_data >> 2) & 0x1F; m_mode = BRAM1; return; }
                break;
            case BRAM1:
                if (read && m_read) m_data = m_bram[m_reg & 0xFF];
                else if (!read && !m_read) m_bram[m_reg & 0xFF] = m_data;
                break;
            case TIME:
                if (read) m_data = (uint8_t)(now() >> (m_reg * 8));
                else {
                    const uint32_t mask = 0xFFu << (8 * m_reg);
                    const uint32_t t = (now() & ~mask) | ((uint32_t)m_data << (8 * m_reg));
                    m_offset = t - hostTime();
                }
                break;
            case INTERNAL: break;                       // the test and write-protect registers
        }
        m_mode = IDLE;
    }

    uint8_t m_data = 0, m_ctl = 0;
    Mode m_mode = IDLE;
    int m_read = 0, m_reg = 0;
    uint32_t m_offset = 0;
    uint8_t m_bram[256] = {};
};

// ---------------------------------------------------------------------------
// The Ensoniq DOC (doc.c): the sound GLU's control ($C03C), data ($C03D,
// reads one access behind) and address ($C03E/F) registers in front of the
// DOC's registers and its 64 KB of sound RAM. Stage 1: the registers and the
// RAM, no oscillators running.
// ---------------------------------------------------------------------------
class GsDoc {
public:
    GsDoc() { m_ram.assign(0x10000, 0); }
    void reset() { m_ctl = 0; m_e0 = 0xFF; }
    uint8_t readCtl() const { return m_ctl; }
    void writeCtl(uint8_t v) { m_ctl = v; }
    uint8_t readPtr(int hi) const { return (uint8_t)(hi ? m_ptr >> 8 : m_ptr); }
    void writePtr(int hi, uint8_t v) { m_ptr = hi ? (uint16_t)((m_ptr & 0xFF) | v << 8) : (uint16_t)((m_ptr & 0xFF00) | v); }
    uint8_t readData() {
        const uint8_t ret = m_saved;
        if (m_ctl & 0x40) m_saved = m_ram[m_ptr];
        else {
            const int osc = m_ptr & 0x1F, type = (m_ptr >> 5) & 7;
            switch (type) {
                case 7:
                    m_saved = osc == 0 ? m_e0 : osc == 1 ? (uint8_t)((m_numOsc - 1) << 1) : osc == 2 ? 0x80 : 0;
                    break;
                default: m_saved = m_reg[type][osc]; break;
            }
        }
        if (m_ctl & 0x20) ++m_ptr;
        return ret;
    }
    void writeData(uint8_t v) {
        if (m_ctl & 0x40) m_ram[m_ptr] = v;
        else {
            const int osc = m_ptr & 0x1F, type = (m_ptr >> 5) & 7;
            if (type == 7) {
                if (osc == 1) m_numOsc = std::clamp(((v & 0x3E) >> 1) + 1, 1, 32);
            } else if (type != 3) {
                m_reg[type][osc] = v;
            }
        }
        if (m_ctl & 0x20) ++m_ptr;
        m_saved = v;
    }

private:
    std::vector<uint8_t> m_ram;
    uint8_t m_reg[7][32] = {};          // frequency low/high, volume, data, wave pointer, control, wave size
    uint8_t m_ctl = 0, m_saved = 0, m_e0 = 0xFF;
    uint16_t m_ptr = 0;
    int m_numOsc = 1;
};

// The SCC (Z8530) at $C038-$C03B, idle: register pointers, the transmitter
// empty, nothing received
class GsScc {
public:
    void reset() { m_ptr[0] = m_ptr[1] = 0; m_mode[0] = m_mode[1] = 0; }
    uint8_t readReg(int port) {
        const int r = m_ptr[port];
        m_ptr[port] = 0;
        m_mode[port] = 0;
        return r == 0 ? 0x44 : 0;       // RR0: Tx buffer empty, Tx underrun
    }
    void writeReg(int port, uint8_t v) {
        if (m_mode[port] == 0 && (v & 0x0F) != 0) { m_ptr[port] = v & 0x0F; m_mode[port] = 1; return; }
        m_ptr[port] = 0;
        m_mode[port] = 0;
    }

private:
    int m_ptr[2] = {}, m_mode[2] = {};
};

// ---------------------------------------------------------------------------
// The bus (moremem.c): fast RAM in banks $00-$3F, the Mega II's slow RAM in
// $E0/$E1, the ROM in $FE/$FF; banks $00/$01 (and $E0/$E1) take the Apple
// II's paging (STATEREG $C068: ALTZP, PAGE2, RAMRD, RAMWRT, RDROM, LCBANK2,
// ROMBANK, INTCXROM) and the I/O space, and writes to the video pages of
// banks $00/$01 are shadowed into $E0/$E1 ($C035)
// ---------------------------------------------------------------------------
class GsBus : public IBus {
public:
    GsBus(std::vector<uint8_t> rom, Apple2Video* video, GsAdb* adb, Apple2Speaker* spk)
        : m_rom(std::move(rom)), m_video(video), m_adb(adb), m_spk(spk) {
        m_cfg = {24, 8, BusRole::DATA, true, 0xFFFFFF};
        m_fast.assign((size_t)FAST_BANKS << 16, 0);
        m_slow.assign(0x20000, 0);
        m_romBase = 0x1000000u - (uint32_t)m_rom.size();
        video->setMemory(m_slow.data(), m_slow.data() + 0x10000);
    }
    void setCpu(const W65C816* cpu) { m_cpu = cpu; }
    std::function<void(bool)> onIrq;

    void powerOn() {
        std::fill(m_fast.begin(), m_fast.end(), 0);
        std::fill(m_slow.begin(), m_slow.end(), 0);
        m_ticks = m_videoCycle = 0;
        m_c036 = 0x80;                          // fast
        m_c029 = 0x01;
        m_c02b = 0x08;
        m_adb->powerOn();
        m_doc.reset();
        ctrlReset();
    }

    // Control-Reset (sim65816.c do_reset)
    void ctrlReset() {
        m_shadow = 0;
        m_state = WRDEFRAM | RDROM | LCBANK2 | INTCX;
        m_c02d = 0;
        m_c023 = 0;
        m_c041 = 0;
        m_c046 = 0;
        m_irqVbl = m_irq25 = m_irq1s = m_irqScan = false;
        // video_reset: text, AN3 set (double hi-res off), white text
        m_vf = Apple2Video::VF_TEXT;
        m_altChar = false;
        m_video->setAltCharSet(false);
        m_video->setMode(m_vf);
        m_tbColor = 0xF0;
        m_adb->reset();
        m_doc.reset();
        m_scc.reset();
        m_iwmMotor = m_iwmDrive2 = m_iwmQ6 = m_iwmQ7 = false;
        m_iwmPhases = 0;
        m_iwmMode = 0;
        m_clock.setCtl(m_clock.ctl() & 0x0F);
    }

    // Instruction framing: the CPU's idle cycles are counted at the end
    void begin() { m_accesses = 0; ++m_instr; }
    void end(int cycles) {
        const int idle = cycles - m_accesses;
        if (idle > 0) m_ticks += (uint64_t)idle * (fast() ? FAST_TICKS : SLOW_TICKS);
        syncVideo();
        m_spk->advance(slowCycles());
        // VBL (sim65816.c update_60hz / do_vbl_int)
        const bool vbl = !m_video->vblBar();
        if (vbl && !m_lastVbl) onVbl();
        m_lastVbl = vbl;
        if (onIrq) onIrq(irqLine());
    }
    /** 1.02 MHz cycles elapsed (the Mega II's clock: video, speaker, devices). */
    uint64_t slowCycles() const { return m_ticks / SLOW_TICKS; }
    // CYAREG bit 7: fast; bits 0-3 slow it down while a slot 4-7 drive's
    // motor runs (bit 2: slot 6, the 5.25" drives)
    bool fast() const { return (m_c036 & 0x80) && !((m_c036 & 0x04) && m_disk && m_disk->motorOn() && !(m_c031 & 0x40)); }
    void setDisk(Disk2Card* d) { m_disk = d; }
    void setDrives35(Gs35Drives* d) { m_drives35 = d; }
    double speedMHz() const { return fast() ? 2.8 : 1.0; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        addr &= 0xFFFFFF;
        ++m_accesses;
        const uint32_t bank = addr >> 16;
        const uint16_t a = (uint16_t)addr;
        uint8_t v;
        if (bank < 2 || bank == 0xE0 || bank == 0xE1) v = megaRead(bank < 2 ? (int)bank : 2 + (int)(bank & 1), a);
        else {
            tick(false);
            v = bank < FAST_BANKS ? m_fast[addr] : addr >= m_romBase ? m_rom[addr - m_romBase] : 0;
        }
        if (m_observer) m_observer->onMemoryRead(this, addr, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override {
        addr &= 0xFFFFFF;
        const uint32_t bank = addr >> 16;
        const uint16_t a = (uint16_t)addr;
        if (bank < 2 || bank == 0xE0 || bank == 0xE1) {
            const int k = bank < 2 ? (int)bank : 2 + (int)(bank & 1);
            if (isIo(k, a)) {
                if (a >= 0xC100) return cxRom(a, false);
                return 0;
            }
            return memRead(k, a);
        }
        return bank < FAST_BANKS ? m_fast[addr] : addr >= m_romBase ? m_rom[addr - m_romBase] : 0;
    }
    void write8(uint32_t addr, uint8_t v) override {
        addr &= 0xFFFFFF;
        ++m_accesses;
        if (m_observer) m_observer->onMemoryWrite(this, addr, peek8(addr), v);
        const uint32_t bank = addr >> 16;
        const uint16_t a = (uint16_t)addr;
        if (bank < 2 || bank == 0xE0 || bank == 0xE1) { megaWrite(bank < 2 ? (int)bank : 2 + (int)(bank & 1), a, v); return; }
        if (bank < FAST_BANKS) {
            m_fast[addr] = v;
            // CYAREG bit 4: shadowing in all banks (KEGS: Super Hi-Res, odd banks into $E1)
            if ((m_c036 & 0x10) && !(m_shadow & 0x08) && (bank & 1) && bank >= 3 && a >= 0x2000 && a < 0xA000) {
                m_slow[0x10000 | a] = v;
                tick(true);
                return;
            }
        }
        tick(false);
    }
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

    uint8_t* slowRam() { return m_slow.data(); }
    uint8_t* fastRam() { return m_fast.data(); }
    uint8_t border() const { return m_clock.ctl() & 0x0F; }
    uint8_t tbColor() const { return m_tbColor; }
    uint32_t videoFlags() const { return m_vf; }
    uint8_t newVideo() const { return m_c029; }
    /** The last complete Super Hi-Res frame (640 x 200), and which lines were Super Hi-Res. */
    const std::vector<uint32_t>& shrFrame() const { return m_shrFront; }
    bool shrLine(int line) const { return m_shrOnFront[line]; }
    static constexpr int SHR_WIDTH = 640, SHR_LINES = 200;
    static constexpr uint32_t VIS_START = Apple2Video::HORZ_START;
    uint8_t stateReg() const { return (uint8_t)m_state; }
    uint8_t shadowReg() const { return (uint8_t)m_shadow; }
    GsClock& clock() { return m_clock; }

private:
    // STATEREG and KEGS's internal bits above it
    enum : uint32_t {
        INTCX = 0x01, ROMBANK = 0x02, LCBANK2 = 0x04, RDROM = 0x08, RAMWRT = 0x10, RAMRD = 0x20,
        PAGE2 = 0x40, ALTZP = 0x80, PREWRITE = 0x100, WRDEFRAM = 0x200, INTC8ROM = 0x400
    };

    void tick(bool slow) { m_ticks += (slow || !fast()) ? SLOW_TICKS : FAST_TICKS; }

    // The video catches up with the CPU; on the way each Super Hi-Res line
    // is drawn as the beam reaches its visible part (cycle 25 of the line),
    // and a line's scan-line interrupt is raised as its line starts
    void syncVideo() {
        const uint64_t to = slowCycles();
        while (m_videoCycle < to) {
            const uint32_t h = m_video->horz();
            uint64_t step = h < VIS_START ? VIS_START - h : Apple2Video::CYCLES_PER_LINE - h;
            if (step > to - m_videoCycle) step = to - m_videoCycle;
            m_video->clock((uint32_t)step);
            m_videoCycle += step;
            const int line = m_video->vert();
            if (m_video->horz() == 0) lineStart(line);
            else if (m_video->horz() == VIS_START && line < SHR_LINES) drawShrLine(line);
        }
    }

    // do_scan_int: the first line from here whose SCB has bit 6, while
    // $C023 bit 5 is clear
    void lineStart(int line) {
        if (line == SHR_LINES) { m_shrFront = m_shrBack; m_shrOnFront = m_shrOn; }
        if (line >= SHR_LINES || !(m_c029 & 0x80) || (m_c023 & 0x20)) return;
        if (!(m_slow[0x19D00 + line] & 0x40)) return;
        m_c023 |= 0xA0;
        if (m_c023 & 0x02) m_irqScan = true;
    }

    // Super Hi-Res ($C029 bit 7), bank $E1: pixels at $2000 (160 bytes a
    // line), the scan-line control bytes at $9D00 (bit 7 640 pixels, 6 the
    // interrupt, 5 fill mode, 0-3 the palette), 16 palettes of 16 colours at
    // $9E00 ($0RGB, low byte first). 640 mode: four 2-bit pixels a byte,
    // from colours 8-11, 12-15, 0-3, 4-7 of the palette in turn
    void drawShrLine(int line) {
        m_shrOn[line] = m_c029 & 0x80;
        if (!m_shrOn[line]) return;
        const uint8_t* e1 = &m_slow[0x10000];
        const uint8_t scb = e1[0x9D00 + line];
        const uint8_t* pal = &e1[0x9E00 + (scb & 0x0F) * 32];
        uint32_t colors[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t lo = pal[i * 2], hi = pal[i * 2 + 1];
            colors[i] = 0xFF000000u | (uint32_t)(hi & 0x0F) * 0x11u << 16 | (uint32_t)(lo >> 4) * 0x11u << 8 | (uint32_t)(lo & 0x0F) * 0x11u;
        }
        uint32_t* out = &m_shrBack[(size_t)line * SHR_WIDTH];
        const uint8_t* px = &e1[0x2000 + line * 160];
        if (scb & 0x80) {
            static const int BASE[4] = {8, 12, 0, 4};
            for (int x = 0; x < 160; ++x)
                for (int k = 0; k < 4; ++k) *out++ = colors[BASE[k] + ((px[x] >> (6 - 2 * k)) & 3)];
        } else {
            int last = 0;
            for (int x = 0; x < 160; ++x)
                for (int k = 0; k < 2; ++k) {
                    int c = k ? px[x] & 0x0F : px[x] >> 4;
                    if ((scb & 0x20) && c == 0) c = last;           // fill mode: colour 0 repeats
                    last = c;
                    *out++ = colors[c];
                    *out++ = colors[c];
                }
        }
    }
    uint8_t floating() { syncVideo(); return m_video->floatingBus(); }
    void setVideo(uint32_t set, uint32_t clear) {
        syncVideo();
        m_vf = (m_vf | set) & ~clear;
        m_video->setMode(m_vf);
    }
    void setState(uint32_t v) {
        m_state = v;
        const bool p2 = v & PAGE2;
        if (p2 != (bool)(m_vf & Apple2Video::VF_PAGE2)) setVideo(p2 ? (uint32_t)Apple2Video::VF_PAGE2 : 0, p2 ? 0 : (uint32_t)Apple2Video::VF_PAGE2);
    }

    // The VBL interrupt, the quarter-second (every 16 VBLs) and one-second
    // (every 60) interrupts
    void onVbl() {
        m_adb->vbl();
        if (m_c041 & 0x08) { m_c046 |= 0x08; m_irqVbl = true; }
        if (++m_cnt25 >= 16) {
            m_cnt25 = 0;
            if (m_c041 & 0x10) { m_c046 |= 0x10; m_irq25 = true; }
        }
        if (++m_cnt1s >= 60) {
            m_cnt1s = 0;
            m_c023 |= 0x40;
            if (m_c023 & 0x04) { m_c023 |= 0x80; m_irq1s = true; }
        }
    }
    bool irqLine() const { return m_irqVbl || m_irq25 || m_irq1s || m_irqScan || m_adb->irq(); }

    // $C000-$CFFF is I/O in banks $E0/$E1, and in $00/$01 unless $C035 bit 6
    bool isIo(int k, uint16_t a) const { return a >= 0xC000 && a < 0xD000 && (k >= 2 || !(m_shadow & 0x40)); }
    uint8_t* base(int k) { return k < 2 ? &m_fast[(size_t)k << 16] : &m_slow[(size_t)(k & 1) << 16]; }
    uint8_t romFF(uint16_t a) const { return m_rom[m_rom.size() - 0x10000 + a]; }

    // Main or auxiliary memory for $0000-$BFFF of an even bank ($00, $E0)
    bool auxFor(uint16_t a, bool write) const {
        if (a < 0x200) return m_state & ALTZP;
        const bool st80 = m_vf & Apple2Video::VF_80STORE;
        if (a >= 0x400 && a < 0x800 && st80) return m_state & PAGE2;
        if (a >= 0x2000 && a < 0x4000 && st80 && (m_vf & Apple2Video::VF_HIRES)) return m_state & PAGE2;
        return m_state & (write ? RAMWRT : RAMRD);
    }

    uint8_t memRead(int k, uint16_t a) {
        const bool even = !(k & 1);
        if (k < 2 && (m_shadow & 0x40) && a >= 0xC000) {           // I/O and the language card inhibited
            if (a < 0xD000) return base(k)[a + 0x1000];
            uint8_t* b = (even && (m_state & ALTZP)) ? base(k + 1) : base(k);
            return b[a < 0xE000 ? a - 0x1000 : a];
        }
        if (a >= 0xD000) {                                          // the language card
            if (k < 2 && (m_state & RDROM)) return romFF(a);
            uint8_t* b = (even && (m_state & ALTZP)) ? base(k + 1) : base(k);
            return b[(a < 0xE000 && !(m_state & LCBANK2)) ? a - 0x1000 : a];
        }
        return (even && auxFor(a, false)) ? base(k + 1)[a] : base(k)[a];
    }
    // Returns true when the write also went to the slow RAM
    bool memWrite(int k, uint16_t a, uint8_t v) {
        const bool even = !(k & 1);
        if (k < 2 && (m_shadow & 0x40) && a >= 0xC000) {
            if (a < 0xD000) { base(k)[a + 0x1000] = v; return false; }
            uint8_t* b = (even && (m_state & ALTZP)) ? base(k + 1) : base(k);
            b[a < 0xE000 ? a - 0x1000 : a] = v;
            return false;
        }
        if (a >= 0xD000) {
            if (k < 2 && !(m_state & WRDEFRAM)) return false;
            uint8_t* b = (even && (m_state & ALTZP)) ? base(k + 1) : base(k);
            b[(a < 0xE000 && !(m_state & LCBANK2)) ? a - 0x1000 : a] = v;
            return false;
        }
        const int p = (even && auxFor(a, true)) ? 1 : (k & 1);     // the main (0) or auxiliary (1) 64 KB
        base((k & 2) | p)[a] = v;
        if (k < 2 && shadowed(p, a)) { m_slow[((size_t)p << 16) | a] = v; return true; }
        return k >= 2;
    }
    // $C035 (a bit set inhibits): 0 text page 1, 5 text page 2, 1 hi-res
    // page 1, 2 hi-res page 2, 4 the auxiliary hi-res pages, 3 Super Hi-Res
    // (bank $01 $2000-$9FFF)
    bool shadowed(int p, uint16_t a) const {
        const uint32_t s = m_shadow;
        if (a >= 0x400 && a < 0x800) return !(s & 0x01);
        if (a >= 0x800 && a < 0xC00) return !(s & 0x20);
        if (a >= 0x2000 && a < 0x4000) return p ? (!(s & 0x12) || !(s & 0x08)) : !(s & 0x02);
        if (a >= 0x4000 && a < 0x6000) return p ? (!(s & 0x14) || !(s & 0x08)) : !(s & 0x04);
        if (a >= 0x6000 && a < 0xA000) return p && !(s & 0x08);
        return false;
    }

    uint8_t megaRead(int k, uint16_t a) {
        if (isIo(k, a)) {
            tick(a < 0xC100 || k >= 2);
            return a < 0xC100 ? io(a, nullptr) : cxRom(a, true);
        }
        // Vector pulls come from the ROM (moremem_fix_vector_pull) unless
        // the I/O and language card are inhibited
        if (k == 0 && a >= 0xFFE0 && m_cpu && m_cpu->vectorPull() && !(m_shadow & 0x40)) { tick(false); return romFF(a); }
        tick(k >= 2);
        return memRead(k, a);
    }
    void megaWrite(int k, uint16_t a, uint8_t v) {
        if (isIo(k, a)) {
            tick(true);
            if (a < 0xC100) io(a, &v);
            else cxWrite(a);
            return;
        }
        tick(memWrite(k, a, v));
    }

    // $C100-$CFFF (fixup_intcx, c3xx_read): the internal ROM unless $C02D
    // picks a slot's card (none are fitted); $C3xx switches the internal
    // $C800 ROM in (INTC8ROM) until $CFFF
    uint8_t cxRom(uint16_t a, bool access) {
        const int s = (a >> 8) & 0x0F;
        uint8_t v;
        if (s < 8) {
            if (access && s == 3 && !(m_c02d & 0x08) && !(m_state & INTC8ROM)) m_state |= INTC8ROM;
            v = (!((m_c02d >> s) & 1) || (m_state & INTCX)) ? romFF(a) : (access ? floating() : 0);
        } else {
            v = (m_state & (INTCX | INTC8ROM)) ? romFF(a) : (access ? floating() : 0);
            if (access && a == 0xCFFF) m_state &= ~INTC8ROM;
        }
        return v;
    }
    void cxWrite(uint16_t a) {
        const int s = (a >> 8) & 0x0F;
        if (s == 0xF) { if (a == 0xCFFF) m_state &= ~INTC8ROM; return; }
        if (s == 4 || s >= 8) return;
        if (s != 3 && (a & 0xFF) == 0) return;
        if (!(m_c02d & 0x08) && !(m_state & INTC8ROM)) m_state |= INTC8ROM;
    }

    // $C02E/$C02F: the vertical counter ($100 + line, $FA-$FF in the last
    // six lines) and the horizontal one ($00, then $40-$7F)
    uint8_t vidCounter(int lo) {
        syncVideo();
        const int line = m_video->vert(), h = m_video->horz();
        const int v = line < 256 ? 0x100 + line : 0xFA + (line - 256);
        if (lo == 0xE) return (uint8_t)(v >> 1);
        return (uint8_t)(((v & 1) << 7) | (h == 0 ? 0 : 0x3F + h));
    }

    static uint8_t ior(bool b) { return b ? 0x80 : 0x00; }

    // $C000-$C0FF (io_read / io_write)
    uint8_t io(uint16_t a, const uint8_t* wr) {
        const int r = a & 0xFF;
        const uint8_t val = wr ? *wr : 0;
        if (r < 0x10) {
            if (!wr) return m_adb->readC000();
            const bool on = r & 1;
            switch (r >> 1) {
                case 0: setVideo(on ? (uint32_t)Apple2Video::VF_80STORE : 0, on ? 0 : (uint32_t)Apple2Video::VF_80STORE); break;
                case 1: setState(on ? m_state | RAMRD : m_state & ~RAMRD); break;
                case 2: setState(on ? m_state | RAMWRT : m_state & ~RAMWRT); break;
                case 3: setState(on ? m_state | INTCX : m_state & ~INTCX); break;
                case 4: setState(on ? m_state | ALTZP : m_state & ~ALTZP); break;
                case 5: if (on) m_c02d |= 0x08; else m_c02d &= ~0x08; break;   // SLOTC3ROM / INTC3ROM
                case 6: setVideo(on ? (uint32_t)Apple2Video::VF_80COL : 0, on ? 0 : (uint32_t)Apple2Video::VF_80COL); break;
                case 7: syncVideo(); m_altChar = on; m_video->setAltCharSet(on); break;
            }
            return 0;
        }
        if (r < 0x20) {
            if (wr || r == 0x10) { const uint8_t k = m_adb->accessC010(); return wr ? 0 : k; }
            switch (r) {
                case 0x11: return ior(m_state & LCBANK2);
                case 0x12: return ior(!(m_state & RDROM));
                case 0x13: return ior(m_state & RAMRD);
                case 0x14: return ior(m_state & RAMWRT);
                case 0x15: return ior(m_state & INTCX);
                case 0x16: return ior(m_state & ALTZP);
                case 0x17: return ior(m_c02d & 0x08);
                case 0x18: return ior(m_vf & Apple2Video::VF_80STORE);
                case 0x19: syncVideo(); return ior(!m_video->vblBar());      // the IIgs: set in VBL
                case 0x1A: return ior(m_vf & Apple2Video::VF_TEXT);
                case 0x1B: return ior(m_vf & Apple2Video::VF_MIXED);
                case 0x1C: return ior(m_state & PAGE2);
                case 0x1D: return ior(m_vf & Apple2Video::VF_HIRES);
                case 0x1E: return ior(m_altChar);
                default:   return ior(m_vf & Apple2Video::VF_80COL);
            }
        }
        switch (r) {
            case 0x20: return wr ? 0 : floating();
            case 0x21: if (wr) m_c021 = val & 0x80; return m_c021;          // monochrome
            case 0x22: if (wr) m_tbColor = val; return m_tbColor;          // text / background colours
            case 0x23:                                                      // VGC interrupts
                if (wr) {
                    uint32_t t = (m_c023 & 0x70) | (val & 0x0F);
                    if ((t & 0x44) == 0x44) m_irq1s = true;
                    if (!(t & 0x04)) m_irq1s = false;
                    if ((t & 0x22) == 0x22) m_irqScan = true;
                    if (!(t & 0x02)) m_irqScan = false;
                    if (m_irq1s || m_irqScan) t |= 0x80;
                    m_c023 = t;
                    return 0;
                }
                return (uint8_t)m_c023;
            case 0x24: return 0;                                            // mouse: not yet
            case 0x25: return wr ? 0 : m_adb->c025();
            case 0x26: if (wr) { m_adb->writeC026(val); return 0; } return m_adb->readC026();
            case 0x27: if (wr) { m_adb->writeC027(val); return 0; } return m_adb->readC027();
            case 0x28: case 0x2A: case 0x2C: return 0;
            case 0x29:                                                      // NEWVIDEO
                if (wr) m_c029 = val & 0xE1;
                return m_c029;
            case 0x2B: if (wr) m_c02b = val; return m_c02b;
            case 0x2D: if (wr) m_c02d = val; return m_c02d;                 // slot ROM select
            case 0x2E: case 0x2F: return wr ? 0 : vidCounter(r & 0xF);
            case 0x30: m_spk->toggle(slowCycles()); return wr ? 0 : floating();
            case 0x31: if (wr) m_c031 = val & 0xC0; return m_c031;         // 3.5" drive control
            case 0x32:                                                      // clear the VGC interrupts
                if (wr) {
                    uint32_t t = m_c023 & 0x7F;
                    if (!(val & 0x40) && (t & 0x40)) { m_irq1s = false; t &= 0xBF; }
                    if (!(val & 0x20) && (t & 0x20)) { m_irqScan = false; t &= 0xDF; }
                    if (m_irq1s || m_irqScan) t |= 0x80;
                    m_c023 = t;
                }
                return 0;
            case 0x33: if (wr) m_clock.setData(val); return m_clock.data();
            case 0x34: if (wr) { syncVideo(); m_clock.setCtl(val); } return m_clock.ctl();
            case 0x35: if (wr) m_shadow = val; return (uint8_t)m_shadow;
            case 0x36:                                                      // CYAREG: speed
                if (wr) m_c036 = val & ~0x60u;                              // ROM 01: bits 5, 6 read 0
                return (uint8_t)m_c036;
            case 0x37: return 0;
            case 0x38: case 0x39: if (wr) { m_scc.writeReg(0x39 - r, val); return 0; } return m_scc.readReg(0x39 - r);
            case 0x3A: case 0x3B: return 0;                                 // SCC data
            case 0x3C: if (wr) m_doc.writeCtl(val); return m_doc.readCtl();
            case 0x3D: if (wr) { m_doc.writeData(val); return 0; } return m_doc.readData();
            case 0x3E: case 0x3F: if (wr) m_doc.writePtr(r & 1, val); return m_doc.readPtr(r & 1);
            case 0x41:                                                      // interrupt enables
                if (wr) {
                    m_c041 = val & 0x1F;
                    if (!(val & 0x08)) m_irqVbl = false;
                    if (!(val & 0x10)) m_irq25 = false;
                }
                return (uint8_t)m_c041;
            case 0x46: {                                                    // interrupt flags
                if (wr) return 0;
                const uint32_t t = m_c046;
                m_c046 = (t & 0xBF) | ((t & 0x80) >> 1);
                return (uint8_t)t;
            }
            case 0x47: m_irqVbl = m_irq25 = false; m_c046 &= 0xE7; return 0;   // clear VBL, quarter-second
        }
        if (r >= 0x40 && r < 0x50) return 0;
        if (r < 0x60) {                                                     // $C050-$C05F
            const int s = r & 0x0F;
            switch (s) {
                case 0x0: setVideo(0, Apple2Video::VF_TEXT); break;
                case 0x1: setVideo(Apple2Video::VF_TEXT, 0); break;
                case 0x2: setVideo(0, Apple2Video::VF_MIXED); break;
                case 0x3: setVideo(Apple2Video::VF_MIXED, 0); break;
                case 0x4: setState(m_state & ~PAGE2); break;
                case 0x5: setState(m_state | PAGE2); break;
                case 0x6: setVideo(0, Apple2Video::VF_HIRES); break;
                case 0x7: setVideo(Apple2Video::VF_HIRES, 0); break;
                case 0xE: setVideo(Apple2Video::VF_DHIRES, 0); return 0;    // AN3 off: double hi-res
                case 0xF: setVideo(0, Apple2Video::VF_DHIRES); return 0;
                default: return 0;                                          // annunciators 0-2
            }
            return wr ? 0 : floating();
        }
        if (r < 0x70) {
            if (wr) { if (r == 0x68) setState((m_state & ~0xFFu) | val); return 0; }
            switch (r) {
                case 0x60: return 0;
                case 0x61: return ior(m_adb->cmdDown() || m_button[0]);
                case 0x62: return ior(m_adb->optionDown() || m_button[1]);
                case 0x63: return 0;
                case 0x64: case 0x65: case 0x66: case 0x67: {
                    const int p = r - 0x64;
                    return ior((double)(slowCycles() - m_ptrig) <= m_paddle[p] * (2816.0 / 255.0));
                }
                case 0x68: return (uint8_t)m_state;
                default: return 0;
            }
        }
        if (r < 0x80) {
            if (r == 0x70) { m_ptrig = slowCycles(); return wr ? 0 : floating(); }
            return wr ? 0 : romFF(a);                                       // $C071-$C07F: the ROM
        }
        if (r < 0x90) {                                                     // the language card
            const uint32_t bank2 = ((a >> 1) & 0x4) ^ 0x4;
            const uint32_t rdrom = ((a << 3) ^ (a << 2)) & 8;
            uint32_t wrdef = m_state & WRDEFRAM, prewrite = 0;
            if (!(a & 1)) wrdef = 0;
            else if (!wr) { wrdef |= (m_state << 1) & WRDEFRAM; prewrite = PREWRITE; }
            setState((m_state & ~(PREWRITE | WRDEFRAM | RDROM | LCBANK2)) | bank2 | rdrom | prewrite | wrdef);
            return wr ? 0 : floating();
        }
        if (r >= 0xE0 && r < 0xF0) return iwm(r & 0x0F, wr);              // slot 6: the IWM
        return 0;                                                           // slots 1-7: no cards yet
    }

    // The IWM (iwm.c read_iwm / write_iwm): the switches, the mode register
    // (Q7, Q6 set, motor off) and the status register (the sense line, the
    // motor, the mode). With the 5.25" drives selected ($C031 bit 6 clear)
    // the switches and the data go to the Disk II emulation, which the
    // IWM's synchronous mode matches
    uint8_t iwm(int loc, const uint8_t* wr) {
        const bool on = loc & 1;
        if (loc < 8) {
            if (on) m_iwmPhases |= 1u << (loc >> 1);
            else m_iwmPhases &= ~(1u << (loc >> 1));
        }
        if (m_drives35 && (m_c031 & 0x40)) return iwm35(loc, wr);
        if (m_disk && !(m_c031 & 0x40)) {
            const uint16_t addr = (uint16_t)(0xC0E0 | loc);
            const int r = m_disk->io(addr, wr, slowCycles());
            switch (loc >> 1) {
                case 4: m_iwmMotor = on; break;
                case 5: m_iwmDrive2 = on; break;
                case 6: m_iwmQ6 = on; break;
                case 7: m_iwmQ7 = on; break;
            }
            if (wr) {
                if (on && m_iwmQ6 && m_iwmQ7 && !m_iwmMotor) m_iwmMode = *wr & 0x1F;
                return 0;
            }
            if (on) return 0;
            const int q = (m_iwmQ7 ? 2 : 0) | (m_iwmQ6 ? 1 : 0);
            if (q == 1) return (uint8_t)((r >= 0 ? (r & 0x80) : 0x80) | (m_iwmMotor ? 0x20 : 0) | m_iwmMode);
            if (q == 2) return 0x80;
            if (q == 0) return r >= 0 ? (uint8_t)r : 0xFF;
            return 0;
        }
        switch (loc >> 1) {
            case 0: case 1: case 2: case 3: break;                          // the stepper phases
            case 4: m_iwmMotor = on; break;
            case 5: m_iwmDrive2 = on; break;
            case 6: m_iwmQ6 = on; break;
            case 7: m_iwmQ7 = on; break;
        }
        if (wr) {
            if (on && m_iwmQ6 && m_iwmQ7 && !m_iwmMotor) m_iwmMode = *wr & 0x1F;
            return 0;
        }
        if (on) return 0;
        const int q = (m_iwmQ7 ? 2 : 0) | (m_iwmQ6 ? 1 : 0);
        switch (q) {
            case 0: return 0xFF;                                            // data: nothing read
            case 1: return (uint8_t)(0x80 | (m_iwmMotor ? 0x20 : 0) | m_iwmMode);   // sense high: no disk
            case 2: return 0x80;                                            // handshake: ready, underrun (nothing written)
            default: return 0;
        }
    }

    // The 3.5" drives ($C031 bit 6; iwm.c): the phase lines CA0-CA2 and SEL
    // ($C031 bit 7) pick a drive register; phase 3 (LSTRB) rising runs it as
    // a command, the status register reads it in bit 7
    int drive35Reg() const {
        return (int)(((m_iwmPhases & 3) << 2) | ((m_c031 >> 6) & 2) | ((m_iwmPhases >> 2) & 1));
    }
    uint8_t iwm35(int loc, const uint8_t* wr) {
        const bool on = loc & 1;
        Gs35Drive& d = m_drives35->drive[m_iwmDrive2 ? 1 : 0];
        switch (loc >> 1) {
            case 3: if (on && m_iwmMotor) d.command(drive35Reg(), m_ticks); break;
            case 4: m_iwmMotor = on; break;
            case 5: m_iwmDrive2 = on; break;
            case 6: m_iwmQ6 = on; break;
            case 7:
                if (!on && m_iwmQ7 && m_iwmMotor) d.endWrite();     // iwm_write_end
                m_iwmQ7 = on;
                break;
        }
        Gs35Drive& cur = m_drives35->drive[m_iwmDrive2 ? 1 : 0];
        if (wr) {
            if (on && m_iwmQ6 && m_iwmQ7) {
                if (m_iwmMotor) cur.writeNibble(*wr, m_ticks);
                else m_iwmMode = *wr & 0x1F;
            }
            return 0;
        }
        if (on) return 0;
        const int q = (m_iwmQ7 ? 2 : 0) | (m_iwmQ6 ? 1 : 0);
        switch (q) {
            case 0: return m_iwmMotor ? cur.readNibble(m_ticks) : 0xFF;
            case 1: {
                const int st = m_iwmMotor ? cur.status(drive35Reg(), m_ticks) : 1;
                return (uint8_t)((st & 1) << 7 | (m_iwmMotor ? 0x20 : 0) | m_iwmMode);
            }
            case 2: return cur.handshake(m_ticks);
            default: return 0;
        }
    }

    BusConfig m_cfg;
    std::vector<uint8_t> m_rom, m_fast, m_slow;
    uint32_t m_romBase = 0;
    Apple2Video* m_video;
    GsAdb* m_adb;
    Apple2Speaker* m_spk;
    const W65C816* m_cpu = nullptr;
    Disk2Card* m_disk = nullptr;
    Gs35Drives* m_drives35 = nullptr;
    uint32_t m_iwmPhases = 0;
    GsClock m_clock;
    GsDoc m_doc;
    GsScc m_scc;
    uint64_t m_ticks = 0, m_videoCycle = 0, m_instr = 0, m_ptrig = 0;
    int m_accesses = 0;
    uint32_t m_state = 0, m_shadow = 0, m_c036 = 0x80, m_c023 = 0, m_c041 = 0, m_c046 = 0;
    uint8_t m_c021 = 0, m_c029 = 0x01, m_c02b = 0x08, m_c02d = 0, m_c031 = 0, m_tbColor = 0xF0;
    uint32_t m_vf = Apple2Video::VF_TEXT;
    bool m_altChar = false, m_lastVbl = false;
    bool m_irqVbl = false, m_irq25 = false, m_irq1s = false, m_irqScan = false;
    std::vector<uint32_t> m_shrBack = std::vector<uint32_t>((size_t)SHR_WIDTH * SHR_LINES, 0xFF000000u);
    std::vector<uint32_t> m_shrFront = m_shrBack;
    std::vector<bool> m_shrOn = std::vector<bool>(SHR_LINES, false), m_shrOnFront = m_shrOn;
    int m_cnt25 = 0, m_cnt1s = 0;
    bool m_iwmMotor = false, m_iwmDrive2 = false, m_iwmQ6 = false, m_iwmQ7 = false;
    uint8_t m_iwmMode = 0;
    int m_paddle[4] = {127, 127, 127, 127};
    bool m_button[2] = {};

public:
    void setPaddle(int n, int pos) { m_paddle[n & 3] = std::clamp(pos, 0, 255); }
    void setButton(int n, bool down) { m_button[n & 1] = down; }
};

// The IIgs colours (IIgs Technote #63; KEGS g_lores_colors), 12-bit RGB
constexpr uint16_t GS_COLORS[16] = {0x000, 0xD03, 0x009, 0xD2D, 0x072, 0x555, 0x22F, 0x6AF,
                                    0x850, 0xF60, 0xAAA, 0xF98, 0x1D0, 0xFF0, 0x4F9, 0xFFF};
uint32_t gsArgb(int c) {
    const uint16_t v = GS_COLORS[c & 15];
    return 0xFF000000u | ((uint32_t)((v >> 8) & 15) * 0x11u) << 16 | ((uint32_t)((v >> 4) & 15) * 0x11u) << 8 |
           ((uint32_t)(v & 15) * 0x11u);
}

// The picture: the Apple II screen (the text in $C022's colours) or Super
// Hi-Res, inside the border ($C034)
class GsDisplay : public IOHandler, public IVideoOutput {
public:
    static constexpr int W = 704, H = 456;
    static constexpr int X0 = (W - Apple2Video::FRAME_WIDTH) / 2, Y0 = (H - Apple2Video::FRAME_HEIGHT) / 2;
    static constexpr int SHR_X0 = (W - 640) / 2, SHR_Y0 = (H - 400) / 2;
    GsDisplay(Apple2Video* v, GsBus* bus) : m_video(v), m_bus(bus), m_inner((size_t)Apple2Video::FRAME_WIDTH * Apple2Video::FRAME_HEIGHT) {}
    const char* name() const override { return "Video"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override { return {W, H, W, H}; }
    void renderFrame(uint32_t* buf) override {
        const uint32_t border = gsArgb(m_bus->border());
        for (int i = 0; i < W * H; ++i) buf[i] = border;
        if (m_bus->newVideo() & 0x80) {                // Super Hi-Res: 640 x 200, lines doubled
            const auto& f = m_bus->shrFrame();
            for (int y = 0; y < GsBus::SHR_LINES; ++y)
                for (int d = 0; d < 2; ++d) {
                    uint32_t* dst = &buf[(size_t)(SHR_Y0 + 2 * y + d) * W + SHR_X0];
                    if (m_bus->shrLine(y)) std::memcpy(dst, &f[(size_t)y * GsBus::SHR_WIDTH], GsBus::SHR_WIDTH * sizeof(uint32_t));
                    else std::fill(dst, dst + GsBus::SHR_WIDTH, 0xFF000000u);
                }
            return;
        }
        m_video->renderFrame(m_inner.data());
        const uint32_t vf = m_bus->videoFlags();
        // Text lines: the white-on-black picture recoloured
        const int textFrom = (vf & Apple2Video::VF_TEXT) ? 0 : (vf & Apple2Video::VF_MIXED) ? 320 : Apple2Video::FRAME_HEIGHT;
        const uint32_t fg = gsArgb(m_bus->tbColor() >> 4), bg = gsArgb(m_bus->tbColor() & 15);
        for (int y = 0; y < Apple2Video::FRAME_HEIGHT; ++y) {
            const uint32_t* src = &m_inner[(size_t)y * Apple2Video::FRAME_WIDTH];
            uint32_t* dst = &buf[(size_t)(y + Y0) * W + X0];
            if (y < textFrom) { std::memcpy(dst, src, Apple2Video::FRAME_WIDTH * sizeof(uint32_t)); continue; }
            for (int x = 0; x < Apple2Video::FRAME_WIDTH; ++x) {
                const uint32_t p = src[x];
                const int l = (int)(((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)) / 3;
                dst[x] = mix(bg, fg, l);
            }
        }
    }

private:
    static uint32_t mix(uint32_t a, uint32_t b, int t) {
        auto ch = [&](int s) {
            const int x = (int)((a >> s) & 0xFF), y = (int)((b >> s) & 0xFF);
            return (uint32_t)(x + (y - x) * t / 255) << s;
        };
        return 0xFF000000u | ch(16) | ch(8) | ch(0);
    }
    Apple2Video* m_video;
    GsBus* m_bus;
    std::vector<uint32_t> m_inner;
};

}  // namespace

MachineDescriptor* createIIgs(std::string* err) {
    // The IIgs ROM is Apple's and not distributed here (doc/README-APPLE2.md)
    const char* romFile = "roms/apple2/apple2gs-rom01.rom";
    std::vector<uint8_t> rom, chars;
    {
        std::ifstream f(romFile, std::ios::binary);
        if (f) rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (rom.size() != 0x20000) {
        if (err) *err = std::string("missing Apple IIgs ROM 01 ") + romFile + " (128 KB; local only, see doc/README-APPLE2.md)";
        return nullptr;
    }
    // Bank $FE then $FF; some dumps have the halves the other way round
    // (bank $FF, with the vectors, first): put them in order
    if (rom[0x1FFFC] == 0 && rom[0x1FFFD] == 0 && (rom[0xFFFC] | rom[0xFFFD]))
        std::rotate(rom.begin(), rom.begin() + 0x10000, rom.end());
    // The Mega II's character generator: the enhanced IIe's (MouseText)
    if (!loadFile("roms/apple2/Apple2e_Enhanced_Video.rom", 0x1000, chars, err)) return nullptr;
    // The 5.25" drives: the IIgs ROM's slot 6 firmware drives them through
    // the IWM; the Disk II emulation (its P5 ROM is not used)
    std::vector<uint8_t> disk2;
    if (!loadFile("roms/apple2/DISK2.rom", 0x100, disk2, err)) return nullptr;

    auto* desc = new MachineDescriptor();
    desc->machineId = "apple2gs";
    desc->displayName = "Apple IIgs";
    desc->licenseClass = "proprietary";
    desc->clockHz = CPU_HZ;
    desc->uiPanesSet = true;
    desc->uiPanes = {"drive"};

    auto* video = new Apple2Video(Apple2Video::Model::IIeEnhanced);
    video->setCharRom(std::move(chars));
    auto* adb = new GsAdb();
    auto* spk = new Apple2Speaker();
    auto* bus = new GsBus(std::move(rom), video, adb, spk);
    auto* display = new GsDisplay(video, bus);
    desc->buses.push_back({"system", bus});
    auto* cpu = new W65C816();
    cpu->setDataBus(bus);
    bus->setCpu(cpu);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(display);
    io->registerHandler(adb);
    io->registerHandler(spk);
    // The 3.5" drives first: they take only 800 KB images, so `disk mount`
    // (which tries each drive device in turn) gives the 5.25" drives the rest
    auto* drives35 = new Gs35Drives();
    bus->setDrives35(drives35);
    io->registerHandler(drives35);
    auto* disk = new Disk2Card(std::move(disk2));
    bus->setDisk(disk);
    io->registerHandler(disk);
    desc->deleters.push_back([video, display, adb, spk, disk, drives35]() {
        delete display; delete video; delete adb; delete spk; delete disk; delete drives35;
    });

    bus->onIrq = [cpu](bool on) { cpu->setIrqLine(on); };
    // Control-Reset; with Command (Open Apple) the ROM cold-starts
    adb->onReset = [bus, cpu, disk](bool, bool) {
        bus->ctrlReset();
        disk->reset(false);
        cpu->reset();
    };
    desc->onKey = [adb](const std::string& n, bool down) { return adb->key(n, down); };
    desc->screenButtons = {{"RESET", {"control", "reset"}, "Control-Reset: the CPU restarts (BASIC warm-starts)."},
                           {"REBOOT", {"control", "alt", "reset"}, "Control-Command-Reset: a cold start."}};

    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [bus](int port, uint8_t bits) {
        const int p = (port & 1) * 2;
        bus->setPaddle(p, !(bits & 0x04) ? 0 : !(bits & 0x08) ? 255 : 127);
        bus->setPaddle(p + 1, !(bits & 0x01) ? 0 : !(bits & 0x02) ? 255 : 127);
        bus->setButton(port & 1, !(bits & 0x10));
    };
    desc->deviceCycles = [bus]() { return bus->slowCycles(); };
    desc->cpuSpeedMHz = [bus]() { return bus->speedMHz(); };

    desc->onReset = [bus, io, cpu](MachineDescriptor&) {     // power on
        bus->powerOn();
        io->resetAll();
        cpu->reset();
    };
    desc->schedulerStep = [bus, cpu, io, disk](MachineDescriptor&) -> int {
        const uint64_t before = bus->slowCycles();
        bus->begin();
        const int c = cpu->step();
        bus->end(c);
        const uint64_t slow = bus->slowCycles() - before;
        disk->update(bus->slowCycles(), slow);
        if (slow) io->tickAll(slow);
        return c;
    };
    if (desc->onReset) desc->onReset(*desc);
    return desc;
}

}  // namespace apple2
