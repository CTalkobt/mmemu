// Commodore 128. Memory map, MMU and keyboard after VICE (src/c128/c128mem.c,
// c128meminit.c, c128mmu.c, data/C128/gtk3_sym.vkm), GPL-2.0-or-later,
// VICE team.

#include "machine_c128.h"
#include "c128_carts.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/isignal_line.h"
#include "libdevices/main/shared_irq_manager.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6510.h"
#include "plugins/z80/main/cpu_z80.h"
#include "plugins/devices/vic2/main/vic2.h"
#include "plugins/devices/sid6581/main/sid6581.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include "plugins/devices/vdc8563/main/vdc8563.h"
#include "plugins/devices/iec_drive/main/iec_drive.h"
#include "plugins/devices/datasette/main/datasette.h"
#include "plugins/devices/reu/main/reu.h"
#include "plugins/devices/georam/main/georam.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include "util/path_util.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <vector>

namespace c128 {
namespace {

using Rom = std::vector<uint8_t>;

bool loadRom(const std::string& rel, size_t size, Rom& out, std::string* err) {
    std::ifstream f(PathUtil::findResource(rel), std::ios::binary);
    if (!f) { if (err) *err = "missing ROM " + rel; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.size() != size) { if (err) *err = rel + ": not " + std::to_string(size / 1024) + " KB"; return false; }
    return true;
}

/** A level that reads from a function (the CAPS LOCK key on the 8502's bit 6). */
class FnLine : public ISignalLine {
public:
    explicit FnLine(std::function<bool()> get) : m_get(std::move(get)) {}
    bool get() const override { return m_get(); }
    void set(bool) override {}
    void pulse() override {}
private:
    std::function<bool()> m_get;
};

// ---------------------------------------------------------------------------
// The bus: MMU and memory map (c128mem.c, c128meminit.c, c128mmu.c)
// ---------------------------------------------------------------------------

class C128Bus : public IBus {
public:
    C128Bus(Rom basic, Rom kernal, Rom chargen, Rom c64)
        : m_basic(std::move(basic)), m_kernal(std::move(kernal)),
          m_chargen(std::move(chargen)), m_c64(std::move(c64)) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        for (size_t i = 0; i < sizeof m_ram; ++i) m_ram[i] = (i & 0x40) ? 0xFF : 0x00;
        std::memset(m_colorRam, 0x0F, sizeof m_colorRam);
        resetMmu();
    }

    void setCpu(MOS6510* cpu) { m_cpu = cpu; }
    /** A second SID fitted at run time (null: none); it answers its 32 bytes first. */
    void setSid2(IOHandler* sid2) { m_sid2 = sid2; }
    void setDevices(VIC2* vic, IOHandler* sid, IOHandler* cia1, IOHandler* cia2, Vdc8563* vdc) {
        m_vic = vic; m_sid = sid; m_cia1 = cia1; m_cia2 = cia2; m_vdc = vdc;
    }
    /** The 40/80 key (MCR bit 7 reads 1 while it is up: 40 columns). */
    void set4080Down(bool down) { m_key4080Down = down; }
    bool key4080Down() const { return m_key4080Down; }

    void resetMmu() {
        std::memset(m_mmu, 0, sizeof m_mmu);
        m_mmu[9] = 1;                                       // page 1 at $0100
        m_mmu[5] = 0x30;                                    // the Z80 runs first (mmu_reset)
        m_p0hLatch = m_p1hLatch = 0;
        updateColorRam();
    }
    uint8_t mmuReg(int i) const { return m_mmu[i]; }

    /** The function ROMs (VICE functionrom.c, c128cart.c): 32 KB each, the
     *  low half at $8000-$BFFF, the high half at $C000-$FFFF; internal (the
     *  socket on the board) when CR picks 1, external (a cartridge) for 2.
     *  Empty: none fitted (reads $FF). */
    void setFunctionRom(Rom rom, std::string label = "") {
        m_intFunc = std::move(rom);
        m_intLabel = std::move(label);
    }
    /** The cartridge at the expansion port (the external function ROM), or none. */
    void setCartridge(std::unique_ptr<c128::C128Cart> cart, std::string label = "") {
        m_cart = std::move(cart);
        m_cartLabel = m_cart ? std::move(label) : "";
    }
    c128::C128Cart* cartridge() const { return m_cart.get(); }
    /** Devices on the expansion port beside the cartridge (through an
     *  expander): 0 a RAM expansion (an REU, a GeoRAM), 1 an RS-232
     *  cartridge (a SwiftLink). They see I/O1, I/O2, $D700 and writes to
     *  $FF00 (the REU's trigger); null for none. */
    void setPortDevice(int n, IOHandler* dev) { m_port[n & 1] = dev; }
    const std::string& cartridgeLabel() const { return m_cartLabel; }
    const std::string& internalFunctionRomLabel() const { return m_intLabel; }
    uint8_t funcByte(int sel, uint16_t addr, bool peek = true) const {
        if (sel != 1) return m_cart ? m_cart->romRead(addr, peek) : 0xFF;
        return m_intFunc.empty() ? 0xFF : m_intFunc[addr & 0x7FFF];
    }
    /** MCR bit 3: the fast serial direction (set = the C128 sends, through
     *  CIA1's shift register onto the bus's DATA and SRQ). */
    bool fastSerialOut() const { return m_mmu[5] & 0x08; }
    /** MCR bit 0 clear: the Z80 runs, the 8502 waits. */
    bool z80Active() const { return !(m_mmu[5] & 0x01); }

