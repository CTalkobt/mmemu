// Apple II / II+ machine, after AppleWin (GPL-2.0-or-later): Memory.cpp
// (the $C0xx soft switches, the floating bus), LanguageCard.cpp,
// Keyboard.cpp, Speaker.cpp, Joystick.cpp.

#include "machine_apple2.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/6502/main/cpu65c02.h"
#include "plugins/devices/apple2_video/main/apple2_video.h"
#include "disk2.h"
#include "apple2_parts.h"
#include <algorithm>
#include "plugins/devices/acia6551/main/acia6551.h"
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace apple2 {

namespace {

// ---------------------------------------------------------------------------
// Keyboard (Keyboard.cpp): a latch of the last key's ASCII code and a strobe
// (bit 7 of $C000), cleared by $C010. The II and II+ keyboards have upper
// case only; the IIe has lower case with Caps Lock (on at power-on, as
// AppleWin), $C010 reads "any key down" in bit 7, and the Open and Closed
// Apple keys (Alt, Right Alt) are buttons 0 and 1. Keys come by name (the
// frontends' US layout, with Shift and Control tracked here) or as typed
// text.
// ---------------------------------------------------------------------------
class Apple2Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    std::function<void()> onReset;              // the RESET key
    std::function<void(int, bool)> onAppleKey;  // IIe: Open (0) / Closed (1) Apple

    explicit Apple2Keyboard(bool iie) : m_iie(iie) {}
    bool anyKeyDown() const { return !m_down.empty(); }

    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { m_strobe = false; m_queue.clear(); m_wait = 0; m_down.clear(); }

    uint8_t data() const { return (uint8_t)(m_code | (m_strobe ? 0x80 : 0)); }
    void clearStrobe() { m_strobe = false; }

    // Typed text: the next character once the program took the last one
    void tick(uint64_t cycles) override {
        if (m_queue.empty() || m_strobe) return;
        if (m_wait > cycles) { m_wait -= cycles; return; }
        latch(m_queue.front());
        m_queue.pop_front();
        m_wait = 2000;
    }

    bool key(std::string n, bool down) {
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (n == "left_shift" || n == "right_shift" || n == "shift") { m_shift = down; return true; }
        if (n == "control" || n == "ctrl") { m_ctrl = down; return true; }
        if (n == "reset") { if (down && onReset) onReset(); return true; }
        if (m_iie) {
            if (n == "alt" || n == "left_alt" || n == "open_apple") { if (onAppleKey) onAppleKey(0, down); return true; }
            if (n == "right_alt" || n == "altgr" || n == "closed_apple") { if (onAppleKey) onAppleKey(1, down); return true; }
            if (n == "caps_lock") { if (down) m_caps = !m_caps; return true; }
        }
        if (!down) { m_down.erase(n); return true; }
        static const std::map<std::string, uint8_t> special = {
            {"return", 0x0D}, {"enter", 0x0D}, {"left", 0x08}, {"delete", 0x08}, {"backspace", 0x08},
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
        if (m_ctrl && std::isalpha(c)) c &= 0x1F;
        m_down.insert(n);
        latch((uint8_t)c);
        return true;
    }

    // IKeyboardMatrix (no matrix: names and typed text)
    void keyDown(int, int) override {}
    void keyUp(int, int) override {}
    void clearKeys() override { m_queue.clear(); m_shift = m_ctrl = false; }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    IPortDevice* getPort(int) override { return nullptr; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\r'; ++i; }
            if (c == '\n') c = '\r';
            if (!m_iie) c = (char)std::toupper((unsigned char)c);
            m_queue.push_back((uint8_t)c & 0x7F);
        }
    }

private:
    int shifted(char c) const {
        static const char base[] = "1234567890-=;',./[]\\`";
        static const char up[] = "!@#$%^&*()_+:\"<>?{}|~";
        if (m_shift)
            if (const char* p = std::strchr(base, c)) c = up[p - base];
        if (m_iie && std::isalpha((unsigned char)c))
            return (m_shift || m_caps) ? std::toupper((unsigned char)c) : std::tolower((unsigned char)c);
        return std::toupper((unsigned char)c);
    }
    void latch(uint8_t c) { m_code = c & 0x7F; m_strobe = true; }

    const bool m_iie;
    bool m_caps = true;
    std::set<std::string> m_down;
    uint8_t m_code = 0;
    bool m_strobe = false, m_shift = false, m_ctrl = false;
    std::deque<uint8_t> m_queue;
    uint64_t m_wait = 0;
};

// ---------------------------------------------------------------------------
// The bus (Memory.cpp, LanguageCard.cpp): RAM, the soft switches, the
// language card; on the IIe also the 64 KB auxiliary RAM (an extended
// 80-column card), the internal $C100-$CFFF ROM and the IIe switches
// ---------------------------------------------------------------------------
class Apple2Bus : public IBus {
public:
    // `rom`: 12 KB at $D000 (II/II+), or 16 KB at $C000 (IIe: the internal
    // $C100-$CFFF ROM, then $D000-$FFFF)
    Apple2Bus(std::vector<uint8_t> rom, Apple2Video* video, Apple2Keyboard* kbd, Apple2Speaker* spk,
              Disk2Card* disk, bool iie, bool enhanced, bool iic = false)
        : m_rom(std::move(rom)), m_video(video), m_kbd(kbd), m_spk(spk), m_disk(disk),
          m_iie(iie), m_enhanced(enhanced), m_iic(iic) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        m_ram.assign(0x10000, 0);
        m_lc.assign(0x4000, 0);
        if (m_iie) m_aux.assign(0x10000, 0);
        video->setMemory(m_ram.data(), m_iie ? m_aux.data() : nullptr);
        powerOn();
    }

    void powerOn() {
        // AppleWin fills RAM with a pattern; zeroes are as good for the ROMs
        std::fill(m_ram.begin(), m_ram.end(), 0);
        std::fill(m_aux.begin(), m_aux.end(), 0);
        m_cycles = m_videoCycle = 0;
        m_annunciators = 0;
        resetPaging();
    }

    // The RESET key: on a IIe the memory switches, the language card and the
    // video modes go back to their power-on state (AppleWin CtrlReset,
    // LanguageCardManager::Reset, VideoResetState); a II/II+ keeps them
    void ctrlReset() {
        if (!m_iie) return;
        resetPaging();
        m_annunciators = 0;
    }

    // IIc: the serial ports' ACIAs (slots 1 and 2) and the CPU's /IRQ
    void setAcias(Acia6551* a1, Acia6551* a2) { m_acia[0] = a1; m_acia[1] = a2; }
    std::function<void(bool)> onIrq;

    // Instruction framing: accesses are counted so I/O happens on its cycle
    void begin() { m_access = 0; ++m_instr; }
    void end(int c) {
        m_cycles += (uint64_t)c;
        syncVideo(m_cycles);
        m_spk->advance(m_cycles);
        if (m_iic) {
            // The IOU's VBL interrupt: on entering vertical blanking, if enabled
            const bool vbl = !m_video->vblBar();
            if (vbl && !m_lastVbl && m_vblMask) m_irqMask |= IRQ_VBL;
            m_lastVbl = vbl;
            bool line = m_irqMask != 0;
            for (Acia6551* a : m_acia) if (a && a->irqAsserted()) line = true;
            if (onIrq) onIrq(line);
        }
    }
    uint64_t cycles() const { return m_cycles; }

    // Paddles (Joystick.cpp): $C07x starts the timers; paddle n reads bit 7
    // set for its position x 11.04 cycles
    void setPaddle(int n, int pos) { m_paddle[n & 3] = std::clamp(pos, 0, 255); }
    void setButton(int n, bool down) { m_button[n % 3] = down; }
    // IIe: Open Apple and Closed Apple are buttons 0 and 1
    void setAppleKey(int n, bool down) { m_appleKey[n & 1] = down; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        const uint64_t at = m_cycles + m_access++;
        uint8_t v;
        if (a < 0xC000) v = ramRead(a);
        else if (a < 0xC100) v = io(a, nullptr, at);
        else if (a < 0xD000) v = cx(a, at);
        else v = highRead(a);
        if (m_observer) m_observer->onMemoryRead(this, a, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        if (a < 0xC000) return ramRead(a);
        if (a < 0xC010) return m_kbd->data();
        if (a < 0xC100) return m_video->floatingBus();
        if (a < 0xD000) {
            const int src = cxSource(a);
            return src == CX_INTERNAL ? m_rom[romBank() + a - 0xC000] : src == CX_DISK ? m_disk->rom((uint8_t)a)
                                                                           : m_video->floatingBus();
        }
        return highRead(a);
    }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t a = addr & 0xFFFF;
        const uint64_t at = m_cycles + m_access++;
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        if (a < 0xC000) ramWrite(a, v);
        else if (a < 0xC100) io(a, &v, at);
        else if (a < 0xD000) cx(a, at);
        else if (m_lcWriteRam) {
            if (m_altZp) m_aux[auxLcIndex(a)] = v;
            else m_lc[lcIndex(a)] = v;
        }
    }
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

    uint8_t* ram() { return m_ram.data(); }
    uint8_t* aux() { return m_aux.empty() ? nullptr : m_aux.data(); }
    uint32_t videoFlags() const { return m_vf; }
    bool lcReadRam() const { return m_lcReadRam; }
    bool lcWriteRam() const { return m_lcWriteRam; }
    bool lcBank2() const { return m_lcBank2; }

private:
    enum { CX_FLOATING, CX_INTERNAL, CX_DISK };

    void resetPaging() {
        // LanguageCardUnit::kMemModeInitialState: ROM read, bank 2, write
        // enabled; every other switch off
        m_lcReadRam = false; m_lcBank2 = true; m_lcWriteRam = true; m_lcPrewrite = false;
        m_80store = m_auxRead = m_auxWrite = m_intCxRom = m_altZp = m_slotC3Rom = m_intC8Rom = false;
        m_altChar = false;
        // IIc IOU (MAME machine_reset): IOUDIS on, interrupts off, ROM bank 0
        m_iouDis = true;
        m_vblMask = m_xyMask = m_x0Edge = m_y0Edge = false;
        m_irqMask = 0;
        m_romSwitch = false;
        if (m_iic) m_intCxRom = true;
        m_video->setAltCharSet(false);
        m_vf = Apple2Video::VF_TEXT;
        m_video->setMode(m_vf);
    }

    bool page2() const { return m_vf & Apple2Video::VF_PAGE2; }
    bool hires() const { return m_vf & Apple2Video::VF_HIRES; }

    // Main or aux for $0000-$BFFF (UpdatePaging): ALTZP for pages 0-1,
    // RAMRD/RAMWRT above, 80STORE with PAGE2 for the text page (and hi-res
    // page 1 with HIRES)
    bool auxFor(uint16_t a, bool write) const {
        if (!m_iie) return false;
        if (a < 0x200) return m_altZp;
        if (m_80store) {
            if (a >= 0x400 && a < 0x800) return page2();
            if (hires() && a >= 0x2000 && a < 0x4000) return page2();
        }
        return write ? m_auxWrite : m_auxRead;
    }
    uint8_t ramRead(uint16_t a) const { return auxFor(a, false) ? m_aux[a] : m_ram[a]; }
    void ramWrite(uint16_t a, uint8_t v) { (auxFor(a, true) ? m_aux : m_ram)[a] = v; }

    // $D000-$FFFF: ROM or the language card's RAM (aux with ALTZP)
    uint8_t highRead(uint16_t a) const {
        if (!m_lcReadRam) return m_rom[romBank() + a - romBase()];
        return m_altZp ? m_aux[auxLcIndex(a)] : m_lc[lcIndex(a)];
    }
    uint32_t romBase() const { return m_iie ? 0xC000 : 0xD000; }
    // IIc 32 KB ROMs: $C028 swaps in the second 16 KB
    uint32_t romBank() const { return (m_romSwitch && m_rom.size() >= 0x8000) ? 0x4000u : 0u; }
    // $D000-$DFFF: bank 1 at 0, bank 2 at $1000; $E000-$FFFF at $2000
    uint32_t lcIndex(uint16_t a) const {
        if (a >= 0xE000) return 0x2000u + (a - 0xE000u);
        return (m_lcBank2 ? 0x1000u : 0u) + (a - 0xD000u);
    }
    // Aux: bank 2 at $D000, bank 1 at $C000 (AppleWin memaux)
    uint32_t auxLcIndex(uint16_t a) const {
        if (a >= 0xE000 || m_lcBank2) return a;
        return a - 0x1000u;
    }

    // $C100-$CFFF (IO_Cxxx, UpdatePaging): which ROM answers. II/II+: the
    // cards (only the Disk II in slot 6). IIe: INTCXROM puts the internal
    // ROM at $C100-$CFFF; with SLOTC3ROM off, $C300 is internal and an access
    // there switches the internal $C800 ROM in (INTC8ROM) until $CFFF
    int cxSource(uint16_t a) const {
        if (a < 0xC800) {
            const int slot = (a >> 8) & 7;
            if (m_iic) return CX_INTERNAL;                  // the IIc: always the internal ROM
            if (m_iie && (m_intCxRom || (slot == 3 && !m_slotC3Rom))) return CX_INTERNAL;
            return slot == 6 ? CX_DISK : CX_FLOATING;
        }
        if (m_iic || (m_iie && (m_intCxRom || m_intC8Rom))) return CX_INTERNAL;
        return CX_FLOATING;     // no card has a $C800 ROM
    }
    uint8_t cx(uint16_t a, uint64_t at) {
        if (m_iie) {
            if (a == 0xCFFF) m_intC8Rom = false;
            else if (a >= 0xC300 && a < 0xC400 && !m_slotC3Rom) m_intC8Rom = true;
        }
        const int src = cxSource(a);
        if (src == CX_INTERNAL) return m_rom[romBank() + a - 0xC000];
        if (src == CX_DISK) return m_disk->rom((uint8_t)a);
        return floating(at);
    }

    void syncVideo(uint64_t to) {
        if (to > m_videoCycle) { m_video->clock((uint32_t)(to - m_videoCycle)); m_videoCycle = to; }
    }
    uint8_t floating(uint64_t at) { syncVideo(at); return m_video->floatingBus(); }
    void setVideo(uint32_t set, uint32_t clear, uint64_t at) {
        syncVideo(at);
        m_vf = (m_vf | set) & ~clear;
        m_video->setMode(m_vf);
    }

    // $C011-$C01F (IORead_C01x): a switch in bit 7, the key code below
    uint8_t status(int n, uint64_t at) {
        bool r = false;
        if (m_iic && (n == 0x5 || n == 0x7)) {         // mouse X0 / Y0 interrupt flags (read and clear)
            m_irqMask &= ~IRQ_MOUSE;
            return (uint8_t)(m_kbd->data() & 0x7F);
        }
        if (m_iic && n == 0x9)                          // VBLINT (not cleared by reading)
            return (uint8_t)((m_kbd->data() & 0x7F) | ((m_irqMask & IRQ_VBL) ? 0x80 : 0));
        switch (n) {
            case 0x1: r = m_lcBank2; break;
            case 0x2: r = m_lcReadRam; break;
            case 0x3: r = m_auxRead; break;
            case 0x4: r = m_auxWrite; break;
            case 0x5: r = m_intCxRom; break;
            case 0x6: r = m_altZp; break;
            case 0x7: r = m_slotC3Rom; break;
            case 0x8: r = m_80store; break;
            case 0x9: syncVideo(at); r = m_video->vblBar(); break;
            case 0xA: r = m_vf & Apple2Video::VF_TEXT; break;
            case 0xB: r = m_vf & Apple2Video::VF_MIXED; break;
            case 0xC: r = page2(); break;
            case 0xD: r = hires(); break;
            case 0xE: r = m_altChar; break;
            case 0xF: r = m_vf & Apple2Video::VF_80COL; break;
        }
        return (uint8_t)((m_kbd->data() & 0x7F) | (r ? 0x80 : 0));
    }

    // $C000-$C00F writes on a IIe (MemSetPaging, VideoSetMode)
    void iieSwitch(int n, uint64_t at) {
        const bool on = n & 1;
        switch (n >> 1) {
            case 0: m_80store = on; setVideo(on ? Apple2Video::VF_80STORE : 0, on ? 0 : Apple2Video::VF_80STORE, at); break;
            case 1: m_auxRead = on; break;
            case 2: m_auxWrite = on; break;
            case 3: m_intCxRom = on; break;
            case 4: m_altZp = on; break;
            case 5: m_slotC3Rom = on; break;
            case 6: setVideo(on ? Apple2Video::VF_80COL : 0, on ? 0 : Apple2Video::VF_80COL, at); break;
            case 7: syncVideo(at); m_altChar = on; m_video->setAltCharSet(on); break;
        }
    }

    // $C000-$C0FF (Memory.cpp IORead_C0xx / IOWrite_C0xx)
    uint8_t io(uint16_t a, const uint8_t* wr, uint64_t at) {
        switch ((a >> 4) & 0x0F) {
            case 0x0:                                   // keyboard data; IIe switches on write
                if (wr) { if (m_iie) iieSwitch(a & 0x0F, at); return 0; }
                return m_kbd->data();
            case 0x1:                                   // clear the strobe; IIe status
                if (wr || !m_iie) {
                    m_kbd->clearStrobe();
                    return wr ? 0 : floating(at);
                }
                if ((a & 0x0F) == 0) {                  // $C010: any key down
                    m_kbd->clearStrobe();
                    return (uint8_t)((m_kbd->data() & 0x7F) | (m_kbd->anyKeyDown() ? 0x80 : 0));
                }
                return status(a & 0x0F, at);
            case 0x2:                                   // cassette output (IIc: the ROM bank)
                if (m_iic) m_romSwitch = !m_romSwitch;
                return wr ? 0 : floating(at);
            case 0x3:                                   // speaker
                m_spk->toggle(at);
                return wr ? 0 : floating(at);
            case 0x4:
                if (m_iic) {
                    if ((a & 0x0F) == 0x8) { m_irqMask &= ~IRQ_MOUSE; return wr ? 0 : floating(at); }   // clear XY
                    if (!wr && (a & 0x0F) <= 3) {       // XYMask, VBL mask, X0Edge, Y0Edge
                        const bool v = (a & 3) == 0 ? m_xyMask : (a & 3) == 1 ? m_vblMask : (a & 3) == 2 ? m_x0Edge : m_y0Edge;
                        return (uint8_t)((floating(at) & 0x7F) | (v ? 0x80 : 0));
                    }
                }
                return wr ? 0 : floating(at);
            case 0x5: {
                const int s = a & 0x0F;
                if (m_iic && !m_iouDis && s >= 8) {     // $C058-$C05F: the IOU's interrupt switches
                    switch (s) {
                        case 0x8: m_xyMask = false; break;
                        case 0x9: m_xyMask = true; break;
                        case 0xA: m_irqMask &= ~IRQ_VBL; m_vblMask = false; break;
                        case 0xB: m_vblMask = true; break;
                        case 0xC: m_x0Edge = false; break;
                        case 0xD: m_x0Edge = true; break;
                        case 0xE: m_y0Edge = false; break;
                        case 0xF: m_y0Edge = true; break;
                    }
                    return wr ? 0 : floating(at);
                }
                switch (s) {
                    case 0x0: setVideo(0, Apple2Video::VF_TEXT, at); break;
                    case 0x1: setVideo(Apple2Video::VF_TEXT, 0, at); break;
                    case 0x2: setVideo(0, Apple2Video::VF_MIXED, at); break;
                    case 0x3: setVideo(Apple2Video::VF_MIXED, 0, at); break;
                    case 0x4: setVideo(0, Apple2Video::VF_PAGE2, at); break;
                    case 0x5: setVideo(Apple2Video::VF_PAGE2, 0, at); break;
                    case 0x6: setVideo(0, Apple2Video::VF_HIRES, at); break;
                    case 0x7: setVideo(Apple2Video::VF_HIRES, 0, at); break;
                    default:                            // annunciators 0-3
                        if (s & 1) m_annunciators |= (uint8_t)(1 << ((s - 8) >> 1));
                        else m_annunciators &= (uint8_t)~(1 << ((s - 8) >> 1));
                        // IIe: AN3 off ($C05E) is double hi-res on
                        if (m_iie && s == 0xE) setVideo(Apple2Video::VF_DHIRES, 0, at);
                        if (m_iie && s == 0xF) setVideo(0, Apple2Video::VF_DHIRES, at);
                        break;
                }
                return wr ? 0 : floating(at);
            }
            case 0x6: {                                 // cassette in, buttons, paddles (bit 3 ignored)
                if (wr) return 0;
                const uint8_t fb = floating(at) & 0x7F;
                if (m_iic) {
                    switch (a & 0x07) {
                        case 0x0: return fb;            // the 40/80 switch: 40 columns
                        case 0x3: return (uint8_t)(fb | 0x80);   // mouse button: up (inverted)
                        case 0x6: case 0x7: return fb;  // mouse X1, Y1
                    }
                }
                switch (a & 0x07) {
                    case 0x0: return fb;                // TAPEIN: no tape
                    case 0x1: case 0x2: case 0x3: {
                        const int b = (a & 7) - 1;
                        const bool down = m_button[b] || (m_iie && b < 2 && m_appleKey[b]);
                        return (uint8_t)(fb | (down ? 0x80 : 0));
                    }
                    default: {
                        const int p = (a & 7) - 4;
                        const bool active = (double)(at - m_ptrig) <= m_paddle[p] * (2816.0 / 255.0);
                        return (uint8_t)(fb | (active ? 0x80 : 0));
                    }
                }
            }
            case 0x7:                                   // paddle timers start
                m_ptrig = at;
                if (m_iic) {
                    m_irqMask &= ~IRQ_VBL;              // any $C07x clears the VBL interrupt
                    if (wr && (a & 0x0F) >= 8) m_iouDis = !(a & 1);   // $C078/A/C/E set IOUDIS, odd clear
                    if (!wr && (a & 0x0F) >= 8 && !(a & 1))
                        return (uint8_t)((floating(at) & 0x7F) | (m_iouDis ? 0x80 : 0));
                }
                if (wr) return 0;
                if (m_enhanced && (a & 0x0F) == 0xF)    // $C07F: double hi-res in bit 7
                    return (uint8_t)((floating(at) & 0x7F) | ((m_vf & Apple2Video::VF_DHIRES) ? 0x80 : 0));
                return floating(at);
            case 0x8: {                                 // the language card (LanguageCard.cpp IO)
                m_lcBank2 = !(a & 8);
                m_lcReadRam = ((a & 2) >> 1) == (a & 1);
                if (a & 1) {
                    // two reads in a row (UTAIIe:5-23), or a read-modify-write
                    // instruction's write after its read (GH#700)
                    const bool rmw = wr && m_lcLastRead == a && m_lcLastInstr == m_instr;
                    if ((!wr && m_lcPrewrite) || (wr && m_lcPrewrite && rmw)) m_lcWriteRam = true;
                } else {
                    m_lcWriteRam = false;
                }
                m_lcPrewrite = (a & 1) && !wr;
                if (!wr) { m_lcLastRead = a; m_lcLastInstr = m_instr; }
                return wr ? 0 : floating(at);
            }
            case 0x9: case 0xA:                         // IIc: the serial ports' ACIAs (slots 1, 2)
                if (m_iic && (a & 0x0F) >= 8 && (a & 0x0F) <= 0xB) {
                    Acia6551* acia = m_acia[((a >> 4) & 0xF) - 9];
                    if (acia) {
                        uint8_t v = wr ? *wr : 0;
                        const uint32_t reg = acia->baseAddr() + (a & 3);
                        if (wr) acia->ioWrite(this, reg, v);
                        else acia->ioRead(this, reg, &v);
                        return wr ? 0 : v;
                    }
                }
                return wr ? 0 : floating(at);
            case 0xE: {                                 // slot 6: the Disk II
                const int r = m_disk->io(a, wr, at);
                return wr ? 0 : r < 0 ? floating(at) : (uint8_t)r;
            }
            default:                                    // the other slots: empty
                return wr ? 0 : floating(at);
        }
    }

    BusConfig m_cfg;
    std::vector<uint8_t> m_rom, m_ram, m_lc, m_aux;
    Apple2Video* m_video;
    Apple2Keyboard* m_kbd;
    Apple2Speaker* m_spk;
    Disk2Card* m_disk;
    const bool m_iie, m_enhanced, m_iic;
    // IIc: the IOU and the ACIAs
    static constexpr uint8_t IRQ_VBL = 1, IRQ_MOUSE = 2;
    Acia6551* m_acia[2] = {nullptr, nullptr};
    bool m_iouDis = true, m_vblMask = false, m_xyMask = false, m_x0Edge = false, m_y0Edge = false;
    bool m_lastVbl = false, m_romSwitch = false;
    uint8_t m_irqMask = 0;
    uint32_t m_vf = Apple2Video::VF_TEXT;
    uint64_t m_cycles = 0, m_videoCycle = 0, m_instr = 0, m_ptrig = 0;
    int m_access = 0;
    bool m_lcReadRam = false, m_lcBank2 = true, m_lcWriteRam = true, m_lcPrewrite = false;
    bool m_80store = false, m_auxRead = false, m_auxWrite = false, m_intCxRom = false;
    bool m_altZp = false, m_slotC3Rom = false, m_intC8Rom = false, m_altChar = false;
    uint16_t m_lcLastRead = 0;
    uint64_t m_lcLastInstr = ~0ull;
    uint8_t m_annunciators = 0;
    int m_paddle[4] = {127, 127, 127, 127};
    bool m_button[3] = {};
    bool m_appleKey[2] = {};
};

}  // namespace

