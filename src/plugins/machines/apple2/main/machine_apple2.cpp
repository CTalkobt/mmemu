// Apple II / II+ machine, after AppleWin (GPL-2.0-or-later): Memory.cpp
// (the $C0xx soft switches, the floating bus), LanguageCard.cpp,
// Keyboard.cpp, Speaker.cpp, Joystick.cpp.

#include "machine_apple2.h"
#include "smartport_bus.h"
#include "mockingboard.h"
#include "super_serial.h"
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
        m_auxBank = 0;                          // a power cycle: bank 0 (AppleWin GH#1414)
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
    /** A card in `slot` (null takes it out); a sound card's output joins the speaker's. */
    void setCard(int slot, SlotCard* card) {
        m_card[slot & 7] = card;
        if (m_c8Slot == (slot & 7)) m_c8Slot = 0;
        std::vector<SlotCard*> boards;
        for (SlotCard* c : m_card) if (c && c->hasSound()) boards.push_back(c);
        if (boards.empty()) { m_spk->setMix(nullptr); return; }
        m_spk->setMix([boards](uint64_t until) {
            float v = 0;
            for (SlotCard* c : boards) v += c->sample(until);
            return v;
        });
    }
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
            // The mouse moves a step an axis a scanline (MAME update_iic_mouse)
            const uint64_t scan = m_cycles / Apple2Video::CYCLES_PER_LINE;
            for (uint64_t l = m_mouseLine; l < scan && (m_mouseDx || m_mouseDy); ++l) mouseStep();
            m_mouseLine = scan;
            m_lastVbl = vbl;
            bool line = m_irqMask != 0;
            for (Acia6551* a : m_acia) if (a && a->irqAsserted()) line = true;
            if (onIrq) onIrq(line);
        } else if (onIrq) {
            bool line = false;                          // the cards' /IRQ (a Mockingboard's VIAs)
            for (SlotCard* c : m_card) if (c && c->irq()) line = true;
            if (line != m_cardIrq) { m_cardIrq = line; onIrq(line); }
        }
    }
    uint64_t cycles() const { return m_cycles; }

    // Paddles (Joystick.cpp): $C07x starts the timers; paddle n reads bit 7
    // set for its position x 11.04 cycles
    void setPaddle(int n, int pos) { m_paddle[n & 3] = std::clamp(pos, 0, 255); }
    void setButton(int n, bool down) { m_button[n % 3] = down; }
    // IIe: Open Apple and Closed Apple are buttons 0 and 1
    void setAppleKey(int n, bool down) { m_appleKey[n & 1] = down; }
    // IIc: the mouse, moves to step through and the button
    void mouse(int dx, int dy, bool button) { m_mouseDx += dx; m_mouseDy += dy; m_mouseButton = button; }

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
            if (src == CX_CARD) { const int v = m_card[(a >> 8) & 7]->pagePeek((uint8_t)a); return v < 0 ? m_video->floatingBus() : (uint8_t)v; }
            if (src == CX_CARD_ROM) return m_card[m_c8Slot]->expansionRom()[a - 0xC800];
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
        else if (a < 0xD000) cx(a, at, &v);
        else if (m_lcWriteRam) {
            if (m_altZp) auxWrite(auxLcIndex(a), v);
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

    // The IIe's aux slot: the extended 80-column card (one 64 KB bank) or a
    // RamWorks III (MAME a2eramworks3.cpp, AppleWin Memory.cpp): banks of
    // 64 KB, contiguous from 0, a write to $C071/3/5/7 picks one; the video
    // always shows bank 0
    void setAuxBanks(int banks) {
        m_aux.assign((size_t)banks * 0x10000u, 0);
        m_auxBank = 0;
        m_video->setMemory(m_ram.data(), m_aux.data());
    }
    int auxBanks() const { return (int)(m_aux.size() / 0x10000u); }
    int auxBank() const { return m_auxBank; }
    uint32_t videoFlags() const { return m_vf; }
    bool lcReadRam() const { return m_lcReadRam; }
    bool lcWriteRam() const { return m_lcWriteRam; }
    bool lcBank2() const { return m_lcBank2; }

private:
    enum { CX_FLOATING, CX_INTERNAL, CX_DISK, CX_CARD, CX_CARD_ROM };

    void resetPaging() {
        // LanguageCardUnit::kMemModeInitialState: ROM read, bank 2, write
        // enabled; every other switch off
        m_lcReadRam = false; m_lcBank2 = true; m_lcWriteRam = true; m_lcPrewrite = false;
        m_80store = m_auxRead = m_auxWrite = m_intCxRom = m_altZp = m_slotC3Rom = m_intC8Rom = false;
        m_altChar = false;
        // IIc IOU (MAME machine_reset): IOUDIS on, interrupts off, ROM bank 0
        m_iouDis = true;
        m_vblMask = m_xyMask = m_x0Edge = m_y0Edge = false;
        m_x1 = m_y1 = m_xIrq = m_yIrq = false;
        m_mouseDx = m_mouseDy = 0;
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
    uint8_t ramRead(uint16_t a) const { return auxFor(a, false) ? auxRead(a) : m_ram[a]; }
    void ramWrite(uint16_t a, uint8_t v) {
        if (auxFor(a, true)) auxWrite(a, v);
        else m_ram[a] = v;
    }
    // The aux card's selected 64 KB bank (a RamWorks III's banks past the
    // fitted memory float: reads see the bus, writes are lost)
    uint8_t auxRead(uint32_t a) const {
        const size_t i = (size_t)m_auxBank * 0x10000u + a;
        return i < m_aux.size() ? m_aux[i] : m_video->floatingBus();
    }
    void auxWrite(uint32_t a, uint8_t v) {
        const size_t i = (size_t)m_auxBank * 0x10000u + a;
        if (i < m_aux.size()) m_aux[i] = v;
    }

    // $D000-$FFFF: ROM or the language card's RAM (aux with ALTZP)
    uint8_t highRead(uint16_t a) const {
        if (!m_lcReadRam) return m_rom[romBank() + a - romBase()];
        return m_altZp ? auxRead(auxLcIndex(a)) : m_lc[lcIndex(a)];
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
            if (m_card[slot]) return CX_CARD;
            return slot == 6 ? CX_DISK : CX_FLOATING;
        }
        if (m_iic || (m_iie && (m_intCxRom || m_intC8Rom))) return CX_INTERNAL;
        // The card whose $Cn00 page was touched last (I/O STROBE)
        if (m_c8Slot && m_card[m_c8Slot] && m_card[m_c8Slot]->expansionRom()) return CX_CARD_ROM;
        return CX_FLOATING;
    }
    uint8_t cx(uint16_t a, uint64_t at, const uint8_t* wr = nullptr) {
        if (m_iie) {
            if (a == 0xCFFF) m_intC8Rom = false;
            else if (a >= 0xC300 && a < 0xC400 && !m_slotC3Rom) m_intC8Rom = true;
        }
        const int src = cxSource(a);
        if (a == 0xCFFF) m_c8Slot = 0;                  // every card lets go of $C800
        if (src == CX_CARD) m_c8Slot = (a >> 8) & 7;
        if (src == CX_INTERNAL) return m_rom[romBank() + a - 0xC000];
        if (src == CX_DISK) return m_disk->rom((uint8_t)a);
        if (src == CX_CARD) {                           // a card's $Cn00 page (a Mockingboard's VIAs, a ROM)
            const int v = m_card[(a >> 8) & 7]->pageIo((uint8_t)a, wr, at);
            return wr ? 0 : v < 0 ? floating(at) : (uint8_t)v;
        }
        if (src == CX_CARD_ROM) return wr ? 0 : m_card[m_c8Slot]->expansionRom()[a - 0xC800];
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
        if (m_iic && (n == 0x5 || n == 0x7)) {         // mouse X0 / Y0 interrupt flags (the IRQ released)
            m_irqMask &= ~IRQ_MOUSE;
            const bool f = n == 0x5 ? m_xIrq : m_yIrq;
            return (uint8_t)((m_kbd->data() & 0x7F) | (f ? 0x80 : 0));
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
                    if ((a & 0x0F) == 0x8) { m_irqMask &= ~IRQ_MOUSE; m_xIrq = m_yIrq = false; return wr ? 0 : floating(at); }   // clear XY
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
                        case 0x3: return (uint8_t)(fb | (m_mouseButton ? 0 : 0x80));   // mouse button (inverted)
                        case 0x6: return (uint8_t)(fb | (m_x1 ? 0x80 : 0));    // mouse X1: the direction
                        case 0x7: return (uint8_t)(fb | (m_y1 ? 0x80 : 0));    // mouse Y1
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
                if (wr && m_iie && !m_iic && auxBanks() > 1 && (a & 0x9) == 1)
                    m_auxBank = *wr;                    // RamWorks III: the bank
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
            case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xF:
                if (!m_iic) {                           // a card's $C0n0-$C0nF
                    SlotCard* c = m_card[((a >> 4) & 0xF) - 8];
                    if (!c) return wr ? 0 : floating(at);
                    const int v = c->deviceIo(a & 0x0F, wr, at);
                    return wr ? 0 : v < 0 ? floating(at) : (uint8_t)v;
                }
                if ((a & 0xF0) == 0xC0) {               // IIc ROMs 03, 04: the memory expansion
                    if (m_memExp.empty() || (a & 0x0F) > 3) return wr ? 0 : floating(at);
                    return memExp(a & 3, wr);
                }
                if ((a & 0xF0) != 0x90 && (a & 0xF0) != 0xA0) return wr ? 0 : floating(at);
                // IIc: the serial ports' ACIAs (slots 1, 2)
                if ((a & 0x0F) >= 8 && (a & 0x0F) <= 0xB) {
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
                if (m_iwm) return iwm(a, wr, at);
                const int r = m_disk->io(a, wr, at);
                return wr ? 0 : r < 0 ? floating(at) : (uint8_t)r;
            }
            default:                                    // the other slots: empty
                return wr ? 0 : floating(at);
        }
    }

    // The IWM in slot 6 (the IIc's 32 KB ROMs, as MAME's apple2c_iwm): the
    // Disk II emulation behind its mode, status and handshake registers, and
    // the SmartPort bus (phases 1 and 3, ENABLE2) answering as with nothing
    // fitted (KEGS iwm.c)
    uint8_t iwm(uint16_t a, const uint8_t* wr, uint64_t at) {
        const int loc = a & 0x0F;
        const bool on = loc & 1;
        if (loc < 8) { if (on) m_iwmPhases |= 1u << (loc >> 1); else m_iwmPhases &= ~(1u << (loc >> 1)); }
        // SmartPort devices on the disk port (UniDisk 3.5 and the like): the
        // bus sees the phases; enabled, it has the IWM's data and sense line
        if (m_spBus) {
            if (loc < 8) m_spBus->phases(m_iwmPhases, at);
            else if (m_spBus->active()) return iwmBus(loc, wr, at);
        }
        const bool enable2 = (m_iwmPhases & 0xA) == 0xA && loc >= 8;
        const int r = enable2 ? -1 : m_disk->io(a, wr, at);
        switch (loc >> 1) {
            case 4: m_iwmMotor = on; break;
            case 6: m_iwmQ6 = on; break;
            case 7: m_iwmQ7 = on; break;
        }
        if (wr) {
            if (on && m_iwmQ6 && m_iwmQ7 && !m_iwmMotor) m_iwmMode = *wr & 0x1F;
            return 0;
        }
        if (on) return 0;
        switch ((m_iwmQ7 ? 2 : 0) | (m_iwmQ6 ? 1 : 0)) {
            case 0: return enable2 ? 0xFF : r >= 0 ? (uint8_t)r : 0xFF;
            case 1: return (uint8_t)(((enable2 || r < 0) ? 0x80 : (r & 0x80)) | (m_iwmMotor ? 0x20 : 0) | m_iwmMode);
            case 2:
                if (enable2) { if (++m_enable2Hs > 3) { m_enable2Hs = 0; return 0x80; } return 0xC0; }
                return 0x80;
            default: return 0;
        }
    }
    // The IWM with the SmartPort bus enabled: data to and from the units,
    // ACK on the sense line (status bit 7)
    uint8_t iwmBus(int loc, const uint8_t* wr, uint64_t at) {
        const bool on = loc & 1;
        switch (loc >> 1) {
            case 4: m_iwmMotor = on; break;
            case 6: m_iwmQ6 = on; break;
            case 7: m_iwmQ7 = on; break;
        }
        if (wr) {
            if (on && m_iwmQ6 && m_iwmQ7) {
                if (m_iwmMotor) m_spBus->write(*wr, at);
                else m_iwmMode = *wr & 0x1F;
            }
            m_spBus->setMode(m_iwmMode);
            return 0;
        }
        if (on) return 0;
        switch ((m_iwmQ7 ? 2 : 0) | (m_iwmQ6 ? 1 : 0)) {
            case 0: return m_iwmMotor ? m_spBus->read(at) : 0xFF;
            case 1: return (uint8_t)((m_spBus->ack(at) ? 0x80 : 0) | (m_iwmMotor ? 0x20 : 0) | m_iwmMode);
            case 2: return m_spBus->handshake(at);
            default: return 0;
        }
    }
    // The IIc memory expansion (MAME apple2e.cpp memexp_r / memexp_w):
    // $C0C0-$C0C2 a 24-bit pointer (the top byte reads with $F0 ORed in),
    // $C0C3 the data, the pointer stepping on
    uint8_t memExp(int r, const uint8_t* wr) {
        auto regs = [&]() {
            m_expRegs[0] = (uint8_t)m_expLive;
            m_expRegs[1] = (uint8_t)(m_expLive >> 8);
            m_expRegs[2] = (uint8_t)((m_expLive >> 16) | 0xF0);
        };
        if (wr) {
            if (r < 3) {
                const int sh = 8 * r;
                m_expWptr = (m_expWptr & ~(0xFFu << sh)) | ((uint32_t)*wr << sh);
                m_expLive = m_expWptr;
            } else {
                if (m_expLive < m_memExp.size()) m_memExp[m_expLive] = *wr;
                ++m_expLive;
            }
            regs();
            return 0;
        }
        if (r < 3) return m_expRegs[r];
        const uint8_t v = m_expLive < m_memExp.size() ? m_memExp[m_expLive] : 0xFF;
        ++m_expLive;
        regs();
        return v;
    }

public:
    /** The IIc's 32 KB ROMs: the IWM in slot 6 and (03, 04) the memory expansion. */
    void setIwm(bool on) { m_iwm = on; }
    void setSmartPortBus(SmartPortBus* b) { m_spBus = b; }
    void setMemoryExpansion(size_t bytes) { m_memExp.assign(bytes, 0); }

private:
    SmartPortBus* m_spBus = nullptr;
    bool m_iwm = false, m_iwmMotor = false, m_iwmQ6 = false, m_iwmQ7 = false;
    uint8_t m_iwmMode = 0;
    uint32_t m_iwmPhases = 0;
    int m_enable2Hs = 0;
    std::vector<uint8_t> m_memExp;
    uint32_t m_expWptr = 0, m_expLive = 0;
    uint8_t m_expRegs[4] = {0, 0, 0xF0, 0};

    BusConfig m_cfg;
    std::vector<uint8_t> m_rom, m_ram, m_lc, m_aux;
    int m_auxBank = 0;
    Apple2Video* m_video;
    Apple2Keyboard* m_kbd;
    Apple2Speaker* m_spk;
    Disk2Card* m_disk;
    const bool m_iie, m_enhanced, m_iic;
    // IIc: the IOU and the ACIAs
    static constexpr uint8_t IRQ_VBL = 1, IRQ_MOUSE = 2;
    Acia6551* m_acia[2] = {nullptr, nullptr};
    SlotCard* m_card[8] = {};
    int m_c8Slot = 0;                           // the card owning $C800-$CFFF (0: none)
    bool m_cardIrq = false;
    bool m_iouDis = true, m_vblMask = false, m_xyMask = false, m_x0Edge = false, m_y0Edge = false;
    bool m_lastVbl = false, m_romSwitch = false;
    // IIc mouse (MAME apple2e.cpp update_iic_mouse): each step toggles X0/Y0,
    // X1/Y1 give the direction; an interrupt on X0/Y0's chosen edge
    int m_mouseDx = 0, m_mouseDy = 0;
    bool m_x0 = false, m_x1 = false, m_y0 = false, m_y1 = false, m_xIrq = false, m_yIrq = false, m_mouseButton = false;
    uint64_t m_mouseLine = 0;
    void mouseStep() {
        bool irq = false;
        if (m_mouseDx) {
            if (m_mouseDx < 0) { ++m_mouseDx; m_x1 = false; } else { --m_mouseDx; m_x1 = true; }
            if ((m_x0 && m_x0Edge) || (!m_x0 && !m_x0Edge))        // falling / rising, as chosen
                if (m_xyMask) { m_xIrq = true; irq = true; }
            m_x0 = !m_x0;
        }
        if (m_mouseDy) {
            if (m_mouseDy < 0) { ++m_mouseDy; m_y1 = true; } else { --m_mouseDy; m_y1 = false; }
            if ((m_y0 && m_y0Edge) || (!m_y0 && !m_y0Edge))
                if (m_xyMask) { m_yIrq = true; irq = true; }
            m_y0 = !m_y0;
        }
        if (irq) m_irqMask |= IRQ_MOUSE;
    }
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
    // The IIc's later ROMs: the IIc, with its ROM file and profile
    const char* iicRom = model == Model::IIcRom00 ? "roms/apple2/apple2c-00.rom"
                       : model == Model::IIcRom03 ? "roms/apple2/apple2c-03.rom"
                       : model == Model::IIcRom04 ? "roms/apple2/apple2c-04.rom" : nullptr;
    const char* iicId = model == Model::IIcRom00 ? "apple2c-rom00" : model == Model::IIcRom03 ? "apple2c-rom03" : "apple2c-rom04";
    const char* iicName = model == Model::IIcRom00 ? "Apple //c (ROM 00)" : model == Model::IIcRom03 ? "Apple //c (ROM 03)" : "Apple //c (ROM 04)";
    if (iicRom) model = Model::IIc;
    if (model == Model::IIgs) return createIIgs(err, 1);
    if (model == Model::IIgsRom03) return createIIgs(err, 3);
    const bool iic = model == Model::IIc;
    const bool iie = model == Model::IIe || model == Model::IIeEnhanced || iic;
    const bool enhanced = model == Model::IIeEnhanced || iic;
    std::vector<uint8_t> rom, chars, disk2;
    const char* romFile = model == Model::II ? "roms/apple2/Apple2.rom"
                        : model == Model::IIPlus ? "roms/apple2/Apple2_Plus.rom"
                        : model == Model::IIe ? "roms/apple2/Apple2e.rom"
                        : model == Model::IIc ? (iicRom ? iicRom : "roms/apple2/apple2c-ff.rom") : "roms/apple2/Apple2e_Enhanced.rom";
    if (iic) {
        // The IIc ROM is Apple's and not distributed here: 16 KB (ROM FF) or
        // 32 KB (03, 04); see doc/README-APPLE2.md for where to get it
        std::ifstream f(romFile, std::ios::binary);
        if (f) rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (rom.size() != 0x4000 && rom.size() != 0x8000) {
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
    desc->machineId = iicRom ? iicId : IDS[(int)model];
    desc->displayName = iicRom ? iicName : NAMES[(int)model];
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
    // The 32 KB ROMs (as MAME's apple2c0, apple2c3, apple2c4): the IWM in
    // slot 6; ROMs 03 and 04 ($FBBF, the version byte, 3 or more) also the
    // 1 MB memory expansion
    const bool rom32k = iic && rom.size() == 0x8000;
    const bool memExp = rom32k && rom[0x3BBF] >= 3 && rom[0x3BBF] != 0xFF;
    auto* bus = new Apple2Bus(std::move(rom), video, kbd, spk, disk, iie, enhanced, iic);
    if (rom32k) bus->setIwm(true);
    if (memExp) bus->setMemoryExpansion(1 << 20);
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
    if (rom32k) {
        // The disk port's SmartPort bus (UniDisk 3.5s, hard disks), as the
        // 32 KB ROMs drive it
        auto* spBus = new SmartPortBus(CPU_HZ / 1e6);
        bus->setSmartPortBus(spBus);
        io->registerHandler(spBus);
        desc->deleters.push_back([spBus]() { delete spBus; });
    }
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
        // The mouse: moves in picture pixels (560 wide), halved to the
        // 280-dot screen
        desc->onMouse = [bus, fx = 0.0, fy = 0.0](int dx, int dy, uint8_t buttons, int) mutable {
            fx += dx * 0.5;
            fy += dy * 0.5;
            const int ix = (int)fx, iy = (int)fy;
            fx -= ix;
            fy -= iy;
            bus->mouse(ix, iy, buttons & 1);
        };
    }

    if (iie && !iic) {
        // The aux slot: the extended 80-column card, or a RamWorks III of
        // 128 KB to 16 MB (AppleWin's limit: 256 banks; MAME's 8 MB)
        MachineDescriptor::ExpansionSlot aux;
        aux.name = "aux";
        aux.description = "The IIe's auxiliary slot: the extended 80-column card (64 KB) or an Applied Engineering RamWorks III (banks of 64 KB at $C073)";
        aux.canEmpty = false;
        aux.devices = {{"ext80", "Apple extended 80-column card: 64 KB, one bank", {}, ""},
                       {"ramworks3", "Applied Engineering RamWorks III: 128 KB-16 MB in 64 KB banks",
                        {"256K", "512K", "1M", "2M", "3M", "4M", "8M", "16M"}, "1M"}};
        aux.install = [bus](const std::string& device, const std::string& option, std::string& err) {
            if (device.empty()) { err = "the aux slot can't be left empty (an empty slot isn't emulated)"; return false; }
            if (device == "ext80") {
                if (!option.empty()) { err = "the extended 80-column card takes no option"; return false; }
                bus->setAuxBanks(1);
                return true;
            }
            if (device != "ramworks3") { err = "no device '" + device + "' for the aux slot (ext80, ramworks3)"; return false; }
            const uint64_t bytes = MachineDescriptor::parseMemorySize(option.empty() ? "1M" : option);
            if (bytes < 0x20000 || bytes > (16u << 20) || bytes % 0x10000) {
                err = "a RamWorks III takes 128K to 16M in 64K steps (e.g. 512K, 1M, 8M)";
                return false;
            }
            bus->setAuxBanks((int)(bytes / 0x10000));
            return true;
        };
        aux.info = [bus]() {
            const int b = bus->auxBanks();
            if (b <= 1) return std::string("ext80 (extended 80-column card, 64 KB)");
            const uint32_t kb = (uint32_t)b * 64;
            return "ramworks3 (RamWorks III, " + (kb % 1024 ? std::to_string(kb) + " KB" : std::to_string(kb / 1024) + " MB") + ")";
        };
        desc->expansionSlots.push_back(std::move(aux));
    }

    // /IRQ: the IIc's IOU and ACIAs; the cards' on the others
    bus->onIrq = [cpu](bool on) { cpu->setIrqLine(on); };

    if (!iic) {
        // Peripheral slots taking a card at run time: a Mockingboard (4 and
        // 5 are AppleWin's usual places; two give twelve voices) or a Super
        // Serial Card (2 by convention). Slot 6 is the Disk II's; 3 the
        // IIe's 80-column firmware's.
        for (int slotNo : {1, 2, 4, 5, 7}) {
            auto card = std::make_shared<SlotCard*>(nullptr);
            desc->deleters.push_back([card, bus, slotNo]() { bus->setCard(slotNo, nullptr); delete *card; *card = nullptr; });
            MachineDescriptor::ExpansionSlot sl;
            const std::string n = std::to_string(slotNo);
            sl.name = "slot" + n;
            sl.description = "Peripheral slot " + n + ": $C0" + std::to_string(8 + slotNo) + "0-$C0" + std::to_string(8 + slotNo) +
                             "F, $C" + n + "00-$C" + n + "FF";
            sl.devices = {{"mockingboard", "Sweet Micro Systems Mockingboard: two 6522 VIAs and two AY-3-8910s (six voices, stereo mixed to mono), /IRQ from the VIAs", {}, ""},
                          {"ssc", "Apple Super Serial Card: a 6551 and its firmware (PR#/IN#); the DIP switches' baud rate as the option; its far end: CLI serial",
                           {"300", "1200", "2400", "9600", "19200"}, "9600"}};
            sl.install = [bus, io, card, slotNo](const std::string& device, const std::string& option, std::string& err) {
                SlotCard* fresh = nullptr;
                if (device == "mockingboard") {
                    if (!option.empty()) { err = "the Mockingboard takes no option"; return false; }
                    fresh = new Mockingboard(slotNo);
                } else if (device == "ssc") {
                    fresh = SuperSerialCard::create(slotNo, option.empty() ? 9600 : std::atoi(option.c_str()), err);
                    if (!fresh) return false;
                } else if (!device.empty()) {
                    err = "no device '" + device + "' for this slot (mockingboard, ssc)";
                    return false;
                }
                if (*card) {
                    bus->setCard(slotNo, nullptr);
                    io->unregisterHandler(*card);
                    delete *card;
                    *card = nullptr;
                }
                if (!fresh) return true;
                *card = fresh;
                io->registerHandler(fresh);         // it ticks with the machine; CLI serial finds an SSC
                bus->setCard(slotNo, fresh);
                return true;
            };
            sl.info = [card]() -> std::string {
                if (!*card) return "";
                if (auto* ssc = dynamic_cast<SuperSerialCard*>(*card))
                    return "ssc (Super Serial Card, " + std::to_string(ssc->baud()) + " bps)";
                return "mockingboard (Mockingboard)";
            };
            desc->expansionSlots.push_back(std::move(sl));
        }
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