    // -----------------------------------------------------------------------
    // The Z80's view (z80mem.c, C128 mode): as the 8502's map, except that in
    // bank 0 $0000-$0FFF reads the Z80 BIOS (the KERNAL ROM's $D000-$DFFF)
    // and writes RAM at $D000-$DFFF; with I/O in, $1000-$13FF is colour RAM;
    // $D000-$DFFF is never I/O (and the character ROM is not visible: RAM).
    // I/O is reached with IN/OUT: ports $D000-$DFFF are the chips (the MMU
    // only with I/O in), $0000-$0FFF RAM at $D000-$DFFF of bank 0, any other
    // port the memory at that address.
    // -----------------------------------------------------------------------
    uint8_t z80Read(uint16_t a, bool peek) {
        if (c64Mode()) return readC64(a, peek);
        const uint8_t cr = m_mmu[0];
        if (a < 0x1000) return bank() ? m_ram[ramAddr(a)] : m_kernal[0x1000 + a];
        if (a < 0x1400 && !(cr & 0x01)) return (uint8_t)(m_colorRam[(portBit(0) ? 0 : 0x400) + (a & 0x3FF)] | 0xF0);
        if (a >= 0xD000 && a < 0xE000) {
            const int hi = (cr >> 4) & 3;
            return (hi == 1 || hi == 2) ? funcByte(hi, a) : m_ram[ramAddr(a)];
        }
        return read(a, peek);
    }
    void z80Write(uint16_t a, uint8_t v) {
        if (c64Mode()) { writeC64(a, v); return; }
        const uint8_t cr = m_mmu[0];
        if (a < 0x1000 && !bank()) { m_ram[0xD000 | a] = v; return; }
        if (a >= 0x1000 && a < 0x1400 && !(cr & 0x01)) { m_colorRam[(portBit(0) ? 0 : 0x400) + (a & 0x3FF)] = v & 0x0F; return; }
        if (a >= 0xD000 && a < 0xE000) { m_ram[ramAddr(a)] = v; return; }
        write(a, v);
    }
    uint8_t z80In(uint16_t port, bool peek) {
        const uint8_t hi = port >> 8;
        if (hi < 0x10) return bank() ? z80Read(port, peek) : m_ram[0xD000 | (port & 0x0FFF)];
        if (hi == 0xD5) return (m_mmu[0] & 0x01) ? 0 : mmuPeek(port & 0xFF);
        if (hi >= 0xD0 && hi <= 0xDF) return io(port, nullptr, peek);
        return z80Read(port, peek);
    }
    void z80Out(uint16_t port, uint8_t v) {
        const uint8_t hi = port >> 8;
        if (hi < 0x10) { if (bank()) z80Write(port, v); else m_ram[0xD000 | (port & 0x0FFF)] = v; return; }
        if (hi == 0xD5) { if (!(m_mmu[0] & 0x01)) mmuStore(port & 0xFF, v); return; }
        if (hi >= 0xD0 && hi <= 0xDF) { io(port, &v, false); return; }
        z80Write(port, v);
    }

    /** The CPU port changed: colour RAM halves (bits 0 and 1) and the VIC's
     *  character ROM (bit 2) in C128 mode (mem_update_config). */
    void updateColorRam() {
        if (!m_vic) return;
        m_vic->setColorRam(&m_colorRam[c64Mode() || portBit(1) ? 0 : 0x400]);
    }

