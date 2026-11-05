// Commander X16 HostFS: Commodore bus L2 (TALK/LISTEN) and L3 (Commodore
// DOS) served from a host folder through the KERNAL's IEEE API.
//
// Ported from x16-emulator src/ieee.c (HostFS), the IEEE intercept and
// STATUS write in src/main.c, and src/iso_8859_15.c,
// Copyright (c) 2019, 2022, 2023 Michael Steil, et al. All rights reserved.
// License: 2-clause BSD:
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met: 1. Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
//
// The UTF-8 decoder is the public-domain branchless decoder (utf8.h); the
// encoder is after Ondřej Hruška's utf8_encode (MIT).
//
// Not ported: the 65C816-only EXTAPI16 calls (XMACPTR / XMCIOUT), the
// "-prg" autoload hack, and the cycle compensation for host time.

#include "x16_hostfs.h"
#include "libcore/main/icore.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include "util/logging.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {

auto logger() { return LogRegistry::instance().getLogger("x16.hostfs"); }

// x16-emulator main.c: the KERNAL call addresses trapped
constexpr uint16_t K_MCIOUT = 0xFEB1, K_MACPTR = 0xFF44, K_SECOND = 0xFF93, K_TKSA = 0xFF96,
                   K_ACPTR = 0xFFA5, K_CIOUT = 0xFFA8, K_UNTLK = 0xFFAB, K_UNLSN = 0xFFAE,
                   K_LISTEN = 0xFFB1, K_TALK = 0xFFB4;

} // namespace

// ---------------------------------------------------------------------------
// Construction, settings, the intercept
// ---------------------------------------------------------------------------

X16HostFs::X16HostFs(const nlohmann::json& e) {
    if (e.contains("unit")) {
        const auto& u = e["unit"];
        m_unit = u.is_string() ? parseCbmUnit(u.get<std::string>()) : parseCbmUnit(std::to_string(u.get<int>()));
    }
    m_rootSetting = e.value("path", std::string("."));
    m_startinSetting = e.value("startin", std::string());
}

std::vector<uint32_t> X16HostFs::addresses() const {
    return { K_MCIOUT, K_MACPTR, K_SECOND, K_TKSA, K_ACPTR, K_CIOUT, K_UNTLK, K_UNLSN, K_LISTEN, K_TALK };
}

bool X16HostFs::setOption(const std::string& key, const std::string& value) {
    if (key == "path" || key == "root") {
        m_rootSetting = value;
        m_startinSetting.clear();
        reset();
        return true;
    }
    if (key == "startin") {
        m_startinSetting = value;
        reset();
        return true;
    }
    if (key == "unit") {
        try { m_unit = parseCbmUnit(value); } catch (...) { return false; }
        return true;
    }
    return false;
}

std::string X16HostFs::describe() const {
    std::string s = "Host folder as IEEE unit " + std::to_string(m_unit) + ": " + m_rootSetting;
    if (m_initialized && m_cwd != m_fsroot) s += " (cwd " + m_cwd + ")";
    if (m_unit == 8 && m_sdCardProbe && m_sdCardProbe()) s += " [inactive: SD card attached serves unit 8]";
    return s;
}

void X16HostFs::reset() {
    CbmHostDos::reset();
    m_kernalLocated = false;        // the KERNAL variables are found again at the next call
}