MachineDescriptor* create(Model model, std::string* err) {
    if (model == Model::IIgs) return createIIgs(err);
    const bool iic = model == Model::IIc;
    const bool iie = model == Model::IIe || model == Model::IIeEnhanced || iic;
    const bool enhanced = model == Model::IIeEnhanced || iic;
    std::vector<uint8_t> rom, chars, disk2;
    const char* romFile = model == Model::II ? "roms/apple2/Apple2.rom"
                        : model == Model::IIPlus ? "roms/apple2/Apple2_Plus.rom"
                        : model == Model::IIe ? "roms/apple2/Apple2e.rom"
                        : model == Model::IIc ? "roms/apple2/apple2c-ff.rom" : "roms/apple2/Apple2e_Enhanced.rom";
    if (iic) {
        // The IIc ROM is Apple's and not distributed here: 16 KB (ROM FF) or
        // 32 KB (03, 04); see doc/README-APPLE2.md for where to get it
        std::ifstream f(romFile, std::ios::binary);
        if (f) rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (rom.size() == 0x8000) {
            if (err) *err = "a 32 KB Apple IIc ROM (00, 03, 04) needs the 3.5\" drive or the memory expansion, "
                            "not emulated yet: use ROM FF (16 KB)";
            return nullptr;
        }
        if (rom.size() != 0x4000) {
            if (err) *err = std::string("missing Apple IIc ROM ") + romFile + " (local only; see doc/README-APPLE2.md)";
            return nullptr;
        }
    } else if (!loadFile(romFile, iie ? 0x4000 : 0x3000, rom, err)) return nullptr;
    // The IIe's 4 KB video ROM (the alternate set; MouseText on the enhanced
    // IIe only: the video leaves it out for the original IIe)
    if (!loadFile(iie ? "roms/apple2/Apple2e_Enhanced_Video.rom" : "roms/apple2/Apple2_Video.rom",
                  iie ? 0x1000 : 0x800, chars, err)) return nullptr;
    if (!loadFile("roms/apple2/DISK2.rom", 0x100, disk2, err)) return nullptr;
    std::vector<uint8_t> disk13;                       // DOS 3.2 (13 sectors), for WOZ disks that need it
    loadFile("roms/apple2/DISK2-13sector.rom", 0x100, disk13, nullptr);

    static const char* const IDS[] = {"apple2", "apple2plus", "apple2e", "apple2e-enhanced", "apple2c"};
    static const char* const NAMES[] = {"Apple ][", "Apple ][+", "Apple //e", "Apple //e (enhanced)", "Apple //c"};
    auto* desc = new MachineDescriptor();
    desc->machineId = IDS[(int)model];
    desc->displayName = NAMES[(int)model];
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"drive"};
    desc->clockHz = CPU_HZ;

    static const Apple2Video::Model VIDEO[] = {Apple2Video::Model::II, Apple2Video::Model::IIPlus,
                                               Apple2Video::Model::IIe, Apple2Video::Model::IIeEnhanced,
                                               Apple2Video::Model::IIeEnhanced};
    auto* video = new Apple2Video(VIDEO[(int)model]);
    video->setCharRom(std::move(chars));
    auto* kbd = new Apple2Keyboard(iie);
    auto* spk = new Apple2Speaker();
    auto* display = new Apple2Display(video);
    auto* disk = new Disk2Card(std::move(disk2), std::move(disk13));
    auto* bus = new Apple2Bus(std::move(rom), video, kbd, spk, disk, iie, enhanced, iic);
    desc->buses.push_back({"system", bus});
    // The enhanced IIe has a 65C02
    MOS6502* cpu = enhanced ? new MOS65C02() : new MOS6502();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(display);
    io->registerHandler(kbd);
    io->registerHandler(spk);
    io->registerHandler(disk);
    desc->deleters.push_back([video, display, kbd, spk, disk]() {
        delete display; delete video; delete kbd; delete spk; delete disk;
    });
    if (iic) {
        // The serial ports: 6551s (MAME: $C098 slot 1, $C0A8 slot 2)
        auto* a1 = new Acia6551(0xC090);
        auto* a2 = new Acia6551(0xC0A0);
        a1->setName("ACIA (port 1)");
        a2->setName("ACIA (port 2)");
        a1->setClockHz(CPU_HZ);
        a2->setClockHz(CPU_HZ);
        bus->setAcias(a1, a2);
        io->registerHandler(a1);
        io->registerHandler(a2);
        desc->deleters.push_back([a1, a2]() { delete a1; delete a2; });
        bus->onIrq = [cpu](bool on) { cpu->setIrqLine(on); };
    }

    // The RESET key (AppleWin CtrlReset): on a IIe the memory switches, the
    // language card and the video modes reset (a II/II+ keeps them); the
    // cards' RESET line (the Disk II's switches off); the CPU
    kbd->onReset = [cpu, disk, bus, kbd]() {
        bus->ctrlReset();
        disk->reset(false);
        kbd->clearStrobe();
        cpu->reset();
    };
    kbd->onAppleKey = [bus](int n, bool down) { bus->setAppleKey(n, down); };
    desc->onKey = [kbd](const std::string& n, bool down) { return kbd->key(n, down); };
    desc->screenButtons = {{"RESET", {"reset"}, iie ? "Control-RESET: the CPU restarts (the monitor warm-starts BASIC)."
                                                : "The RESET key: the CPU restarts (Applesoft's autostart monitor warm-starts)."}};

    // Joysticks: port 0 drives paddles 0/1 and button 0, port 1 paddles 2/3
    // and button 1 (bits active low: 0 up, 1 down, 2 left, 3 right, 4 fire)
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [bus](int port, uint8_t bits) {
        const int p = (port & 1) * 2;
        bus->setPaddle(p, !(bits & 0x04) ? 0 : !(bits & 0x08) ? 255 : 127);
        bus->setPaddle(p + 1, !(bits & 0x01) ? 0 : !(bits & 0x02) ? 255 : 127);
        bus->setButton(port & 1, !(bits & 0x10));
    };

    desc->onReset = [bus, io, cpu](MachineDescriptor&) {     // power on
        bus->powerOn();
        io->resetAll();
        cpu->reset();
    };
    desc->schedulerStep = [bus, cpu, io, disk](MachineDescriptor&) -> int {
        bus->begin();
        const int c = cpu->step();
        bus->end(c);
        disk->update(bus->cycles(), (uint64_t)c);
        io->tickAll((uint64_t)c);
        return c;
    };
    if (desc->onReset) desc->onReset(*desc);
    return desc;
}

}  // namespace apple2