    /** What the VIC-II fetches: its RAM bank (RCR bit 6), and the C128
     *  character set at $x1000-$x1FFF of every 16 KB unless port bit 2 is set. */
    uint8_t vicFetch(uint32_t addr) const {
        addr &= 0xFFFF;
        if (c64Mode()) {                                    // the C64 set at $1000 / $9000
            if ((addr & 0x7000) == 0x1000) return m_chargen[addr & 0x0FFF];
        } else if (!portBit(2) && (addr & 0x3000) == 0x1000) {
            return m_chargen[0x1000 + (addr & 0x0FFF)];
        }
        return m_ram[(uint32_t)((m_mmu[6] & 0x40) ? 0x10000 : 0) + addr];
    }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        addr &= 0xFFFF;
        uint8_t v = read((uint16_t)addr, false);
        if (m_observer) m_observer->onMemoryRead(this, addr, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override { return read((uint16_t)(addr & 0xFFFF), true); }
    void write8(uint32_t addr, uint8_t val) override {
        addr &= 0xFFFF;
        uint8_t before = m_observer ? read((uint16_t)addr, true) : 0;
        write((uint16_t)addr, val);
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
    }

    size_t stateSize() const override { return sizeof m_ram + sizeof m_colorRam + sizeof m_mmu + 2; }
    void saveState(uint8_t* buf) const override {
        std::memcpy(buf, m_ram, sizeof m_ram); buf += sizeof m_ram;
        std::memcpy(buf, m_colorRam, sizeof m_colorRam); buf += sizeof m_colorRam;
        std::memcpy(buf, m_mmu, sizeof m_mmu); buf += sizeof m_mmu;
        buf[0] = m_p0hLatch; buf[1] = m_p1hLatch;
    }
    void loadState(const uint8_t* buf) override {
        std::memcpy(m_ram, buf, sizeof m_ram); buf += sizeof m_ram;
        std::memcpy(m_colorRam, buf, sizeof m_colorRam); buf += sizeof m_colorRam;
        std::memcpy(m_mmu, buf, sizeof m_mmu); buf += sizeof m_mmu;
        m_p0hLatch = buf[0]; m_p1hLatch = buf[1];
        updateColorRam();
    }
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    /** An 8502 port pin level, bits 0-2 (the 6510's LORAM/HIRAM/CHAREN lines).
     *  In C128 mode: bit 0 the CPU's colour RAM half, bit 1 the VIC's, bit 2
     *  set = no character ROM for the VIC (mem_update_config). */
    bool portBit(int bit) const {
        if (!m_cpu) return true;
        switch (bit) {
            case 0: return m_cpu->signalLoram()->get();
            case 1: return m_cpu->signalHiram()->get();
            case 2: return m_cpu->signalCharen()->get();
        }
        return true;
    }

    int bank() const { return (m_mmu[0] >> 6) & 1; }        // 128 KB: two banks

    /** Shared RAM limits (mem_set_ram_config). */
    uint32_t sharedSize() const { return (m_mmu[6] & 3) ? 0x1000u << ((m_mmu[6] & 3) - 1) : 0x400u; }
    bool bottomShared(uint16_t a) const { return (m_mmu[6] & 4) && a < sharedSize(); }
    bool topShared(uint16_t a) const { return (m_mmu[6] & 8) && a > 0xFFFF - sharedSize(); }

    /** Whether `page` of `pageBank` is RAM in the current configuration
     *  (mmu_is_valid_ram). */
    bool pageIsRam(uint8_t page, int pageBank) const {
        if (pageBank != bank()) return false;
        const uint8_t cr = m_mmu[0];
        if (page <= 0x3F) return true;
        if (page <= 0x7F) return cr & 0x02;
        if (page <= 0xBF) return ((cr >> 2) & 3) == 3;
        if (page >= 0xD0 && page <= 0xDF) return (cr & 0x01) && ((cr >> 4) & 3) == 3;
        return ((cr >> 4) & 3) == 3;
    }

    /** The physical RAM address of a CPU access that hits RAM: page 0/1
     *  relocation (c128_mem_mmu_wrap_read, C128 mode), shared RAM, the bank. */
    uint32_t ramAddr(uint16_t a) const {
        const uint8_t page = a >> 8, pos = a & 0xFF;
        const uint8_t p0 = m_mmu[7], p1 = m_mmu[9];
        const int p0b = m_mmu[8] & 1, p1b = m_mmu[10] & 1;
        if (!(p0 == 0 && p1 == 1 && p0b == 0 && p1b == 0)) {
            const bool zpShared = m_mmu[6] & 4;
            if (zpShared && page == 1) return (uint32_t)(p1 << 8 | pos);
            if (zpShared && page == 0) return (uint32_t)(p0 << 8 | pos);
            if (page == 1) return (uint32_t)(p1b << 16 | p1 << 8 | pos);
            if (page == p1 && pageIsRam(p1, p1b)) return (uint32_t)(p1b << 16 | 0x100 | pos);
            if (page == 0) return (uint32_t)(p0b << 16 | p0 << 8 | pos);
            if (page == p0 && pageIsRam(p0, p0b)) return (uint32_t)(p0b << 16 | pos);
        }
        if (page < 2) return a;                             // pages 0 and 1: bank 0
        if (bank() && (bottomShared(a) || topShared(a))) return a;
        return (uint32_t)(bank() << 16) + a;
    }

    // C64 mode (MCR bit 6): the C64's map from the 8502 port (no cartridge:
    // VICE's configurations 0-7, c64meminit.c) with BASIC and KERNAL 64 and
    // the C64 character set; RAM is the bank CR had, shared areas from bank
    // 0 (mem_initialize_go64_memory_bank); no MMU, the VDC stays at $D600.
    bool c64Mode() const { return m_mmu[5] & 0x40; }
    uint32_t c64RamAddr(uint16_t a) const {
        if (bank() && (bottomShared(a) || topShared(a))) return a;
        return (uint32_t)(bank() << 16) + a;
    }
    bool c64Io() const { return portBit(2) && (portBit(0) || portBit(1)); }
    uint8_t readC64(uint16_t addr, bool peek) {
        const bool lo = portBit(0), hi = portBit(1);
        if (addr >= 0xA000 && addr < 0xC000 && lo && hi) return m_c64[addr - 0xA000];
        if (addr >= 0xD000 && addr < 0xE000 && (lo || hi)) {
            if (portBit(2)) return io(addr, nullptr, peek);
            return m_chargen[addr & 0x0FFF];
        }
        if (addr >= 0xE000 && hi) return m_c64[0x2000 + (addr - 0xE000)];
        return m_ram[c64RamAddr(addr)];
    }
    void writeC64(uint16_t addr, uint8_t val) {
        if (addr >= 0xD000 && addr < 0xE000 && c64Io()) { io(addr, &val, false); return; }
        m_ram[c64RamAddr(addr)] = val;
    }

    uint8_t read(uint16_t addr, bool peek) {
        if (c64Mode()) return readC64(addr, peek);
        const uint8_t cr = m_mmu[0];
        if (addr >= 0xFF00 && addr <= 0xFF04) return m_mmu[addr & 0x0F];
        if (addr < 0x4000) return m_ram[ramAddr(addr)];
        if (addr < 0x8000) return (cr & 0x02) ? m_ram[ramAddr(addr)] : m_basic[addr - 0x4000];
        if (addr < 0xC000) {
            switch ((cr >> 2) & 3) {
                case 0: return m_basic[addr - 0x4000];
                case 3: return m_ram[ramAddr(addr)];
                default: return funcByte((cr >> 2) & 3, addr, peek);   // internal / external function ROM
            }
        }
        if (addr >= 0xD000 && addr < 0xE000 && !(cr & 0x01)) return io(addr, nullptr, peek);
        switch ((cr >> 4) & 3) {
            case 0:
                if (addr >= 0xD000 && addr < 0xE000) return m_chargen[0x1000 + (addr & 0x0FFF)];
                if ((addr == 0xFFFA || addr == 0xFFFB) && m_cart && m_cart->nmiVectorForced()) return 0xDE;
                return m_kernal[addr - 0xC000];             // editor $C000, KERNAL $E000
            case 3: return m_ram[ramAddr(addr)];
            default: return funcByte((cr >> 4) & 3, addr, peek);
        }
    }

    void write(uint16_t addr, uint8_t val) {
        if (addr == 0xFF00)                                 // the REU's trigger (beside the MMU's CR)
            for (IOHandler* d : m_port) if (d) d->ioWrite(this, 0xFF00, val);
        if (c64Mode()) { writeC64(addr, val); return; }
        if (addr >= 0xFF00 && addr <= 0xFF04) {             // mmu_ffxx_store
            mmuStore(0, addr == 0xFF00 ? val : m_mmu[addr & 0x0F]);
            return;
        }
        if (addr >= 0xD000 && addr < 0xE000 && !(m_mmu[0] & 0x01)) { io(addr, &val, false); return; }
        // The cartridge sees writes where CR maps it (GMod2's flash), and the
        // RAM underneath takes them too (VICE external_function_rom_store)
        if (m_cart && addr >= 0x8000 && ((addr < 0xC000 ? (m_mmu[0] >> 2) : (m_mmu[0] >> 4)) & 3) == 2)
            m_cart->romWrite(addr, val);
        m_ram[ramAddr(addr)] = val;                         // under ROM: RAM
    }

    /** $D000-$DFFF with I/O in; `wr` is the value written, or null for a read. */
    uint8_t io(uint16_t addr, const uint8_t* wr, bool peek) {
        uint8_t v = 0xFF;
        auto dev = [&](IOHandler* h, uint32_t a) {
            if (!h) return;
            if (wr)        h->ioWrite(this, a, *wr);
            else if (peek) h->ioPeek(this, a, &v);
            else           h->ioRead(this, a, &v);
        };
        if (m_sid2 && (addr & 0xFFE0) == m_sid2->baseAddr()) { dev(m_sid2, addr); return v; }
        switch (addr & 0xFF00) {
            case 0xD000: case 0xD100: case 0xD200: case 0xD300:
                dev(m_vic, 0xD000 | (addr & 0x3F)); break;
            case 0xD400:
                dev(m_sid, 0xD400 | (addr & 0x1F)); break;
            case 0xD500:
                if (c64Mode()) break;                       // no MMU in C64 mode (open)
                if (wr) mmuStore(addr & 0xFF, *wr);
                else v = mmuPeek(addr & 0xFF);
                break;
            case 0xD600:
                if (wr && m_cart) m_cart->iod6Write(addr, *wr);
                dev(m_vdc, addr); break;
            case 0xD800: case 0xD900: case 0xDA00: case 0xDB00: {
                uint8_t* cram = &m_colorRam[c64Mode() || portBit(0) ? 0 : 0x400];
                if (wr) cram[addr & 0x3FF] = *wr & 0x0F;
                else v = (uint8_t)(cram[addr & 0x3FF] | 0xF0);
                break;
            }
            case 0xDC00: dev(m_cia1, 0xDC00 | (addr & 0x0F)); break;
            case 0xDD00: dev(m_cia2, 0xDD00 | (addr & 0x0F)); break;
            case 0xDE00: case 0xDF00:                       // I/O1, I/O2: the cartridge
                if (portIo(addr, wr, peek, v)) break;      // the port's devices first; what they don't take goes on
                if (m_cart && !c64Mode()) {
                    uint8_t cv = 0xFF;
                    const bool drives = (addr & 0xFF00) == 0xDE00 ? m_cart->io1(addr, wr, peek, cv)
                                                                  : m_cart->io2(addr, wr, peek, cv);
                    if (drives && !wr) v = cv;
                }
                break;
            default:                                        // $D700: open, or a SwiftLink there
                portIo(addr, wr, peek, v);
                break;
        }
        return v;
    }

    uint8_t mmuPeek(uint8_t r) const {
        if (r >= 0x0C) return 0xFF;
        switch (r) {
            case 5:                                         // 40/80 up, no GAME/EXROM
                return (uint8_t)((m_mmu[5] & 0x0F) | (m_key4080Down ? 0 : 0x80) | 0x30);
            case 8: case 10: return m_mmu[r] | 0xF0;
            case 11: return 0x20;                           // version / 128 KB
            default: return m_mmu[r];
        }
    }

    void mmuStore(uint8_t r, uint8_t v) {
        if (r >= 0x0B) return;
        switch (r) {
            case 5: m_mmu[5] = (v & 0x7F) | 0x30; break;   // bit 0: the CPU (z80Active), bit 6: C64 mode
            case 8: m_p0hLatch = v; break;                  // committed by a P0L write
            case 10: m_p1hLatch = v; break;
            case 7: m_mmu[7] = v; m_mmu[8] = m_p0hLatch; break;
            case 9: m_mmu[9] = v; m_mmu[10] = m_p1hLatch; break;
            default: m_mmu[r] = v; break;
        }
    }

    BusConfig m_cfg;
    uint8_t m_ram[0x20000];
    uint8_t m_colorRam[0x800];
    Rom m_basic, m_kernal, m_chargen, m_c64;
    Rom m_intFunc;
    std::unique_ptr<c128::C128Cart> m_cart;
    IOHandler* m_port[2] = {nullptr, nullptr};
    /** The port devices' turn at an I/O access: true if one answered. */
    bool portIo(uint16_t addr, const uint8_t* wr, bool peek, uint8_t& v) {
        for (IOHandler* d : m_port) {
            if (!d) continue;
            if (wr ? d->ioWrite(this, addr, *wr) : peek ? d->ioPeek(this, addr, &v) : d->ioRead(this, addr, &v))
                return true;
        }
        return false;
    }
    std::string m_cartLabel, m_intLabel;
    uint8_t m_mmu[12];
    uint8_t m_p0hLatch = 0, m_p1hLatch = 0;
    bool m_key4080Down = false;
    MOS6510* m_cpu = nullptr;
    VIC2* m_vic = nullptr;
    IOHandler* m_sid = nullptr;
    IOHandler* m_sid2 = nullptr;
    IOHandler* m_cia1 = nullptr;
    IOHandler* m_cia2 = nullptr;
    Vdc8563* m_vdc = nullptr;
};

/** The VIC-II's view of memory (its DMA bus). */
/** A view of the C128 bus for the Z80: its memory (`io` false) or its
 *  IN/OUT ports (`io` true). */
class Z80Bus : public IBus {
public:
    Z80Bus(C128Bus* bus, bool io) : m_bus(bus), m_io(io) { m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF}; }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return m_io ? "z80io" : "z80"; }
    uint8_t read8(uint32_t a) override { return m_io ? m_bus->z80In((uint16_t)a, false) : m_bus->z80Read((uint16_t)a, false); }
    uint8_t peek8(uint32_t a) override { return m_io ? m_bus->z80In((uint16_t)a, true) : m_bus->z80Read((uint16_t)a, true); }
    void write8(uint32_t a, uint8_t v) override { if (m_io) m_bus->z80Out((uint16_t)a, v); else m_bus->z80Write((uint16_t)a, v); }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
private:
    C128Bus* m_bus;
    bool m_io;
    BusConfig m_cfg;
};