// The KERNAL's variables, found from its code (x16-emulator ieee_init and
// set_kernal_status): cbdos_flags from ACPTR's first instruction (BIT
// cbdos_flags), STATUS from READST (LDA/ORA/STA status).
void X16HostFs::locateKernalVariables() {
    m_kernalLocated = true;
    IBus& b = *m_bus;
    m_cbdosFlags = 0;
    if (b.peek8(0xFFA5) == 0x4C) {
        uint16_t kacptr = (uint16_t)(b.peek8(0xFFA6) | b.peek8(0xFFA7) << 8);
        if (kacptr >= 0xC000 && b.peek8(kacptr) == 0x2C) {
            uint16_t f = (uint16_t)(b.peek8(kacptr + 1) | b.peek8(kacptr + 2) << 8);
            if (f >= 0x0200 && f < 0x0400) m_cbdosFlags = f;
        }
    }
    if (!m_cbdosFlags) logger()->warn("HostFS: unable to find KERNAL cbdos_flags");

    m_statusAddr = 0;
    if (b.peek8(0xFFB7) == 0x4C) {
        uint16_t readst = (uint16_t)(b.peek8(0xFFB8) | b.peek8(0xFFB9) << 8);
        if (readst >= 0xC000 && b.peek8(readst) == 0xAD && b.peek8(readst + 3) == 0x0D &&
            b.peek8(readst + 6) == 0x8D) {
            uint16_t s0 = (uint16_t)(b.peek8(readst + 1) | b.peek8(readst + 2) << 8);
            uint16_t s1 = (uint16_t)(b.peek8(readst + 4) | b.peek8(readst + 5) << 8);
            uint16_t s2 = (uint16_t)(b.peek8(readst + 7) | b.peek8(readst + 8) << 8);
            if (s0 == s1 && s0 == s2) m_statusAddr = s0;
        }
    }
    if (!m_statusAddr) logger()->warn("HostFS: unable to find the KERNAL STATUS variable");

    setError(0x73, 0, 0);
}

uint8_t X16HostFs::read6502(uint16_t addr) { return m_bus->read8(addr); }
void X16HostFs::write6502(uint16_t addr, uint8_t v) { m_bus->write8(addr, v); }

void X16HostFs::setKernalStatus(uint8_t s) {
    if (m_statusAddr) write6502(m_statusAddr, s);
    else logger()->warn("HostFS: could not set STATUS");
}

void X16HostFs::setCbdosFlags(uint8_t flags) {
    if (m_cbdosFlags && m_bus) write6502(m_cbdosFlags, flags);
}

uint8_t X16HostFs::getCbdosFlags() {
    return (m_cbdosFlags && m_bus) ? read6502(m_cbdosFlags) : 0;
}

bool X16HostFs::onExecute(MachineDescriptor&, ICore& cpu, IBus& bus, uint32_t pc) {
    return handle(cpu, bus, pc);
}

bool X16HostFs::handle(ICore& cpu, IBus& bus, uint32_t pc) {
    pc &= 0xFFFF;
    if (pc < 0xFEA8) return false;
    // Only the KERNAL (its "MIST" signature, in the current ROM bank)
    bool kernal = (bus.peek8(0xFFF6) == 'M' && bus.peek8(0xFFF7) == 'I' &&
                   bus.peek8(0xFFF8) == 'S' && bus.peek8(0xFFF9) == 'T') ||
                  (bus.peek8(0xC008) == 'M' && bus.peek8(0xC009) == 'I' &&
                   bus.peek8(0xC00A) == 'S' && bus.peek8(0xC00B) == 'T');
    if (!kernal) return false;
    // An SD card serves unit 8: HostFS on unit 8 steps aside
    if (m_unit == 8 && m_sdCardProbe && m_sdCardProbe()) return false;

    m_bus = &bus;
    if (!m_initialized && !init()) { m_bus = nullptr; return false; }
    if (!m_kernalLocated) locateKernalVariables();

    uint8_t a = (uint8_t)cpu.regReadByName("A");
    uint8_t x = (uint8_t)cpu.regReadByName("X");
    uint8_t y = (uint8_t)cpu.regReadByName("Y");
    uint8_t p = (uint8_t)cpu.regReadByName("P");

    bool handled = true;
    int s = -1;
    switch (pc) {
        case K_MCIOUT:
        case K_MACPTR: {
            uint16_t count = a;
            uint16_t addr = (uint16_t)(y << 8 | x);
            s = pc == K_MACPTR ? macptr(addr, &count, p & 0x01) : mciout(addr, &count, p & 0x01);
            if (s == -2) {
                handled = false;
            } else if (s == -3) {
                p |= 0x01;                                  // SEC: unsupported (no open file)
            } else {
                cpu.regWriteByName("X", count & 0xFF);
                cpu.regWriteByName("Y", count >> 8);
                p &= 0xFE;                                  // CLC: supported
            }
            break;
        }
        case K_SECOND:
            s = second(a);
            if (s == -2) handled = false;
            break;
        case K_TKSA:
            s = tksa(a);
            if (s == -2) handled = false;
            break;
        case K_ACPTR:
            s = acptr(&a);
            if (s == -2) {
                handled = false;
            } else {
                cpu.regWriteByName("A", a);
                p = (uint8_t)((p & ~3) | (!a << 1));        // CLC, Z from the byte
            }
            break;
        case K_CIOUT:
            s = ciout(a);
            if (s == -2) handled = false;
            else p &= 0xFE;
            break;
        case K_UNTLK:
            s = untlk();
            if (s == -2) handled = false;
            break;
        case K_UNLSN:
            s = unlsn();
            if (s == -2) handled = false;
            break;
        case K_LISTEN:
            s = listen(a);
            if (s == -2) handled = false;
            else p &= 0xFE;
            break;
        case K_TALK:
            s = talk(a);
            if (s == -2) handled = false;
            else p &= 0xFE;
            break;
        default:
            handled = false;
            break;
    }

    if (handled) {
        cpu.regWriteByName("P", p);
        if (s >= 0) setKernalStatus((uint8_t)s);
        // RTS
        uint8_t sp = (uint8_t)cpu.regReadByName("SP");
        sp++;
        uint8_t lo = bus.read8(0x0100 | sp);
        sp++;
        uint8_t hi = bus.read8(0x0100 | sp);
        cpu.regWriteByName("SP", sp);
        cpu.setPc((uint16_t)(((hi << 8) | lo) + 1));
    }
    m_bus = nullptr;
    return handled;
}


