// SmartPort block device card in slot 7, after KEGS by Kent Dickey
// (GPL-3.0): config.c (g_rom_c700, the card's ROM), smartport.c (do_c700,
// do_c70a, do_c70d, do_read_c7, do_write_c7, do_format_c7), sim65816.c
// (do_wdm: the trap's flags pick the entry).

#include "gs_smartport.h"
#include "libcore/main/icore.h"
#include "libdevices/main/disk_file.h"
#include "libmem/main/ibus.h"
#include "plugins/65816/main/cpu65816.h"
#include <algorithm>
#include <cstring>

namespace apple2 {

namespace {
// KEGS g_rom_c700: LDX #$20 / LDX #$00 / LDX #$03 / CMP #$00 (the
// signature), BRA to SEP #$41 / BVS (boot: V set); $C70A CLC / CLV / BVS
// (never) whose operand is $C70D's SEC; CLV; $C70F WDM $C7,$00; RTS
const uint8_t ROM_HEAD[] = {0xA2, 0x20, 0xA2, 0x00, 0xA2, 0x03, 0xC9, 0x00, 0x80, 0x0C, 0x18, 0xB8, 0x70,
                            0x38, 0xB8, 0x42, 0xC7, 0x00, 0x60, 0x00, 0x00, 0xEA, 0xE2, 0x41, 0x70, 0xF5};
// $C7FB: SmartPort ID ($80: extended calls), $C7FC/D: 0 (status gives the
// size), $C7FE: $BF (status, read, write, format; 4 volumes; removable,
// interruptible), $C7FF: the ProDOS entry ($C70A)
const uint8_t ROM_TAIL[] = {0x80, 0x00, 0x00, 0xBF, 0x0A};

using R = W65C816;
}  // namespace

GsSmartPort::GsSmartPort() {
    std::memcpy(m_rom, ROM_HEAD, sizeof ROM_HEAD);
    std::memcpy(m_rom + 0xFB, ROM_TAIL, sizeof ROM_TAIL);
}

bool GsSmartPort::anyMounted() const {
    for (const Unit& u : m_unit) if (u.size) return true;
    return false;
}

bool GsSmartPort::mountDisk(int unit, const std::string& path) {
    if (unit < 1 || unit > UNITS) return false;
    std::vector<uint8_t> data;
    if (!diskfile::read(path, data)) return false;
    size_t off = 0, size = data.size();
    if (data.size() >= 64 && std::memcmp(data.data(), "2IMG", 4) == 0) {
        if (data[12] != 1) return false;                     // ProDOS order only
        off = data[24] | data[25] << 8 | data[26] << 16 | (size_t)data[27] << 24;
        size = data[28] | data[29] << 8 | data[30] << 16 | (size_t)data[31] << 24;
        if (!size) size = (size_t)(data[20] | data[21] << 8 | data[22] << 16 | (size_t)data[23] << 24) * 512;
        if (off + size > data.size()) return false;
    }
    if (size < 512 || size % 512) return false;
    Unit& u = m_unit[unit - 1];
    u = Unit();
    u.file = std::move(data);
    u.offset = off;
    u.size = size;
    u.path = path;
    u.protect = !diskfile::writable(path);
    u.switched = true;
    return true;
}

bool GsSmartPort::insertBytes(int unit, std::vector<uint8_t> data) {
    if (unit < 1 || unit > UNITS || data.size() < 512 || data.size() % 512) return false;
    Unit& u = m_unit[unit - 1];
    u = Unit();
    u.size = data.size();
    u.file = std::move(data);
    u.switched = true;
    return true;
}

void GsSmartPort::ejectDisk(int unit) {
    if (unit >= 1 && unit <= UNITS) { m_unit[unit - 1] = Unit(); m_unit[unit - 1].switched = true; }
}

bool GsSmartPort::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit < 1 || unit > UNITS) return false;
    Unit& u = m_unit[unit - 1];
    track = std::max(u.lastBlock, 0);
    sector = 0;
    led = u.busy;
    u.busy = false;
    return u.size != 0;
}