/** One device output wired to both CPUs' interrupt inputs. */
class FanoutLine : public ISignalLine {
public:
    FanoutLine(ISignalLine* a, ISignalLine* b) : m_a(a), m_b(b) {}
    bool get() const override { return m_a->get(); }
    void set(bool level) override { m_a->set(level); m_b->set(level); }
    void pulse() override { m_a->pulse(); m_b->pulse(); }
private:
    ISignalLine* m_a;
    ISignalLine* m_b;
};

class VicBus : public IBus {
public:
    explicit VicBus(C128Bus* bus) : m_bus(bus) { m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF}; }
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "vic"; }
    uint8_t read8(uint32_t a) override { return m_bus->vicFetch(a); }
    uint8_t peek8(uint32_t a) override { return m_bus->vicFetch(a); }
    void write8(uint32_t, uint8_t) override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
private:
    C128Bus* m_bus;
    BusConfig m_cfg;
};

// ---------------------------------------------------------------------------
// Keyboard: the C64's 8x8 matrix on CIA1 (port A selects, port B reads) plus
// three lines K0-K2 from VIC-IIe $D02F; CAPS LOCK on the 8502's bit 6 and
// 40/80 on the MMU (both locking). Positions from VICE's data/C128.
// ---------------------------------------------------------------------------