void X16HostFs::onActivity(bool active) {
    uint8_t f = getCbdosFlags();
    if (active) f |= 0x10;
    else        f &= ~0x10;
    setCbdosFlags(f);
}

void X16HostFs::onErrorFlag(bool error) {
    uint8_t f = getCbdosFlags();
    if (error) f |= 0x20;                       // set error flag
    else       f &= ~0x20;                      // clear error
    setCbdosFlags(f);
}

int X16HostFs::macptr(uint16_t addr, uint16_t* c, bool streamMode) {
    if (!m_talking) return -2;
    int ret = 0;
    int count = *c ? *c : 256;
    uint8_t ramBank = read6502(0);
    int i = 0;
    Channel& ch = m_channels[m_channel];
    if (m_channel != 15 && ch.read && (ch.name.empty() || ch.name[0] != '$') && ch.f) {
        // the end of the file, to know the EOF condition ahead of time
        long curpos = std::ftell(ch.f);
        std::fseek(ch.f, 0, SEEK_END);
        long endpos = std::ftell(ch.f);
        std::fseek(ch.f, curpos, SEEK_SET);
        do {
            int b = std::fgetc(ch.f);
            if (b == EOF) {
                ret = 0x42;
            } else {
                curpos++;
                if (curpos == endpos) {
                    ret = 0x40;
                    ch.read = false;
                    cclose(m_channel);
                }
                write6502(addr, (uint8_t)b);
                i++;
                if (!streamMode) {
                    addr++;
                    if (addr == 0xc000) {
                        addr = 0xa000;
                        ramBank++;
                        write6502(0, ramBank);
                    }
                }
            }
            if (ret > 0) break;
        } while (i < count);
    } else {
        ret = -3;                                   // unsupported
    }
    *c = (uint16_t)i;
    return ret;
}

int X16HostFs::mciout(uint16_t addr, uint16_t* c, bool streamMode) {
    if (!m_listening) return -2;
    int ret = 0;
    int count = *c ? *c : 256;
    uint8_t ramBank = read6502(0);
    int i = 0;
    Channel& ch = m_channels[m_channel];
    if (ch.f && ch.write) {
        do {
            uint8_t b = read6502(addr);
            i++;
            if (!streamMode) {
                addr++;
                if (addr == 0xc000) {
                    addr = 0xa000;
                    ramBank++;
                    write6502(0, ramBank);
                }
            }
            ret = ciout(b);
            if (ret) break;
        } while (i < count);
    } else {
        ret = -3;
    }
    *c = (uint16_t)i;
    return ret;
}

void registerX16HostFsIntercept() {
    auto& reg = InterceptRegistry::instance();
    if (reg.hasType("x16.hostfs")) return;
    reg.registerType("x16.hostfs", [](const nlohmann::json& e) -> IIntercept* { return new X16HostFs(e); });
}