std::string GsSmartPort::getMountedDiskPath(int unit) {
    return unit >= 1 && unit <= UNITS ? m_unit[unit - 1].path : "";
}

// do_read_c7: 0, $27 (I/O error: past the end), $2F (offline)
int GsSmartPort::readBlock(int unit, IBus& bus, uint32_t buf, uint32_t block) {
    if (unit < 0 || unit >= UNITS) return 0x28;
    Unit& u = m_unit[unit];
    if (!u.size) return 0x2F;
    if ((uint64_t)(block + 1) * 512 > u.size) return 0x27;
    const uint8_t* p = &u.file[u.offset + (size_t)block * 512];
    if (buf < 0xFC0000)
        for (uint32_t i = 0; i < 512; ++i) bus.write8((buf + i) & 0xFFFFFF, p[i]);
    u.lastBlock = (int)block;
    u.busy = true;
    return 0;
}

// do_write_c7: $2B write protected
int GsSmartPort::writeBlock(int unit, IBus& bus, uint32_t buf, uint32_t block) {
    if (unit < 0 || unit >= UNITS) return 0x28;
    Unit& u = m_unit[unit];
    if (!u.size) return 0x28;
    if (u.protect) return 0x2B;
    if ((uint64_t)(block + 1) * 512 > u.size) return 0x27;
    const size_t at = u.offset + (size_t)block * 512;
    for (uint32_t i = 0; i < 512; ++i) u.file[at + i] = bus.read8((buf + i) & 0xFFFFFF);
    if (!u.path.empty()) diskfile::patch(u.path, u.file, at, 512);
    u.lastBlock = (int)block;
    u.busy = true;
    return 0;
}

// do_format_c7: the volume zeroed
int GsSmartPort::format(int unit) {
    if (unit < 0 || unit >= UNITS) return 0x28;
    Unit& u = m_unit[unit];
    if (!u.size) return 0x28;
    if (u.protect) return 0x2B;
    std::fill(u.file.begin() + (long)u.offset, u.file.begin() + (long)(u.offset + u.size), 0);
    if (!u.path.empty()) diskfile::patch(u.path, u.file, u.offset, u.size);
    return 0;
}

bool GsSmartPort::onExecute(MachineDescriptor&, ICore& cpu, IBus& bus, uint32_t pc) {
    (void)pc;
    // Only the card's ROM in bank $00 (or its shadows) at $C70F
    const uint32_t full = cpu.pc();
    const uint32_t bank = full >> 16;
    if (bank != 0 && bank != 1 && bank != 0xE0 && bank != 0xE1) return false;
    if (!romVisibleNow || !romVisibleNow()) return false;
    const uint8_t p = (uint8_t)cpu.regRead(R::R_P);
    if (p & R::FV) boot(cpu, bus);
    else if (p & R::FC) smartport(cpu, bus);
    else prodos(cpu, bus);
    return true;
}

// do_c700: block 0 of unit 1 to $0800, entered at $0801 with X = $70
// (slot 7); with no disk, slot 5 boots instead
void GsSmartPort::boot(ICore& cpu, IBus& bus) {
    const int ret = readBlock(0, bus, 0x0800, 0);
    bus.write8(0x07F8, 0x07);
    bus.write8(0x42, 0x01); bus.write8(0x43, 0x70);
    bus.write8(0x44, 0x00); bus.write8(0x45, 0x08);
    bus.write8(0x46, 0x00); bus.write8(0x47, 0x00);
    cpu.regWrite(R::R_X, 0x70);
    cpu.regWrite(R::R_PBR, 0);
    cpu.regWrite(R::R_PC, ret == 0 ? 0x0801 : 0xC500);
}