class C128Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    C128Keyboard() : m_portA(this, true), m_portB(this, false) {
        static const char* const names[11][8] = {
            {"delete", "return", "crsr_right", "f7", "f1", "f3", "f5", "crsr_down"},
            {"3", "w", "a", "4", "z", "s", "e", "left_shift"},
            {"5", "r", "d", "6", "c", "f", "t", "x"},
            {"7", "y", "g", "8", "b", "h", "u", "v"},
            {"9", "i", "j", "0", "m", "k", "o", "n"},
            {"plus", "p", "l", "minus", "period", "colon", "at", "comma"},
            {"pound", "asterisk", "semicolon", "home", "right_shift", "equal", "arrow_up", "slash"},
            {"1", "arrow_left", "ctrl", "2", "space", "cbm", "q", "run_stop"},
            {"help", "kp8", "kp5", "tab", "kp2", "kp4", "kp7", "kp1"},
            {"esc", "kp_plus", "kp_minus", "line_feed", "kp_enter", "kp6", "kp9", "kp3"},
            {"alt", "kp0", "kp_period", "up", "down", "left", "right", "no_scroll"},
        };
        for (int r = 0; r < 11; ++r)
            for (int c = 0; c < 8; ++c) m_map[names[r][c]] = {r, c};
        // Other names (the GUI's, VICE's host keys for the C128 keys)
        m_alias = {{"control", "ctrl"}, {"commodore", "cbm"}, {"shift", "left_shift"},
                   {"backspace", "delete"}, {"f9", "esc"}, {"f10", "tab"}, {"f11", "alt"},
                   {"end", "help"}, {"pagedown", "line_feed"}, {"uparrow", "arrow_up"},
                   {"leftarrow", "arrow_left"}, {"backslash", "arrow_left"}, {"kpeq", "equal"}};
        m_shifted = {{"exclaim", "1"}, {"dquote", "2"}, {"hash", "3"}, {"dollar", "4"}, {"percent", "5"},
                     {"ampersand", "6"}, {"squote", "7"}, {"lparen", "8"}, {"rparen", "9"},
                     {"question", "slash"}, {"less", "comma"}, {"greater", "period"},
                     {"bracket_left", "colon"}, {"bracket_right", "semicolon"},
                     {"f2", "f1"}, {"f4", "f3"}, {"f6", "f5"}, {"f8", "f7"}, {"insert", "delete"}};
        clearKeys();
    }

    void setVic(VIC2* vic) { m_vic = vic; }
    IPortDevice* portA() { return &m_portA; }
    IPortDevice* portB() { return &m_portB; }
    /** CAPS LOCK (ASCII/DIN): low while locked down. */
    bool capsLine() const { return !m_capsDown; }
    std::function<void(bool)> on4080;                       // the 40/80 key toggled

    /** Joystick 1 (CIA1 port B) / 2 (port A): active-low up, down, left, right, fire. */
    void setJoystick(int port, uint8_t bits) { if (port >= 0 && port < 2) m_joy[port] = (uint8_t)(bits | 0xE0); }

    bool key(const std::string& name, bool down) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (auto a = m_alias.find(n); a != m_alias.end()) n = a->second;
        if (n == "caps_lock" || n == "f12") { if (down) m_capsDown = !m_capsDown; return true; }
        if (n == "40_80" || n == "key_40_80") {
            if (down) { m_4080Down = !m_4080Down; if (on4080) on4080(m_4080Down); }
            return true;
        }
        auto it = m_map.find(n);
        if (it == m_map.end()) {
            auto s = m_shifted.find(n);
            if (s == m_shifted.end()) return false;
            set("left_shift", down);
            set(s->second, down);
            return true;
        }
        set(n, down);
        return true;
    }

    // IOHandler: ticks the typing queue
    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    // Keys held through a reset stay down (C= for C64 mode); only text being
    // typed is dropped
    void reset() override { m_queue.clear(); releaseTyped(); }
    void tick(uint64_t cycles) override {
        if (m_queue.empty()) return;
        if (m_timer > cycles) { m_timer -= cycles; return; }
        auto ev = m_queue.front();
        m_queue.pop_front();
        if (ev.first.empty()) releaseTyped();
        else {
            key(ev.first, ev.second);
            if (ev.second) m_typed.push_back(ev.first);
        }
        // A press is held ~100 ms (CP/M's keyboard scan misses shorter ones),
        // a release ~30 ms
        m_timer = ev.second ? 100000 : 30000;
    }

    // IKeyboardMatrix
    void keyDown(int row, int col) override { if (row >= 0 && row < 11) m_rows[row] &= (uint8_t)~(1 << col); }
    void keyUp(int row, int col) override { if (row >= 0 && row < 11) m_rows[row] |= (uint8_t)(1 << col); }
    void clearKeys() override { std::memset(m_rows, 0xFF, sizeof m_rows); }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    IPortDevice* getPort(int i) override { return i == 0 ? &m_portA : &m_portB; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            std::string name;
            if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'n' || text[i + 1] == 'r')) { ++i; name = "return"; }
            else if (c == '\n' || c == '\r') name = "return";
            else if (c >= 'A' && c <= 'Z') name = std::string(1, (char)std::tolower((unsigned char)c));   // plain key: capitals
            else if (c >= 'a' && c <= 'z') name = std::string(1, (char)std::toupper((unsigned char)c));   // shifted
            else if (std::isdigit((unsigned char)c)) name = std::string(1, c);
            else {
                static const std::map<char, const char*> sym = {
                    {' ', "space"}, {'!', "exclaim"}, {'"', "dquote"}, {'#', "hash"}, {'$', "dollar"}, {'%', "percent"},
                    {'&', "ampersand"}, {'\'', "squote"}, {'(', "lparen"}, {')', "rparen"}, {'*', "asterisk"},
                    {'+', "plus"}, {',', "comma"}, {'-', "minus"}, {'.', "period"}, {'/', "slash"}, {':', "colon"},
                    {';', "semicolon"}, {'<', "less"}, {'=', "equal"}, {'>', "greater"}, {'?', "question"}, {'@', "at"},
                    {'[', "bracket_left"}, {']', "bracket_right"}, {'^', "arrow_up"}, {'_', "arrow_left"}};
                auto it = sym.find(c);
                if (it == sym.end()) continue;
                name = it->second;
            }
            if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
                std::string lower(1, (char)std::tolower((unsigned char)name[0]));
                m_queue.push_back({"left_shift", true});
                m_queue.push_back({lower, true});
                m_queue.push_back({lower, false});
                m_queue.push_back({"left_shift", false});
            } else {
                m_queue.push_back({name, true});
                m_queue.push_back({name, false});
            }
            m_queue.push_back({"", false});
        }
    }

private:
    // CIA1 port A: the columns (rows here) the KERNAL selects, and joystick 2;
    // port B: the keys of the selected lines, and joystick 1
    class Port : public IPortDevice {
    public:
        Port(C128Keyboard* kbd, bool a) : m_kbd(kbd), m_a(a) {}
        uint8_t readPort() override { return m_kbd->pins(m_a); }
        void writePort(uint8_t v) override { m_pra = v; }
        void setDdr(uint8_t d) override { m_ddr = d; }
        uint8_t driven() const { return (uint8_t)((m_pra & m_ddr) | ~m_ddr); }
    private:
        C128Keyboard* m_kbd;
        bool m_a;
        uint8_t m_pra = 0xFF, m_ddr = 0;
    };

    uint8_t pins(bool portA) const {
        if (portA) return m_joy[1];                         // joystick 2
        uint8_t sel = m_portA.driven();                     // a low line selects a row
        uint8_t k = m_vic ? m_vic->keyboardLines() : 0x07;
        uint8_t v = 0xFF;
        for (int r = 0; r < 8; ++r) if (!(sel & (1 << r))) v &= m_rows[r];
        for (int r = 0; r < 3; ++r) if (!(k & (1 << r))) v &= m_rows[8 + r];
        return (uint8_t)(v & m_joy[0]);
    }

    /** Release the keys the typing queue pressed (not ones held by the user). */
    void releaseTyped() {
        for (const auto& k : m_typed) key(k, false);
        m_typed.clear();
    }

    void set(const std::string& n, bool down) {
        auto it = m_map.find(n);
        if (it == m_map.end()) return;
        if (down) keyDown(it->second.first, it->second.second);
        else      keyUp(it->second.first, it->second.second);
    }

    Port m_portA, m_portB;
    VIC2* m_vic = nullptr;
    std::map<std::string, std::pair<int, int>> m_map;
    std::map<std::string, std::string> m_alias, m_shifted;
    uint8_t m_rows[11];
    uint8_t m_joy[2] = {0xFF, 0xFF};
    bool m_capsDown = false, m_4080Down = false;
    std::deque<std::pair<std::string, bool>> m_queue;
    std::vector<std::string> m_typed;
    uint64_t m_timer = 0;
};

/**
 * The screen the frontends show: the VIC-II's 40 columns (scaled 2x) or the
 * VDC's 80, picked by the 40/80 key as VICE's single-window builds do
 * (c128mmu.c set_column4080_key). Both are 768 x 544 so the window keeps
 * its size.
 */
class C128Display : public IOHandler, public IVideoOutput {
public:
    C128Display(VIC2* vic, Vdc8563* vdc, const C128Bus* bus) : m_vic(vic), m_vdc(vdc), m_bus(bus) {}
    bool showsVdc() const { return m_bus->key4080Down(); }

