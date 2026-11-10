// The Game Boy's memory map, timer, OAM DMA, joypad and serial port, after
// Mesen2 (Core/Gameboy/Gameboy.cpp, GbMemoryManager.cpp, GbTimer.cpp,
// GbDmaController.cpp, GbControlManager.cpp), GPL-3.0, Sour and the Mesen
// contributors.

#include "gameboy.h"

#include <algorithm>
#include <cstring>

namespace gb {

namespace {

// SameBoy's open source SGB and SGB2 boot ROMs (LIJI32, MIT licence:
// https://github.com/LIJI32/SameBoy/tree/master/BootROMs), as Mesen embeds them
// (Core/Gameboy/GbBootRom.h). Like Nintendo's they send the cartridge's header to
// the SNES in joypad packets.
const uint8_t SGB_BOOT[256] = {
    0x31, 0xFE, 0xFF, 0x21, 0x00, 0x80, 0x22, 0xCB, 0x6C, 0x28, 0xFB, 0x3E,
    0x80, 0xE0, 0x26, 0xE0, 0x11, 0x3E, 0xF3, 0xE0, 0x12, 0xE0, 0x25, 0x3E,
    0x77, 0xE0, 0x24, 0x3E, 0x00, 0xE0, 0x47, 0x11, 0x04, 0x01, 0x21, 0x10,
    0x80, 0x1A, 0x47, 0xCD, 0xC9, 0x00, 0xCD, 0xC9, 0x00, 0x13, 0x7B, 0xEE,
    0x34, 0x20, 0xF2, 0x11, 0xEA, 0x00, 0x0E, 0x08, 0x1A, 0x13, 0x22, 0x23,
    0x0D, 0x20, 0xF9, 0x3E, 0x19, 0xEA, 0x10, 0x99, 0x21, 0x2F, 0x99, 0x0E,
    0x0C, 0x3D, 0x28, 0x08, 0x32, 0x0D, 0x20, 0xF9, 0x2E, 0x0F, 0x18, 0xF5,
    0x3E, 0x91, 0xE0, 0x40, 0x3E, 0xF1, 0xE0, 0x80, 0x21, 0x04, 0x01, 0xAF,
    0x4F, 0xAF, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xCD, 0xB7, 0x00, 0xE5,
    0x06, 0x0E, 0x16, 0x00, 0xCD, 0xAD, 0x00, 0x82, 0x57, 0x05, 0x20, 0xF8,
    0xCD, 0xB7, 0x00, 0xE1, 0x06, 0x0E, 0xCD, 0xAD, 0x00, 0xCD, 0xB7, 0x00,
    0x05, 0x20, 0xF7, 0x3E, 0x20, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xC6,
    0x02, 0xE0, 0x80, 0x3E, 0x58, 0xBD, 0x20, 0xC9, 0x0E, 0x13, 0x3E, 0xC1,
    0xE2, 0x0C, 0x3E, 0x07, 0xE2, 0x3E, 0xFC, 0xE0, 0x47, 0x3E, 0x01, 0x21,
    0x60, 0xC0, 0xC3, 0xF2, 0x00, 0x3E, 0x4F, 0xBD, 0x38, 0x02, 0x2A, 0xC9,
    0x23, 0xAF, 0xC9, 0x5F, 0x16, 0x08, 0x3E, 0x10, 0xCB, 0x1B, 0x38, 0x01,
    0x87, 0xE2, 0x3E, 0x30, 0xE2, 0x15, 0xC8, 0x18, 0xF1, 0x3E, 0x04, 0x0E,
    0x00, 0xCB, 0x20, 0xF5, 0xCB, 0x11, 0xF1, 0xCB, 0x11, 0x3D, 0x20, 0xF5,
    0x79, 0x22, 0x23, 0x22, 0x23, 0xC9, 0xE5, 0x21, 0x0F, 0xFF, 0xCB, 0x86,
    0xCB, 0x46, 0x28, 0xFC, 0xE1, 0xC9, 0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5,
    0x42, 0x3C, 0x16, 0xE1, 0x1E, 0xAA, 0x1B, 0x14, 0x15, 0x20, 0xFB, 0x5A,
    0xEE, 0x00, 0xE0, 0x50,
};
const uint8_t SGB2_BOOT[256] = {
    0x31, 0xFE, 0xFF, 0x21, 0x00, 0x80, 0x22, 0xCB, 0x6C, 0x28, 0xFB, 0x3E,
    0x80, 0xE0, 0x26, 0xE0, 0x11, 0x3E, 0xF3, 0xE0, 0x12, 0xE0, 0x25, 0x3E,
    0x77, 0xE0, 0x24, 0x3E, 0x00, 0xE0, 0x47, 0x11, 0x04, 0x01, 0x21, 0x10,
    0x80, 0x1A, 0x47, 0xCD, 0xC9, 0x00, 0xCD, 0xC9, 0x00, 0x13, 0x7B, 0xEE,
    0x34, 0x20, 0xF2, 0x11, 0xEA, 0x00, 0x0E, 0x08, 0x1A, 0x13, 0x22, 0x23,
    0x0D, 0x20, 0xF9, 0x3E, 0x19, 0xEA, 0x10, 0x99, 0x21, 0x2F, 0x99, 0x0E,
    0x0C, 0x3D, 0x28, 0x08, 0x32, 0x0D, 0x20, 0xF9, 0x2E, 0x0F, 0x18, 0xF5,
    0x3E, 0x91, 0xE0, 0x40, 0x3E, 0xF1, 0xE0, 0x80, 0x21, 0x04, 0x01, 0xAF,
    0x4F, 0xAF, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xCD, 0xB7, 0x00, 0xE5,
    0x06, 0x0E, 0x16, 0x00, 0xCD, 0xAD, 0x00, 0x82, 0x57, 0x05, 0x20, 0xF8,
    0xCD, 0xB7, 0x00, 0xE1, 0x06, 0x0E, 0xCD, 0xAD, 0x00, 0xCD, 0xB7, 0x00,
    0x05, 0x20, 0xF7, 0x3E, 0x20, 0xE2, 0x3E, 0x30, 0xE2, 0xF0, 0x80, 0xC6,
    0x02, 0xE0, 0x80, 0x3E, 0x58, 0xBD, 0x20, 0xC9, 0x0E, 0x13, 0x3E, 0xC1,
    0xE2, 0x0C, 0x3E, 0x07, 0xE2, 0x3E, 0xFC, 0xE0, 0x47, 0x3E, 0xFF, 0x21,
    0x60, 0xC0, 0xC3, 0xF2, 0x00, 0x3E, 0x4F, 0xBD, 0x38, 0x02, 0x2A, 0xC9,
    0x23, 0xAF, 0xC9, 0x5F, 0x16, 0x08, 0x3E, 0x10, 0xCB, 0x1B, 0x38, 0x01,
    0x87, 0xE2, 0x3E, 0x30, 0xE2, 0x15, 0xC8, 0x18, 0xF1, 0x3E, 0x04, 0x0E,
    0x00, 0xCB, 0x20, 0xF5, 0xCB, 0x11, 0xF1, 0xCB, 0x11, 0x3D, 0x20, 0xF5,
    0x79, 0x22, 0x23, 0x22, 0x23, 0xC9, 0xE5, 0x21, 0x0F, 0xFF, 0xCB, 0x86,
    0xCB, 0x46, 0x28, 0xFC, 0xE1, 0xC9, 0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5,
    0x42, 0x3C, 0x16, 0xE1, 0x1E, 0xAA, 0x1B, 0x14, 0x15, 0x20, 0xFB, 0x5A,
    0xEE, 0x00, 0xE0, 0x50,
};

}  // namespace

Gameboy::Gameboy() {
    m_cpu.attach(this, &m_ppu);
    m_ppu.attach(this);
    m_apu.attach(this);
}

Gameboy::~Gameboy() = default;

bool Gameboy::load(std::vector<uint8_t> rom, std::string& err) {
    if (rom.size() < 0x150) { err = "Game Boy ROM too small (" + std::to_string(rom.size()) + " bytes)"; return false; }
    if (rom.size() & 0x3FFF) rom.resize((rom.size() + 0x3FFF) & ~(size_t)0x3FFF, 0);   // whole 16 KB banks, as Mesen
    m_header = Header::parse(rom);
    auto cart = Cart::create(m_header, rom, err);
    if (!cart) return false;
    m_rom = std::move(rom);
    m_cart = std::move(cart);
    m_cart->attach(this);
    m_cartRam.assign(m_header.cartRamSize(), 0);
    return true;
}

void Gameboy::powerOn(SgbLink* sgb) {
    m_sgb = sgb;
    // Mesen's Init order: the timer, the APU, the cartridge, the memory map, the CPU, the PPU, DMA
    m_div = 0x06;                                        // Mesen's (boot_div-dmgABCmgb)
    m_timerBit = 1024;
    m_needReload = m_reloaded = m_timerOn = false;
    m_tima = m_tma = m_tac = 0;
    m_apuCycles = 0;
    m_apu.init();
    m_wram.assign(0x2000, 0);
    std::memset(m_hram, 0, sizeof(m_hram));
    std::memset(m_reads, 0, sizeof(m_reads));
    std::memset(m_writes, 0, sizeof(m_writes));
    std::memset(m_readReg, 0, sizeof(m_readReg));
    std::memset(m_writeReg, 0, sizeof(m_writeReg));
    m_ie = m_if = 0;
    m_serialData = m_serialControl = m_serialBits = 0;
    m_serialOut.clear();
    m_joySelect = 0x30;
    if (!m_bootRomOverride.empty()) m_bootRom = m_bootRomOverride;
    else if (m_model == Model::Sgb) m_bootRom.assign(SGB_BOOT, SGB_BOOT + 256);
    else if (m_model == Model::Sgb2) m_bootRom.assign(SGB2_BOOT, SGB2_BOOT + 256);
    else m_bootRom.clear();
    m_bootRomOff = m_bootRom.empty();
    mapRegisters(0x8000, 0x9FFF, ACC_RW);
    mapRegisters(0xFE00, 0xFFFF, ACC_RW);
    if (m_cart) m_cart->initCart();
    refreshMappings();
    m_cpu.init();
    m_ppu.init();
    std::memset(m_ppu.vram(), 0, 0x2000);
    std::memset(m_ppu.oam(), 0, 0xA0);
    m_dmaSource = 0xFF;
    m_dmaStartDelay = m_dmaDest = m_dmaCounter = m_dmaBuffer = 0;
    m_dmaRunning = false;
    if (m_bootRomOff) postBootState();
    m_cpu.powerOn();
}

/** Where the DMG boot ROM leaves things (Pan Docs' "Power Up Sequence"), for a start without one. */
void Gameboy::postBootState() {
    auto& s = m_cpu.state();
    s.a = 0x01; s.f = 0xB0; s.b = 0x00; s.c = 0x13; s.d = 0x00; s.e = 0xD8; s.h = 0x01; s.l = 0x4D;
    s.sp = 0xFFFE; s.pc = 0x0100;
    // The sound: the boot ROM's chime on square 1 (NR52 $F1)
    write(0xFF26, 0x80);
    write(0xFF11, 0x80);
    write(0xFF12, 0xF3);
    write(0xFF25, 0xF3);
    write(0xFF24, 0x77);
    write(0xFF13, 0xC1);
    write(0xFF14, 0x87);
    write(0xFF47, 0xFC);
    write(0xFF40, 0x91);
    m_serialControl = 0;                                 // SC reads $7E
    m_div = 0xABCC;
    m_tac = 0;                                           // TAC reads $F8
    m_if = 0x01;
}

void Gameboy::runTo(uint64_t cycle) {
    if (!m_cart) return;
    while (m_cpu.cycles() < cycle) m_cpu.exec();
}

void Gameboy::runFrame() {
    if (!m_cart) return;
    const uint32_t f = m_ppu.frameCount();
    while (f == m_ppu.frameCount()) m_cpu.exec();
    m_apu.run();
}

void Gameboy::setButtons(uint8_t buttons) {
    inputChange([&]() { m_buttons = buttons; });
}

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------

std::vector<uint8_t>* Gameboy::memory(Mem type) {
    switch (type) {
    case Mem::Rom: return &m_rom;
    case Mem::WorkRam: return &m_wram;
    case Mem::CartRam: return &m_cartRam;
    case Mem::BootRom: return &m_bootRom;
    default: return nullptr;
    }
}

void Gameboy::map(uint16_t start, uint16_t end, Mem type, uint32_t offset, bool readOnly) {
    auto* mem = memory(type);
    if (!mem || mem->empty()) { unmap(start, end); return; }
    const uint32_t size = (uint32_t)mem->size();
    offset %= size;
    for (int i = start; i < end; i += 0x100) {
        uint8_t* p = mem->data() + offset;
        m_reads[i >> 8] = p;
        m_writes[i >> 8] = readOnly ? nullptr : p;
        offset += 0x100;
        if (offset >= size) offset = 0;
    }
}

void Gameboy::unmap(uint16_t start, uint16_t end) {
    for (int i = start; i < end; i += 0x100) m_reads[i >> 8] = m_writes[i >> 8] = nullptr;
}

void Gameboy::mapRegisters(uint16_t start, uint16_t end, uint8_t access) {
    for (int i = start; i < end; i += 0x100) {
        m_readReg[i >> 8] = (access & ACC_READ) != 0;
        m_writeReg[i >> 8] = (access & ACC_WRITE) != 0;
    }
}

void Gameboy::refreshMappings() {
    map(0xC000, 0xCFFF, Mem::WorkRam, 0, false);
    map(0xD000, 0xDFFF, Mem::WorkRam, 0x1000, false);
    map(0xE000, 0xEFFF, Mem::WorkRam, 0, false);
    map(0xF000, 0xFFFF, Mem::WorkRam, 0x1000, false);
    if (m_cart) m_cart->refreshMappings();
    if (!m_bootRomOff) map(0x0000, 0x00FF, Mem::BootRom, 0, true);
}

// ---------------------------------------------------------------------------
// Time (Mesen's GbMemoryManager::Exec / ExecMasterCycle)
// ---------------------------------------------------------------------------

void Gameboy::timerDmaSerial() {
    m_apuCycles += 2;
    timerExec();
    if ((m_cpu.cycles() & 3) == 0) dmaExec();
    if (m_serialBits && (m_cpu.cycles() & 0x1FF) == 0) {
        // The internal clock shifts a bit out (and a 1 in: nothing is connected)
        m_serialData = (uint8_t)(m_serialData << 1 | 1);
        if (--m_serialBits == 0) {
            m_serialControl &= 0x7F;
            requestIrq(IRQ_SERIAL);
        }
    }
}

void Gameboy::execMasterCycle() {
    uint64_t& c = m_cpu.cycleRef();
    ++c;
    if (c & 1) timerDmaSerial();
    m_ppu.exec(true);
}

void Gameboy::exec() {
    m_cpu.cycleRef() += 2;
    timerDmaSerial();
    m_ppu.exec(false);
}

// ---------------------------------------------------------------------------
// Reads and writes
// ---------------------------------------------------------------------------

template <OamCorruption C> uint8_t Gameboy::read(uint16_t addr) {
    if (m_dmaRunning && dmaConflict(addr)) addr = dmaReadAddress();   // the CPU sees what DMA reads
    if (m_readReg[addr >> 8]) {
        m_ppu.oamCorruption<C>(addr);
        return readRegister(addr);
    }
    if (m_reads[addr >> 8]) return m_reads[addr >> 8][addr & 0xFF];
    return 0;
}

void Gameboy::write(uint16_t addr, uint8_t v) {
    if (m_dmaRunning && dmaConflict(addr)) return;
    if (m_writeReg[addr >> 8]) {
        m_ppu.oamCorruption<OamCorruption::Write>(addr);
        writeRegister(addr, v);
    } else if (m_writes[addr >> 8]) {
        m_writes[addr >> 8][addr & 0xFF] = v;
    }
}

void Gameboy::cpuWrite(uint16_t addr, uint8_t v) {
    if (addr == 0xFF47) {
        // BGP takes effect a little earlier than other writes (ppu_scanline_bgp, m3_bgp_change)
        execMasterCycle();
        execMasterCycle();
        execMasterCycle();
        write(addr, v);
        execMasterCycle();
        return;
    }
    exec();
    exec();
    write(addr, v);
}

uint8_t Gameboy::peek(uint16_t addr) {
    if (m_readReg[addr >> 8]) {
        if (addr >= 0xFF40) return readRegister(addr);
        if (addr >= 0xFE00 && addr < 0xFEA0) return m_ppu.readOam((uint8_t)addr);
        if (addr >= 0xFF10 && addr < 0xFF40) return m_apu.peek(addr);
        if (addr >= 0x8000 && addr < 0xA000) return m_ppu.readVram(addr);
        if (addr >= 0xFF00 && addr < 0xFF10 && addr != 0xFF00) return readRegister(addr);
        if (addr == 0xFF00) return readJoypad();
        return 0xFF;
    }
    return m_reads[addr >> 8] ? m_reads[addr >> 8][addr & 0xFF] : 0;
}

void Gameboy::poke(uint16_t addr, uint8_t v) {
    if (!m_writeReg[addr >> 8] && m_writes[addr >> 8]) m_writes[addr >> 8][addr & 0xFF] = v;
}

uint8_t Gameboy::pendingIrq() const {
    const uint8_t r = m_if & m_ie & 0x1F;
    if (!r) return 0;
    return (uint8_t)(r & -r);     // the lowest bit: VBlank first
}

uint8_t Gameboy::readRegister(uint16_t addr) {
    if (addr >= 0xFF00) {
        if (addr == 0xFFFF) return m_ie;
        if (addr == 0xFF46) return m_dmaSource;
        if (addr >= 0xFF80) return m_hram[addr & 0x7F];
        if (addr >= 0xFF4C) return 0xFF;                 // the CGB's registers, and nothing
        if (addr >= 0xFF40) return m_ppu.read(addr);
        if (addr >= 0xFF10) return m_apu.read(addr);
        switch (addr) {
        case 0xFF00: return readJoypad();
        case 0xFF01: return m_serialData;
        case 0xFF02: return (uint8_t)(m_serialControl | 0x7E);
        case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07: return timerRead(addr);
        case 0xFF0F: return (uint8_t)(m_if | 0xE0);
        default: return 0xFF;
        }
    }
    if (addr >= 0xFE00) return m_ppu.readOam((uint8_t)addr);
    if (addr >= 0x8000 && addr < 0xA000) return m_ppu.readVram(addr);
    return m_cart ? m_cart->readRegister(addr) : 0xFF;
}

void Gameboy::writeRegister(uint16_t addr, uint8_t v) {
    if (addr >= 0xFF00) {
        if (addr == 0xFFFF) { m_ie = v; return; }
        if (addr == 0xFF46) { m_dmaStartDelay = 1; m_dmaSource = v; return; }
        if (addr >= 0xFF80) { m_hram[addr & 0x7F] = v; return; }
        if (addr >= 0xFF4C) {
            if (addr == 0xFF50 && (v & 1) && !m_bootRomOff) { m_bootRomOff = true; refreshMappings(); }
            return;
        }
        if (addr >= 0xFF40) { m_ppu.write(addr, v); return; }
        if (addr >= 0xFF10) { m_apu.write(addr, v); return; }
        switch (addr) {
        case 0xFF00:
            // Changing the select lines can raise the joypad interrupt (Double Dragon 3, as Mesen)
            inputChange([&]() { m_joySelect = v & 0x30; });
            if (m_sgb) m_sgb->joypadWrite(m_joySelect);
            break;
        case 0xFF01: m_serialData = v; break;
        case 0xFF02:
            m_serialControl = v & 0x81;
            if ((m_serialControl & 0x80) && (m_serialControl & 0x01)) {
                m_serialBits = 8;
                m_serialOut += (char)m_serialData;       // what's sent (Blargg's tests print this way)
            } else {
                m_serialBits = 0;
            }
            break;
        case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07: timerWrite(addr, v); break;
        case 0xFF0F: m_if = v & 0x1F; break;
        }
        return;
    }
    if (addr >= 0xFE00) { m_ppu.writeOam((uint8_t)addr, v, false); return; }
    if (addr >= 0x8000 && addr < 0xA000) { m_ppu.writeVram(addr, v); return; }
    if (m_cart) m_cart->writeRegister(addr, v);
}

// ---------------------------------------------------------------------------
// The joypad (Mesen's GbControlManager)
// ---------------------------------------------------------------------------

uint8_t Gameboy::readJoypad() const {
    uint8_t r = 0x0F;
    const uint8_t sel = m_joySelect;
    if (m_sgb) {
        if ((sel & 0x30) == 0x30) r = m_sgb->joypadId();
        else {
            if (!(sel & 0x20)) r &= m_sgb->joypadButtons() >> 4;
            if (!(sel & 0x10)) r &= m_sgb->joypadButtons() & 0x0F;
        }
    } else {
        if (!(sel & 0x20)) r &= (uint8_t)~(m_buttons >> 4);
        if (!(sel & 0x10)) r &= (uint8_t)~(m_buttons & 0x0F);
    }
    return (uint8_t)((r & 0x0F) | (sel & 0x30) | 0xC0);
}

// ---------------------------------------------------------------------------
// The timer (Mesen's GbTimer)
// ---------------------------------------------------------------------------

void Gameboy::timerExec() {
    if ((m_div & 3) == 2) {
        m_reloaded = false;
        if (m_needReload) reloadTimer();
    }
    setDivider((uint16_t)(m_div + 2));
}

void Gameboy::reloadTimer() {
    m_tima = m_tma;
    requestIrq(IRQ_TIMER);
    m_needReload = false;
    m_reloaded = true;
}

void Gameboy::setDivider(uint16_t v) {
    if (m_timerOn && !(v & m_timerBit) && (m_div & m_timerBit)) {
        if (++m_tima == 0) m_needReload = true;
    }
    if (!(v & 0x1000) && (m_div & 0x1000)) m_apu.clockFrameSequencer();
    m_div = v;
}

uint8_t Gameboy::timerRead(uint16_t addr) const {
    switch (addr) {
    case 0xFF04: return (uint8_t)(m_div >> 8);
    case 0xFF05: return m_tima;
    case 0xFF06: return m_tma;
    default: return (uint8_t)(m_tac | 0xF8);
    }
}

void Gameboy::timerWrite(uint16_t addr, uint8_t v) {
    switch (addr) {
    case 0xFF04: setDivider(0); break;
    case 0xFF05:
        if (m_needReload) m_needReload = false;           // a write cancels the reload and its interrupt
        if (!m_reloaded) m_tima = v;                      // ignored on the reload's cycle
        break;
    case 0xFF06:
        m_tma = v;
        if (m_reloaded) m_tima = v;
        break;
    case 0xFF07: {
        m_tac = v & 0x07;
        const bool on = (v & 0x04) != 0;
        static const uint16_t bits[4] = {1 << 9, 1 << 3, 1 << 5, 1 << 7};
        const uint16_t bit = bits[v & 3];
        if (m_timerOn) {
            // Changing TAC can tick TIMA (the falling edge of the multiplexer's output)
            const bool tick = on ? ((m_div & m_timerBit) && !(m_div & bit)) : (m_div & m_timerBit) != 0;
            if (tick && ++m_tima == 0) reloadTimer();
        }
        m_timerOn = on;
        m_timerBit = bit;
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// OAM DMA (Mesen's GbDmaController)
// ---------------------------------------------------------------------------

void Gameboy::dmaExec() {
    if (m_cpu.halted()) return;                           // paused while the CPU is halted
    const bool wasRunning = m_dmaCounter > 0;
    if (wasRunning) {
        if (m_dmaCounter <= 160) m_ppu.writeOam((uint8_t)(160 - m_dmaCounter), m_dmaBuffer, true);
        --m_dmaCounter;
        m_dmaRunning = m_dmaCounter > 0;
        if (m_dmaRunning) m_dmaBuffer = dmaRead(dmaReadAddress());
    }
    if (m_dmaStartDelay > 0 && --m_dmaStartDelay == 0) {
        m_dmaDest = m_dmaSource;
        m_dmaCounter = 161;
        m_dmaRunning = wasRunning;
    }
}

bool Gameboy::dmaConflict(uint16_t addr) const {
    const uint8_t src = m_dmaSource;
    return ((src < 0x80 || (src >= 0xA0 && src <= 0xFD)) && (addr < 0x8000 || (addr >= 0xA000 && addr <= 0xFDFF))) ||
           (src >= 0x80 && src <= 0x9F && addr >= 0x8000 && addr <= 0x9FFF);
}

uint8_t Gameboy::dmaRead(uint16_t addr) {
    if (m_reads[addr >> 8]) return m_reads[addr >> 8][addr & 0xFF];
    if (addr >= 0x8000 && addr <= 0x9FFF) return m_ppu.readVram(addr);
    return 0;
}

template uint8_t Gameboy::read<OamCorruption::Read>(uint16_t);
template uint8_t Gameboy::read<OamCorruption::ReadIncDec>(uint16_t);
template uint8_t Gameboy::read<OamCorruption::Write>(uint16_t);

}  // namespace gb