// do_c70a: the ProDOS block call ($42 command, $43 unit, $44 buffer, $46
// block), then the ROM's RTS
void GsSmartPort::prodos(ICore& cpu, IBus& bus) {
    bus.write8(0x07F8, 0xC7);
    const uint32_t d = cpu.regRead(R::R_D);
    auto zp = [&](uint32_t o) { return bus.read8((d + o) & 0xFFFF); };
    const uint32_t cmd = zp(0x42), pu = zp(0x43);
    const uint32_t buf = zp(0x44) | zp(0x45) << 8, blk = zp(0x46) | zp(0x47) << 8;
    int unit = (int)(pu >> 7);                               // drive 1, 2
    if ((pu & 0x70) != 0x70) unit += 2;                      // mirrored into another slot: 3, 4
    int ret = 0x27;
    if (cmd == 0) {
        const uint32_t blocks = (uint32_t)(m_unit[unit].size / 512);
        ret = m_unit[unit].size ? 0 : 0x2F;
        cpu.regWrite(R::R_X, blocks & 0xFF);
        cpu.regWrite(R::R_Y, (blocks >> 8) & 0xFF);
    } else if (cmd == 1) ret = readBlock(unit, bus, buf, blk);
    else if (cmd == 2) ret = writeBlock(unit, bus, buf, blk);
    else if (cmd == 3) ret = format(unit);
    cpu.regWrite(R::R_A, (cpu.regRead(R::R_A) & 0xFF00) | (uint32_t)ret);
    const uint8_t p = (uint8_t)cpu.regRead(R::R_P);
    cpu.regWrite(R::R_P, ret ? (p | R::FC) : (p & ~R::FC));
    cpu.regWrite(R::R_PC, 0xC712);                           // the RTS
}

