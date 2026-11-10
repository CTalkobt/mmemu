#include "super_serial.h"
#include "apple2_parts.h"

namespace apple2 {

namespace {
// SW1's baud codes (AppleWin BaudRateToIndex: the 6551's rate codes)
int baudCode(int baud) {
    switch (baud) {
        case 110: return 0x05; case 300: return 0x06; case 600: return 0x07; case 1200: return 0x08;
        case 2400: return 0x0A; case 4800: return 0x0C; case 9600: return 0x0E; case 19200: return 0x0F;
        default: return -1;
    }
}
}  // namespace

SuperSerialCard* SuperSerialCard::create(int slot, int baud, std::string& err) {
    const int code = baudCode(baud);
    if (code < 0) { err = "the Super Serial Card's switches give 110, 300, 600, 1200, 2400, 4800, 9600 or 19200 bps"; return nullptr; }
    std::vector<uint8_t> rom;
    if (!loadFile("roms/apple2/SSC.rom", 0x800, rom, &err)) return nullptr;
    return new SuperSerialCard(slot, baud, code, std::move(rom));
}

SuperSerialCard::SuperSerialCard(int slot, int baud, int baudCode, std::vector<uint8_t> rom)
    : m_slot(slot), m_baud(baud), m_baudCode((uint8_t)baudCode), m_rom(std::move(rom)),
      m_acia(0xC080u + (uint32_t)slot * 16u) {
    m_name = "Super Serial Card (slot " + std::to_string(slot) + ")";
    m_acia.setName(m_name);
    m_acia.setClockHz(CPU_HZ);
    m_acia.setResetCommand(0x00);                   // a Synertek SY6551
}

uint8_t SuperSerialCard::dipSwitch1() const { return (uint8_t)(m_baudCode << 4); }   // CIC mode

uint8_t SuperSerialCard::dipSwitch2() const {
    // 1 stop bit, 8 data bits, no parity: 0; no LF: b1; CTS active low
    uint8_t st = 0;
    const_cast<Acia6551&>(m_acia).ioPeek(nullptr, m_acia.baseAddr() + 1, &st);
    const bool farEnd = !(st & 0x40);               // DSR asserted: something is there
    return (uint8_t)(0x02 | (farEnd ? 0 : 1));
}

int SuperSerialCard::devicePeek(int reg) {
    if (reg == 1) return dipSwitch1();
    if (reg == 2) return dipSwitch2();
    if (reg >= 8 && reg <= 11) {
        uint8_t v = 0xFF;
        m_acia.ioPeek(nullptr, m_acia.baseAddr() + (uint32_t)(reg & 3), &v);
        return v;
    }
    return -1;
}

int SuperSerialCard::deviceIo(int reg, const uint8_t* wr, uint64_t) {
    if (reg >= 8 && reg <= 11) {
        const uint32_t a = m_acia.baseAddr() + (uint32_t)(reg & 3);
        if (wr) { m_acia.ioWrite(nullptr, a, *wr); return 0; }
        uint8_t v = 0xFF;
        m_acia.ioRead(nullptr, a, &v);
        return v;
    }
    if (wr) return 0;
    return devicePeek(reg);
}

void SuperSerialCard::getDeviceInfo(DeviceInfo& out) const {
    m_acia.getDeviceInfo(out);
    out.name = m_name;
    out.state.push_back({"DIP SW1", std::to_string(dipSwitch1())});
    out.state.push_back({"DIP SW2", std::to_string(dipSwitch2())});
}

}  // namespace apple2