    VideoDimensions getDimensions() const override { return m_vdc->getDimensions(); }
    void renderFrame(uint32_t* buffer) override {
        if (showsVdc()) { m_vdc->renderFrame(buffer); return; }
        const int w = VIC2::FRAME_W, h = VIC2::FRAME_H;
        m_vicFrame.resize((size_t)w * h);
        m_vic->renderFrame(m_vicFrame.data());
        for (int y = 0; y < Vdc8563::FRAME_H; ++y) {
            const uint32_t* src = &m_vicFrame[(size_t)std::min(y / 2, h - 1) * w];
            uint32_t* dst = &buffer[(size_t)y * Vdc8563::FRAME_W];
            for (int x = 0; x < Vdc8563::FRAME_W; ++x) dst[x] = src[std::min(x / 2, w - 1)];
        }
    }

    const char* name() const override { return "Display"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}

private:
    VIC2* m_vic;
    Vdc8563* m_vdc;
    const C128Bus* m_bus;
    std::vector<uint32_t> m_vicFrame;
};

C128Bus* c128Bus(MachineDescriptor* m) {
    if (!m) return nullptr;
    for (auto& b : m->buses)
        if (auto* p = dynamic_cast<C128Bus*>(b.bus)) return p;
    return nullptr;
}

}  // namespace

bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err) {
    auto* bus = c128Bus(m);
    if (!bus) { err = "Not a C128."; return false; }
    auto cart = c128::makeCart(image, err);
    if (!cart) return false;
    const std::string type = cart->type();
    bus->setCartridge(std::move(cart), label + " (" + type + ")");
    if (m->onReset) m->onReset(*m);
    return true;
}

void removeCartridge(MachineDescriptor* m) {
    auto* bus = c128Bus(m);
    if (!bus) return;
    bus->setCartridge(nullptr);
    if (m->onReset) m->onReset(*m);
}

std::string cartridgeInfo(MachineDescriptor* m) {
    auto* bus = c128Bus(m);
    return bus ? bus->cartridgeLabel() : "";
}

bool setInternalFunctionRom(MachineDescriptor* m, const std::vector<uint8_t>& image, std::string& err,
                            const std::string& label) {
    auto* bus = c128Bus(m);
    if (!bus) { err = "Not a C128."; return false; }
    if (image.empty()) { bus->setFunctionRom({}); return true; }
    Rom rom;
    std::string type;
    if (!c128::functionRomImage(image, rom, type, err)) return false;
    bus->setFunctionRom(std::move(rom), label.empty() ? type : label + " (" + type + ")");
    return true;
}

std::string internalFunctionRomInfo(MachineDescriptor* m) {
    auto* bus = c128Bus(m);
    return bus ? bus->internalFunctionRomLabel() : "";
}