// do_c70d: JSR $C70D / command / parameter list (2 bytes, or 4 for the
// extended calls, $40 set); returns past them
void GsSmartPort::smartport(ICore& cpu, IBus& bus) {
    bus.write8(0x07F8, 0xC7);
    auto rd = [&](uint32_t a) { return (uint32_t)bus.read8(a & 0xFFFFFF); };
    auto wr = [&](uint32_t a, uint32_t v) { bus.write8(a & 0xFFFFFF, (uint8_t)v); };
    // The return address, pulled as RTS would (page 1 in emulation mode)
    uint32_t s = cpu.regRead(R::R_S);
    const bool emu = cpu.regRead(R::R_E);
    auto pull = [&]() {
        s = emu ? (0x100 | ((s + 1) & 0xFF)) : ((s + 1) & 0xFFFF);
        return rd(s);
    };
    const uint32_t lo = pull(), hi = pull();
    cpu.regWrite(R::R_S, s);
    const uint32_t rts = ((lo | hi << 8) + 1) & 0xFFFF;
    const uint32_t cmd = rd(rts);
    const bool ext = cmd & 0x40;
    const uint32_t mask = ext ? 0xFFFFFF : 0xFFFF;
    uint32_t list = rd(rts + 1) | rd(rts + 2) << 8 | (ext ? rd(rts + 3) << 16 : 0);
    const uint32_t count = rd(list), unit = rd((list + 1) & mask);
    const uint32_t ctl = rd((list + 4 + (ext ? 2 : 0)) & mask);
    int ret = 0;
    if (unit >= 1 && unit <= UNITS && ext) {
        if (m_unit[unit - 1].switched) ret = 0x2E;           // disk switched
        m_unit[unit - 1].switched = false;
    }
    auto ptr = [&](uint32_t at) {                            // a buffer pointer in the list
        uint32_t p = rd((list + at) & mask) | rd((list + at + 1) & mask) << 8;
        if (ext) p |= rd((list + at + 2) & mask) << 16 | rd((list + at + 3) & mask) << 24;
        return p;
    };
    uint32_t x = 0, y = 2;
    switch (cmd & 0x3F) {
        case 0x00: {                                         // status
            if (count != 3) { ret = 0x04; break; }
            uint32_t st = ptr(2) & 0xFFFFFF;
            const bool online = unit >= 1 && unit <= UNITS && m_unit[unit - 1].size;
            const uint32_t blocks = online ? (uint32_t)(m_unit[unit - 1].size / 512) : 0;
            const uint32_t flags = online ? (0xF8u | (m_unit[unit - 1].protect ? 4u : 0u)) : 0x80u;
            if (unit == 0 && ctl == 0) {                     // the driver: number of units
                wr(st, UNITS); wr(st + 1, 0xFF);
                wr(st + 2, 0x4B); wr(st + 3, 0x00);           // vendor
                wr(st + 4, 0x00); wr(st + 5, 0x10);           // version
                wr(st + 6, 0); wr(st + 7, 0);
                x = 8; y = 0;
            } else if (unit > 0 && ctl == 0) {               // the unit's status and size
                wr(st, flags);
                wr(st + 1, blocks); wr(st + 2, blocks >> 8); wr(st + 3, blocks >> 16);
                x = 4;
                if (ext) { wr(st + 4, blocks >> 24); x = 5; }
                y = 0;
                if (ret == 0x2E && !online) ret = 0;
            } else if (ctl == 3) {                           // the device information block
                wr(st, flags);
                wr(st + 1, blocks); wr(st + 2, blocks >> 8); wr(st + 3, blocks >> 16);
                if (ext) { wr(st + 4, blocks >> 24); ++st; }
                const char* id = "MMEMU SMARTPORT ";
                wr(st + 4, 16);
                for (int i = 0; i < 16; ++i) wr(st + 5 + (uint32_t)i, (uint8_t)id[i]);
                wr(st + 21, 0x02); wr(st + 22, 0xC0);         // a hard disk: extended calls, disk switch
                wr(st + 23, 0x00); wr(st + 24, 0x00);
                x = ext ? 26 : 25; y = 0;
                if (unit == 0 || unit > UNITS) ret = 0x28;
            } else {
                ret = 0x21;                                  // bad control code
            }
            break;
        }
        case 0x01: case 0x02: {                              // read / write a block
            if (count != 3) { ret = 0x04; break; }
            const uint32_t buf = ptr(2) & 0xFFFFFF;
            const uint32_t at = ext ? 6 : 4;
            uint32_t block = rd((list + at) & mask) | rd((list + at + 1) & mask) << 8 | rd((list + at + 2) & mask) << 16;
            if (ext) block |= rd((list + at + 3) & mask) << 24;
            if (ret == 0) {
                if (unit < 1 || unit > UNITS) ret = 0x28;
                else ret = (cmd & 0x3F) == 1 ? readBlock((int)unit - 1, bus, buf, block)
                                             : writeBlock((int)unit - 1, bus, buf, block);
            }
            break;
        }
        case 0x03:                                           // format
            if (count != 1) { ret = 0x04; break; }
            if (unit < 1 || unit > UNITS) ret = 0x11;
            if (ret == 0) ret = format((int)unit - 1);
            break;
        case 0x04:                                           // control: accepted
            break;
        default:
            x = rts & 0xFF; y = (rts >> 8) & 0xFF;
            ret = 0x01;                                      // bad command
            break;
    }
    cpu.regWrite(R::R_X, x);
    cpu.regWrite(R::R_Y, y);
    cpu.regWrite(R::R_A, (cpu.regRead(R::R_A) & 0xFF00) | (uint32_t)ret);
    const uint8_t p = (uint8_t)cpu.regRead(R::R_P);
    cpu.regWrite(R::R_P, ret ? (p | R::FC) : (p & ~R::FC));
    cpu.regWrite(R::R_PC, (rts + 3 + (ext ? 2 : 0)) & 0xFFFF);
}

}  // namespace apple2
