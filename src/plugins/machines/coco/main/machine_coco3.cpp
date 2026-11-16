// The Tandy Color Computer 3: the CoCo board around the TCC1014 GIME. The
// wiring after the CoCo 3 service manual, Sockmaster's GIME reference and
// XRoar (coco3.c; GPL-3.0-or-later): mmsim's own code.

#include "coco_system.h"
#include "libcore/main/machine_desc.h"
#include <cstring>
#include <fstream>

namespace coco {

namespace {

bool readRom(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

// The board: 32 KB of ROM, the RAM, the GIME's sync pulses to PIA0 (HS on
// CA1, FS on CB1, as on the CoCo 2: no inversion on a PAL board), its
// interrupts to the CPU with the PIAs'
void System::wireCoco3(std::vector<uint8_t> rom, int ramKb) {
    m_rom3.assign(0x8000, 0xFF);
    for (size_t i = 0; i < m_rom3.size() && !rom.empty(); ++i) m_rom3[i] = rom[i % rom.size()];
    m_gime.reset(new Gime(Gime::Revision::G1987));
    m_gime->setPal(m_pal);
    display.setGime(m_gime.get());
    setRamKb(ramKb);
    m_gime->onHs = [this](bool level, uint64_t at) {
        pia0.advance(at);
        pia0.setCa1(level);
    };
    m_gime->onFs = [this](bool level, uint64_t at) {
        pia0.advance(at);
        pia0.setCb1(level);
    };
    m_gime->onIrq = [this]() { updateLines(); };
}

// Every cycle at the GIME's rate; I/O brings the devices up to time first.
// The ROM's address lines are the CPU's (A0-A14); the cartridge sees /CTS
// and /SCS, and decodes $FF60-$FF8F itself (Multi-Pak, RS-232 Pak,
// Orchestra-90). Nothing driving the bus leaves its last byte there.
uint8_t System::access3(uint16_t a, bool write, uint8_t v) {
    const uint64_t start = m_ticks + m_instr;
    if (a >= 0xFF00) sync(start);
    m_instr += (uint64_t)m_gime->cycleTicks();
    ++m_accesses;
    const Gime::Decode d = m_gime->decode(a);
    const bool cts = d.s == Gime::S_CTS, scs = d.s == Gime::S_SCS;
    const bool cartSees = m_cart && (cts || scs || (a >= 0xFF60 && a < 0xFF90));
    if (write) {
        m_bus = v;
        switch (d.s) {
            case Gime::S_RAM: m_ram3[d.phys] = v; break;
            case Gime::S_PIA0:
                pia0.write(a, v);
                coco3Lines();           // the keyboard's EI1 as the strobe changes (XRoar: every cycle)
                break;
            case Gime::S_PIA1:
                m_gime->snoopPia1(a, v);
                pia1.write(a, v);
                break;
            case Gime::S_GIME: m_gime->write(a, v); break;
            default: break;
        }
        if (cartSees) {
            m_cart->write(a, cts, scs, v);
            soundChanged();
        }
        return v;
    }
    uint8_t r = m_bus;
    switch (d.s) {
        case Gime::S_RAM: r = m_ram3[d.phys]; break;
        case Gime::S_ROM: r = m_rom3[a & 0x7FFF]; break;
        case Gime::S_PIA0: r = pia0.read(a); break;
        case Gime::S_PIA1: r = pia1.read(a); break;
        case Gime::S_GIME: {
            uint8_t g;
            if (m_gime->read(a, g)) {
                const uint8_t mask = m_gime->readMask(a);    // six bits (the Disto board's two more)
                r = (uint8_t)((r & ~mask) | (g & mask));
            }
            break;
        }
        default: break;
    }
    if (cartSees) {
        uint8_t cd;
        if (m_cart->read(a, cts, scs, cd)) r = cd;
    }
    m_bus = r;
    return r;
}

uint8_t System::peek3(uint16_t a) {
    const Gime::Decode d = m_gime->decode(a);
    const bool cts = d.s == Gime::S_CTS, scs = d.s == Gime::S_SCS;
    uint8_t r = 0xFF;
    switch (d.s) {
        case Gime::S_RAM: r = m_ram3[d.phys]; break;
        case Gime::S_ROM: r = m_rom3[a & 0x7FFF]; break;
        case Gime::S_PIA0: r = pia0.peek(a); break;
        case Gime::S_PIA1: r = pia1.peek(a); break;
        case Gime::S_GIME: m_gime->peek(a, r); break;
        default: break;
    }
    if (m_cart && (cts || scs || (a >= 0xFF60 && a < 0xFF90))) {
        uint8_t cd;
        if (m_cart->peek(a, cts, scs, cd)) r = cd;
    }
    return r;
}

// The GIME's keyboard interrupt (EI1): any of PIA0 PA0-PA6 low (a key in a
// column the software drives low, or a fire button). The serial interrupt
// (EI2): the RS-232 receive line at space (a start bit's falling edge), as
// PIA1 PB0 reads it, while something drives the line. A CM-8 (RGB) monitor pulls PIA1 PB3 low (XRoar's
// monitor detect).
void System::coco3Lines() {
    keyboardUpdate();
    m_gime->setInterruptInput(Gime::INT_EI1, (pia0.portA() | 0x80) != 0xFF);
    const bool connected = rs232.rxConnected();
    const bool rx = rs232.rxLevel(m_dev) && connected;     // nothing connected: low (as XRoar)
    if (rx) { pia1.b.inSource |= 0x01; pia1.b.inSink |= 0x01; }
    else { pia1.b.inSource &= (uint8_t)~0x01; pia1.b.inSink &= (uint8_t)~0x01; }
    m_gime->setInterruptInput(Gime::INT_EI2, connected && !rx);
    if (m_gime->monitor() == Gime::Monitor::Rgb) pia1.b.inSink &= (uint8_t)~0x08;
    else pia1.b.inSink |= 0x08;
}

bool loadCoco3Rom(bool pal, std::vector<uint8_t>& rom, std::string& missing) {
    const std::string dir = firmwareDir();
    const std::vector<std::string> names = pal ? std::vector<std::string>{"coco3p.rom", "coco3.rom"}
                                               : std::vector<std::string>{"coco3.rom"};
    for (const auto& n : names) {
        if (readRom(dir + "/" + n, rom) && rom.size() == 0x8000) return true;
    }
    rom.clear();
    missing = "Super Extended Color BASIC ROM not found: put " + names[0] +
              " (the CoCo 3's 32 KB ROM) in " + dir + "/ or set MMEMU_COCO_FIRMWARE to its directory";
    return false;
}

// The stand-in: the VDG-compatible text screen at $0000 (GIME COCO mode,
// vertical offset $70000 so the screen is the CPU's page $38), the text
// colours in the palette, the message, and a wait
std::vector<uint8_t> coco3PlaceholderRom() {
    std::vector<uint8_t> r(0x8000, 0x12);
    const uint8_t code[] = {
        0x10, 0xCE, 0x7F, 0x00,     // LDS #$7F00
        0x86, 0x84,                 // LDA #$84 (COCO, MC2)
        0xB7, 0xFF, 0x90,           // STA $FF90
        0x86, 0xE0,                 // LDA #$E0
        0xB7, 0xFF, 0x9D,           // STA $FF9D (video at $70000)
        0x7F, 0xFF, 0x9E,           // CLR $FF9E
        0x86, 0x0F,                 // LDA #$0F
        0xB7, 0xFF, 0x9C,           // STA $FF9C (12 lines a text row)
        0x7F, 0xFF, 0xBC,           // CLR $FFBC (text background: black)
        0x86, 0x12,                 // LDA #$12
        0xB7, 0xFF, 0xBD,           // STA $FFBD (text foreground: green)
        0x8E, 0x00, 0x00,           // LDX #$0000
        0x86, 0x60,                 // LDA #$60 (space)
        0xA7, 0x80,                 // STA ,X+
        0x8C, 0x02, 0x00,           // CMPX #$0200
        0x26, 0xF9,                 // BNE
        0x8E, 0x00, 0x00,           // LDX #$0000
        0x10, 0x8E, 0x80, 0x80,     // LDY #$8080
        0xA6, 0xA0,                 // LDA ,Y+
        0x27, 0x04,                 // BEQ done
        0xA7, 0x80,                 // STA ,X+
        0x20, 0xF8,                 // BRA
        0x20, 0xFE,                 // done: BRA *
    };
    std::memcpy(r.data(), code, sizeof code);
    const char* msg = "NO SUPER EXTENDED BASIC ROM: PUT COCO3.ROM IN ROMS/COCO/ (SEE DOC/README-COCO.MD)";
    size_t i = 0;
    for (; msg[i]; ++i) {
        const uint8_t c = (uint8_t)msg[i];
        r[0x80 + i] = (c >= 0x20 && c < 0x40) ? (uint8_t)(c | 0x40) : c;    // as BASIC stores text
    }
    r[0x80 + i] = 0;
    r[0x7FFE] = 0x80;               // reset vector: $8000
    r[0x7FFF] = 0x00;
    return r;
}

void coco3Slots(MachineDescriptor* desc, System* sys) {
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "ram";
        s.description = "RAM fitted: 128K (the stock machine), 512K (the common upgrade), or 1M / 2M "
                        "(Disto's MMU extension: page register bits 6-7, video bank at $FF9B); changing it clears the RAM";
        s.devices = {{"ram", "The RAM", {"128K", "512K", "1M", "2M"}, "128K"}};
        s.canEmpty = false;
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            if (device != "ram") { err = "the ram slot takes 'ram' (128K, 512K, 1M, 2M)"; return false; }
            const uint64_t bytes = MachineDescriptor::parseMemorySize(option.empty() ? "128K" : option);
            if (bytes != 128 * 1024 && bytes != 512 * 1024 && bytes != 1024 * 1024 && bytes != 2048 * 1024) {
                err = "128K, 512K, 1M or 2M";
                return false;
            }
            sys->setRamKb((int)(bytes / 1024));
            return true;
        };
        s.info = [sys]() {
            const int kb = sys->gime()->ramKb();
            return "ram (" + (kb >= 1024 ? std::to_string(kb / 1024) + "M" : std::to_string(kb) + "K") + ")";
        };
        desc->expansionSlots.push_back(std::move(s));
    }
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "monitor";
        s.description = "The display: a CM-8 RGB monitor or a TV / composite monitor (the GIME's NTSC colours; "
                        "a PAL board's RGB encoded); composite's option: NTSC artifact colours off, bluered or redblue";
        s.devices = {
            {"composite", "A TV or composite monitor: two bits of intensity, four of hue", {"off", "bluered", "redblue"}, "off"},
            {"rgb", "The CM-8 RGB monitor: two bits each of red, green and blue", {}, ""},
        };
        s.canEmpty = false;
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            Gime* g = sys->gime();
            if (device == "composite" || device.empty()) {
                if (option.empty() || option == "off") g->setArtifact(Gime::Artifact::Off);
                else if (option == "bluered") g->setArtifact(Gime::Artifact::BlueRed);
                else if (option == "redblue") g->setArtifact(Gime::Artifact::RedBlue);
                else { err = "composite's artifact colours: off, bluered or redblue"; return false; }
                g->setMonitor(Gime::Monitor::Composite);
            } else if (device == "rgb") {
                g->setMonitor(Gime::Monitor::Rgb);
            } else {
                err = "composite or rgb";
                return false;
            }
            return true;
        };
        s.info = [sys]() {
            const Gime* g = sys->gime();
            if (g->monitor() == Gime::Monitor::Rgb) return std::string("rgb");
            const auto a = g->artifact();
            return std::string("composite") + (a == Gime::Artifact::BlueRed ? " (bluered)" : a == Gime::Artifact::RedBlue ? " (redblue)" : "");
        };
        desc->expansionSlots.push_back(std::move(s));
    }
    {
        MachineDescriptor::ExpansionSlot s;
        s.name = "gime";
        s.description = "The GIME's revision: 1986 (the first CoCo 3s) or 1987 (timer value + 2 or + 1, horizontal timing)";
        s.devices = {{"gime", "The TCC1014 GIME", {"1986", "1987"}, "1987"}};
        s.canEmpty = false;
        s.install = [sys](const std::string& device, const std::string& option, std::string& err) {
            if (device != "gime") { err = "the gime slot takes 'gime' (1986, 1987)"; return false; }
            if (option == "1986") sys->gime()->setRevision(Gime::Revision::G1986);
            else if (option.empty() || option == "1987") sys->gime()->setRevision(Gime::Revision::G1987);
            else { err = "1986 or 1987"; return false; }
            return true;
        };
        s.info = [sys]() { return std::string("gime (") + (sys->gime()->revision() == Gime::Revision::G1986 ? "1986" : "1987") + ")"; };
        desc->expansionSlots.push_back(std::move(s));
    }
}

}  // namespace coco