MachineDescriptor* create(bool ntsc, std::string* err) {
    Rom basicLo, basicHi, kernal, chargen, c64;
    if (!loadRom("roms/c128/basic-4000.318018-04.bin", 0x4000, basicLo, err)) return nullptr;
    if (!loadRom("roms/c128/basic-8000.318019-04.bin", 0x4000, basicHi, err)) return nullptr;
    if (!loadRom("roms/c128/kernal.318020-05.bin", 0x4000, kernal, err)) return nullptr;
    if (!loadRom("roms/c128/characters.390059-01.bin", 0x2000, chargen, err)) return nullptr;
    if (!loadRom("roms/c128/c64.251913-01.bin", 0x4000, c64, err)) return nullptr;
    Rom basic = basicLo;
    basic.insert(basic.end(), basicHi.begin(), basicHi.end());

    const uint32_t clock = ntsc ? 1022727 : 985248;
    auto* desc = new MachineDescriptor();
    desc->machineId = ntsc ? "c128-ntsc" : "c128";
    desc->displayName = ntsc ? "Commodore 128 (NTSC)" : "Commodore 128";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"graphics-vic", "sidscope", "tape", "drive", "cartridge"};
    desc->graphicsFormats = {"sprite", "sprite-mc", "char", "char-mc"};
    desc->clockHz = clock;
    desc->screenButtons = {
        {"40/80", {"40_80"}, "The 40/80 DISPLAY key (it locks): down shows the VDC's 80 columns, up the VIC-II's 40. The screen editor reads it at reset to pick its screen."},
        {"RUN/STOP + RESTORE", {"run_stop", "restore"}, "Hold RUN/STOP and press RESTORE: the KERNAL's warm start."},
        {"RESTORE", {"restore"}, "RESTORE on its own: an NMI."},
        {"FREEZE", {"freeze"}, "The cartridge's freeze button (Partner 128)."}};

    desc->romSockets.push_back({"function",
        "The internal function ROM socket (U36): 32 KB at $8000-$FFFF when CR picks it; a raw 1-32 KB image or a C128 .crt",
        [desc](const std::vector<uint8_t>& image, const std::string& label, std::string& err) {
            return setInternalFunctionRom(desc, image, err, label);
        },
        [desc]() { return internalFunctionRomInfo(desc); }});

    auto* bus = new C128Bus(std::move(basic), std::move(kernal), std::move(chargen), std::move(c64));
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS6510();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    bus->setCpu(cpu);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});
    // The Z80: it runs first, from its boot BIOS, and hands over to the
    // 8502 through MCR bit 0 (CP/M switches back and forth)
    auto* z80 = new Z80();
    auto* z80Mem = new Z80Bus(bus, false);
    auto* z80Io = new Z80Bus(bus, true);
    z80->setDataBus(z80Mem);
    z80->setIoBus(z80Io);
    desc->cpus.push_back({"z80", z80, z80Mem, z80Mem, z80Io, false, 1});
    desc->deleters.push_back([z80Mem, z80Io]() { delete z80Mem; delete z80Io; });   // the CPU is the descriptor's

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    auto* vic = new VIC2();
    vic->setVicIIe(true);
    vic->setPal(!ntsc);
    auto* vicBus = new VicBus(bus);
    vic->setDmaBus(vicBus);
    auto* sid = new SID6581("SID", 0xD400);
    sid->setClockHz(clock);
    auto* cia1 = new CIA6526("CIA1", 0xDC00);
    auto* cia2 = new CIA6526("CIA2", 0xDD00);
    cia1->setClockHz(clock);
    cia2->setClockHz(clock);
    auto* vdc = new Vdc8563();
    vdc->setClockHz(clock);
    vdc->setPal(!ntsc);
    auto* kbd = new C128Keyboard();
    kbd->setVic(vic);
    bus->setDevices(vic, sid, cia1, cia2, vdc);

    // The 8502: CAPS LOCK on port bit 6; port bits 0-2 pick colour RAM and
    // the VIC's character ROM
    auto* capsLine = new FnLine([kbd]() { return kbd->capsLine(); });
    cpu->set8502(capsLine);
    kbd->on4080 = [bus](bool down) { bus->set4080Down(down); };

    // CIA1: keyboard and joysticks; CIA2: VIC bank, serial bus
    cia1->setPortADevice(kbd->portA());
    cia1->setPortBDevice(kbd->portB());
    cia2->setPortAWriteCallback([vic](uint8_t pra, uint8_t ddra) {
        uint8_t eff = (pra & ddra) | (uint8_t)~ddra;
        vic->setBankBase((uint32_t)(~eff & 0x03) * 0x4000);
    });
    auto* drive = new IecDrive(8);
    drive->setClockHz(clock);
    cia2->setPortADevice(drive);           // the serial bus on CIA2 port A
    // Fast serial: CIA1's shift register to and from a 1571 (MCR bit 3 sets
    // the direction)
    cia1->setSerialOut([bus, drive](uint8_t b) { if (bus->fastSerialOut()) drive->fastByteFromHost(b); });
    drive->setFastSerial([bus, cia1](uint8_t b) { if (!bus->fastSerialOut()) cia1->receiveSerialByte(b); });

    // The datasette on the 8502 port (motor, write, sense) and CIA1 FLAG
    auto* tape = new Datasette();
    tape->setName("Tape");
    tape->setMotorLine(cpu->signalCassetteMotor());
    tape->setWriteLine(cpu->signalCassetteWrite());
    tape->setSenseLine(cpu->signalCassetteSense());
    tape->setReadPulseLine(cia1->signalFlag());

    // The display first: the frontends show the first video output
    auto* display = new C128Display(vic, vdc, bus);
    for (IOHandler* h : std::initializer_list<IOHandler*>{display, vic, sid, cia1, cia2, vdc, kbd, drive, tape})
        io->registerHandler(h);

    // /IRQ goes to both CPUs
    auto* irq = new SharedIrqManager(cpu);
    auto* irqZ80 = new SharedIrqManager(z80);
    auto* vicIrq = new FanoutLine(irq->createLine(), irqZ80->createLine());
    auto* ciaIrq = new FanoutLine(irq->createLine(), irqZ80->createLine());
    vic->setIrqLine(vicIrq);
    cia1->setIrqLine(ciaIrq);
    desc->deleters.push_back([irqZ80, vicIrq, ciaIrq]() { delete irqZ80; delete vicIrq; delete ciaIrq; });
    auto* nmi = new SharedNmiManager(cpu);
    cia2->setIrqLine(nmi->createLine());
    ISignalLine* restore = nmi->createLine();

    desc->deleters.push_back([=]() {
        delete display; delete vic; delete sid; delete cia1; delete cia2; delete vdc; delete kbd;
        delete drive; delete tape; delete vicBus; delete capsLine;
        delete irq; delete nmi;
    });

    // A second SID (a stereo-SID cartridge or mod), mixed into the first
    {
        auto sid2 = std::make_shared<SID6581*>(nullptr);
        // (the first SID is gone by now: its deleter came first)
        desc->deleters.push_back([sid2, bus]() { bus->setSid2(nullptr); delete *sid2; *sid2 = nullptr; });
        MachineDescriptor::ExpansionSlot sl;
        sl.name = "sid2";
        sl.description = "A second SID (a stereo-SID cartridge or mod), mixed with the first; its address is the option";
        sl.devices = {{"sid", "A second 6581 SID: $D420 (the common mod), $D700, $DE00 or $DF00 (cartridges)",
                       {"D420", "D700", "DE00", "DF00"}, "D420"}};
        sl.install = [bus, io, sid, sid2, clock](const std::string& device, const std::string& option, std::string& err) {
            if (!device.empty() && device != "sid") { err = "no device '" + device + "' for the sid2 slot (sid)"; return false; }
            SID6581* fresh = nullptr;
            if (!device.empty()) {
                fresh = new SID6581("SID2", 0xD420);
                if (!fresh->setOption("base", option.empty() ? "D420" : option, err)) { delete fresh; return false; }
                if (fresh->baseAddr() >= 0xD500 && fresh->baseAddr() < 0xD700) { delete fresh; err = "$D500-$D6FF are the C128's MMU and VDC"; return false; }
                fresh->setClockHz(clock);
            }
            if (*sid2) {
                bus->setSid2(nullptr);
                sid->setLinkedDevice("mix", nullptr);
                io->unregisterHandler(*sid2);
                delete *sid2;
                *sid2 = nullptr;
            }
            if (!fresh) return true;
            *sid2 = fresh;
            io->registerHandler(fresh);             // it ticks with the machine
            bus->setSid2(fresh);
            sid->setLinkedDevice("mix", fresh);
            return true;
        };
        sl.info = [sid2]() -> std::string {
            if (!*sid2) return "";
            char b[8];
            std::snprintf(b, sizeof b, "%04X", (*sid2)->baseAddr());
            return std::string("sid (SID2, ") + b + ")";
        };
        desc->expansionSlots.push_back(std::move(sl));
    }

    // The expansion port's RAM expansions, fitted at run time: an REU (VICE
    // reu.c: I/O2, the $FF00 trigger, /IRQ to both CPUs, DMA through the
    // 8502's map) or a GeoRAM (VICE georam.c: a page at I/O1)
    {
        auto portIrq = std::make_shared<FanoutLine>(irq->createLine(), irqZ80->createLine());
        auto dev = std::make_shared<IOHandler*>(nullptr);
        desc->deleters.push_back([dev, bus]() { bus->setPortDevice(0, nullptr); delete *dev; *dev = nullptr; });
        MachineDescriptor::ExpansionSlot port;
        port.name = "port";
        port.description = "The expansion port: a RAM expansion beside any cartridge (an REU at $DF00, or a GeoRAM paged through $DE00)";
        port.devices = {{"reu", "Commodore REU: 128K (1700), 256K (1764), 512K (1750), or 1M-16M (1750 XL style); or a .reu image file",
                         {"128K", "256K", "512K", "1M", "2M", "4M", "8M", "16M"}, "512K"},
                        {"georam", "GeoRAM / NeoRAM: 512K-4M, a 256-byte page at $DE00 ($DFFE page, $DFFF block); or an image file",
                         {"512K", "1M", "2M", "4M"}, "512K"}};
        port.install = [desc, bus, io, dev, portIrq, vic](const std::string& device, const std::string& option, std::string& err) {
            auto remove = [&]() {
                if (!*dev) return;
                bus->setPortDevice(0, nullptr);
                io->unregisterHandler(*dev);
                desc->busMasterStep = nullptr;
                portIrq->set(false);
                delete *dev;
                *dev = nullptr;
            };
            if (device.empty()) { remove(); return true; }
            IOHandler* d = nullptr;
            if (device == "reu") d = new Reu();
            else if (device == "georam") d = new GeoRam();
            else { err = "no device '" + device + "' for the port slot (reu, georam)"; return false; }
            const std::string opt = option.empty() ? "512K" : option;
            std::ifstream probe(opt, std::ios::binary);
            if (!d->setOption(probe.good() ? "image" : "size", opt, err)) { delete d; return false; }
            remove();
            d->setName(device == "reu" ? "REU" : "GeoRAM");
            d->setDmaBus(bus);
            d->setIrqLine(portIrq.get());
            *dev = d;
            bus->setPortDevice(0, d);
            io->registerHandler(d);                 // reset, the debugger's device list
            if (device == "reu") { d->setBaSource(vic); desc->busMasterStep = [d]() { return d->busMasterCycle(); }; }
            return true;
        };
        port.info = [dev]() -> std::string {
            if (!*dev) return "";
            DeviceInfo di;
            (*dev)->getDeviceInfo(di);
            std::string size;
            for (const auto& kv : di.state) if (kv.first == "Size") size = kv.second;
            const std::string id = std::string((*dev)->name()) == "REU" ? "reu" : "georam";
            return id + " (" + (*dev)->name() + ", " + size + ")";
        };
        port.save = [dev](const std::string& path, std::string& err) {
            if (!*dev) { err = "nothing is fitted"; return false; }
            return (*dev)->saveDeviceImage(path, err);
        };
        desc->expansionSlots.push_back(std::move(port));
    }
    // An RS-232 cartridge on the port (VICE c64acia1.c): a 6551 at $DE00,
    // $DF00 or $D700 with doubled rates, on NMI
    {
        ISignalLine* aciaNmi = nmi->createLine();
        auto acia = std::make_shared<Acia6551*>(nullptr);
        desc->deleters.push_back([acia, bus]() { bus->setPortDevice(1, nullptr); delete *acia; *acia = nullptr; });
        MachineDescriptor::ExpansionSlot rs;
        rs.name = "rs232";
        rs.description = "An RS-232 cartridge on the expansion port (through a port expander, beside a RAM expansion); its far end: CLI serial";
        rs.devices = {{"swiftlink", "SwiftLink: a 6551 ACIA with a 3.6864 MHz crystal (up to 38400 bps), interrupting on NMI",
                       {"DE00", "DF00", "D700"}, "DE00"},
                      {"turbo232", "Turbo232: a SwiftLink with an enhanced control register for 57600, 115200 and 230400 bps",
                       {"DE00", "DF00", "D700"}, "DE00"}};
        rs.install = [bus, io, acia, aciaNmi, clock](const std::string& device, const std::string& option, std::string& err) {
            auto remove = [&]() {
                if (!*acia) return;
                bus->setPortDevice(1, nullptr);
                io->unregisterHandler(*acia);
                aciaNmi->set(false);
                delete *acia;
                *acia = nullptr;
            };
            if (device.empty()) { remove(); return true; }
            if (device != "swiftlink" && device != "turbo232") { err = "no device '" + device + "' for the rs232 slot (swiftlink, turbo232)"; return false; }
            auto* a = new Acia6551(0xDE00);
            a->setMode(device == "swiftlink" ? Acia6551::Mode::SwiftLink : Acia6551::Mode::Turbo232);
            if (!a->setOption("base", option.empty() ? "DE00" : option, err)) { delete a; return false; }
            remove();
            a->setName(device == "swiftlink" ? "SwiftLink" : "Turbo232");
            a->setClockHz(clock);
            a->setIrqLine(aciaNmi);
            *acia = a;
            bus->setPortDevice(1, a);
            io->registerHandler(a);                 // ticks, reset, CLI serial
            return true;
        };
        rs.info = [acia]() -> std::string {
            if (!*acia) return "";
            char b[8];
            std::snprintf(b, sizeof b, "%04X", (*acia)->baseAddr());
            return std::string((*acia)->mode() == Acia6551::Mode::SwiftLink ? "swiftlink" : "turbo232") + " (" + (*acia)->name() + ", " + b + ")";
        };
        desc->expansionSlots.push_back(std::move(rs));
    }

    // The cartridge's freeze button (Partner 128): NMI while it is held
    ISignalLine* freezeLine = nmi->createLine();
    desc->onKey = [kbd, restore, freezeLine, bus](const std::string& n, bool down) {
        std::string l = n;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (l == "restore") { restore->set(down); return true; }
        if (l == "freeze") {
            if (!down) { freezeLine->set(false); return true; }
            if (bus->cartridge() && bus->cartridge()->freeze()) freezeLine->set(true);
            return true;
        }
        return kbd->key(n, down);
    };
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 1;
    desc->onJoystick = [kbd](int port, uint8_t bits) { kbd->setJoystick(port, bits); };

    // Reset: the Z80 starts at $0000 (its BIOS); the 8502 takes its reset
    // vector when it first runs, after the BIOS has pointed the RAM vectors
    // at its start-up code (VICE's 8502 does the same)
    auto* fresh8502 = new bool(true);
    desc->deleters.push_back([fresh8502]() { delete fresh8502; });
    desc->onReset = [bus, io, cpu, z80, fresh8502, desc](MachineDescriptor&) {
        bus->resetMmu();
        if (bus->cartridge()) bus->cartridge()->reset();
        io->resetAll();
        z80->reset();
        *fresh8502 = true;
        bus->updateColorRam();
        desc->cpus[0].active = false;
        desc->cpus[1].active = true;
    };

    // The 8502 at 1 or 2 MHz ($D030 bit 0), the Z80 at an effective 2 MHz
    // (its 4 MHz clock waits for the VIC-II); time is counted at 1 MHz.
    // The VIC-IIe's badlines and sprite fetches take the bus from the 8502
    // at 1 MHz (#266): its read cycles wait while BA is low, its writes go
    // on. In FAST mode the VIC-IIe steals nothing (VICE vicii-badline.c,
    // vicii-fetch.c: no dma_maincpu_steal_cycles while fastmode) and the
    // screen is drawn as usual.
    auto* half = new int(0);
    desc->deleters.push_back([half]() { delete half; });
    desc->schedulerStep = [io, cpu, z80, vic, bus, half, fresh8502](MachineDescriptor& d) -> int {
        int c;
        const bool z = bus->z80Active();
        d.cpus[0].active = !z;
        d.cpus[1].active = z;
        if (d.busMasterStep && !z) {             // an REU's DMA holds the bus
            if (const int dc = d.busMasterStep()) { io->tickAll((uint64_t)dc); return dc; }
        }
        if (z) {
            *half += z80->step();
            c = *half / 2;
            *half %= 2;
        } else {
            if (*fresh8502) { *fresh8502 = false; cpu->reset(); }
            c = cpu->step();
            bus->updateColorRam();
            if (!vic->fastMode()) {
                const uint32_t writes = cpu->lastStepWrites();
                int t = 0;
                for (int i = 0; i < c; ++i) {
                    if (!(i < 32 && ((writes >> i) & 1)))
                        while (vic->baLow(t)) ++t;
                    ++t;
                }
                c = t;
            }
            if (vic->fastMode()) {
                *half += c;
                c = *half / 2;
                *half %= 2;
            }
        }
        if (c) io->tickAll((uint64_t)c);
        return c;
    };
    return desc;
}

}  // namespace c128
