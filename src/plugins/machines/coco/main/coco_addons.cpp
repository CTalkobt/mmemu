// The Speech/Sound Cartridge and the CoCo Max Hi-Res Input Pack, after
// MAME's src/devices/bus/coco/coco_ssc.cpp and coco_max.cpp (tim lindner;
// BSD-3-Clause).

#include "coco_addons.h"
#include "machine_coco.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace coco {

namespace {

// Port C
constexpr uint8_t C_BC1 = 0x01;     // (and the RAM's A8)
constexpr uint8_t C_RRW = 0x08;     // the RAM's R/W (and the AY's BDIR)
constexpr uint8_t C_BDR = 0x08;
constexpr uint8_t C_RCS = 0x10;
constexpr uint8_t C_ALD = 0x20;
constexpr uint8_t C_ACS = 0x40;
constexpr uint8_t C_BSY = 0x80;

constexpr int TICKS_PER_CYCLE = 16; // the TMS7040's machine cycle: (E x 2) / 2
constexpr int TICKS_PER_AY_STEP = 128;  // SpectrumAy::step is 16 AY clocks of E x 2
constexpr float AY_FULL = 0.72f;    // SpectrumAy::step with all three channels at 15
constexpr double SAC_RATE = 48000.0;    // MAME's sound activity circuit runs at the output rate
constexpr float SP0256_GAIN = 1.75f, AY8913_GAIN = 2.0f;    // MAME's routes
constexpr float MIX = 0.2f;         // onto the CoCo's audio

bool readAll(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The Speech/Sound Cartridge
// ---------------------------------------------------------------------------
SpeechSoundPak::SpeechSoundPak(std::vector<uint8_t> mcuRom, std::vector<uint8_t> speechRom) {
    m_mcu.setRom(std::move(mcuRom));
    m_haveSpeech = !speechRom.empty();
    m_sp.setRom(speechRom);
    m_mcu.portIn = [this](int port) { return portIn(port); };
    m_mcu.portOut = [this](int port, uint8_t v) { portOut(port, v); };
    m_sp.onLrq = [this](bool on) { m_mcu.setInt1(on); };
    reset();                        // (a Multi-Pak slot doesn't reset what goes in)
}

std::string SpeechSoundPak::describe() const {
    std::string s = "Speech/Sound Cartridge (TMS7040, SP0256-AL2, AY-3-8913 at $FF7D/$FF7E)";
    if (!m_haveSpeech) s += "; no SP0256-AL2 ROM: speech silent";
    return s;
}

void SpeechSoundPak::reset() {
    std::fill(std::begin(m_ram), std::end(m_ram), 0);
    m_resetLine = 0;                // (the host's RESET clears the reset flip-flop)
    m_busy = false;
    m_portA = m_portB = m_portC = m_portD = 0;
    m_ayActive = false;
    m_ayLatch = 0;
    m_ay.reset();
    m_sp.reset();
    m_mcu.setInt3(false);
    m_mcu.setInt1(m_sp.lrq());
    m_mcu.reset();
    m_budget = 0;
    m_ayAcc = 0;
    m_spAcc = m_sacAcc = 0;
    m_ayLevel = 0;
    m_aySum = 0;
    m_ayCount = 0;
    m_audio = 0;
    m_sacEnvelope = m_sacPrevIn = m_sacPrevOut = 0;
    m_sacActive = false;
}

uint8_t SpeechSoundPak::status() const {
    uint8_t d = 0x1F;
    if (!m_busy) d |= 0x80;
    if (m_sp.sby()) d |= 0x40;
    if (!m_sacActive) d |= 0x20;
    return d;
}

bool SpeechSoundPak::read(uint16_t a, bool cts, bool scs, uint8_t& d) { return peek(a, cts, scs, d); }

bool SpeechSoundPak::peek(uint16_t a, bool, bool, uint8_t& d) const {
    // (the 74LS138 decodes only writes to $FF7D and $FF7E and reads of $FF7E)
    if (a == 0xFF7E) { d = status(); return true; }
    return false;
}

void SpeechSoundPak::write(uint16_t a, bool, bool, uint8_t d) {
    if (a == 0xFF7D) {
        // D0 high holds the TMS7040, the SP0256 and the AY in reset (the
        // service manual's flip-flop IC4); low lets them run
        if (d & 1) {
            m_sp.reset();
            m_ay.reset();
            m_ayActive = false;
            m_busy = false;
        }
        if ((m_resetLine & 1) && !(d & 1)) m_mcu.reset();
        m_resetLine = d;
    } else if (a == 0xFF7E) {
        m_portA = d;
        m_busy = true;
        m_mcu.setInt3(true);
    }
}

uint8_t SpeechSoundPak::portIn(int port) {
    switch (port) {
        case 0:                                     // the CoCo's byte: reading it acknowledges INT3
            m_mcu.setInt3(false);
            return m_portA;
        case 2: return m_portC;
        case 3: portDRead(); return m_portD;
        default: return 0xFF;
    }
}

void SpeechSoundPak::portOut(int port, uint8_t v) {
    switch (port) {
        case 1: m_portB = v; break;
        case 2: portCWrite(v); break;
        case 3: m_portD = v; break;
        default: break;
    }
}

void SpeechSoundPak::portCWrite(uint8_t v) {
    if (!(v & C_RCS) && !(v & C_RRW))               // the RAM: write
        m_ram[((v << 8) | m_portB) & 0x7FF] = m_portD;
    if (!(v & C_ACS)) {                             // the AY
        if ((v & (C_BDR | C_BC1)) == (C_BDR | C_BC1)) {         // latch an address
            m_ayActive = (m_portD >> 4) == 0;       // (the 8913's mask: the high nibble 0)
            if (m_ayActive) {
                m_ayLatch = m_portD & 15;
                m_ay.selectRegister(m_ayLatch);
            }
        } else if ((v & C_BDR) && !(v & C_BC1)) {               // write
            if (m_ayActive) m_ay.write(m_portD);
        }
    }
    if ((m_portC & C_ALD) && !(v & C_ALD) && m_portD < 64) m_sp.ald(m_portD);
    if (!(m_portC & C_BSY) && (v & C_BSY)) m_busy = false;
    m_portC = v;
}

void SpeechSoundPak::portDRead() {
    if (!(m_portC & C_RCS) && (m_portC & C_RRW))   // the RAM: read
        m_portD = m_ram[((m_portC << 8) | m_portB) & 0x7FF];
    if (!(m_portC & C_ACS) && !(m_portC & C_BDR) && (m_portC & C_BC1)) {      // the AY: read
        // (no I/O ports on the 8913: R14/R15 read back what was written)
        m_portD = !m_ayActive ? 0xFF : m_ayLatch >= 14 ? m_ay.reg(m_ayLatch) : m_ay.read();
    }
}

void SpeechSoundPak::clock(uint64_t ticks) {
    m_aySum = 0;
    m_ayCount = 0;
    m_budget += (int64_t)ticks;
    while (m_budget > 0) {
        const int64_t t = (m_resetLine & 1) ? m_budget : (int64_t)m_mcu.step() * TICKS_PER_CYCLE;
        m_budget -= t;
        runSound(t);
    }
    const float ay = m_ayCount ? (float)(m_aySum / m_ayCount) : m_ayLevel;
    const float sp = (float)m_sp.output() / 32768.0f;
    m_audio = MIX * (SP0256_GAIN * sp + AY8913_GAIN * ay);
}

// The AY (a step every 16 of its clocks), the SP0256 (a sample every 312 of
// its 3.12 MHz clocks) and the sound activity circuit, `ticks` on
void SpeechSoundPak::runSound(int64_t ticks) {
    m_ayAcc += (uint64_t)ticks;
    while (m_ayAcc >= TICKS_PER_AY_STEP) {
        m_ayAcc -= TICKS_PER_AY_STEP;
        m_ayLevel = m_ay.step() / AY_FULL;
        m_aySum += m_ayLevel;
        ++m_ayCount;
    }
    m_spAcc += (double)ticks * (Sp0256::CLOCK_HZ / Sp0256::CLOCK_DIVIDER);
    while (m_spAcc >= m_hz) {
        m_spAcc -= m_hz;
        m_sp.sample();
    }
    m_sacAcc += (double)ticks * SAC_RATE;
    while (m_sacAcc >= m_hz) {
        m_sacAcc -= m_hz;
        sacStep(AY8913_GAIN * m_ayLevel);
    }
}

// MAME's cocossc_sac_device: a high-pass filter (the DC), an envelope
// follower (fast attack, slow decay) and on / off thresholds
void SpeechSoundPak::sacStep(float x) {
    constexpr float HPF_ALPHA = 0.99f, ATTACK = 0.0026f, DECAY = 0.0003f, THRESH_ON = 0.05f, THRESH_OFF = 0.01f;
    const float y = HPF_ALPHA * (m_sacPrevOut + x - m_sacPrevIn);
    m_sacPrevIn = x;
    m_sacPrevOut = y;
    const float rect = std::fabs(y);
    m_sacEnvelope += (rect - m_sacEnvelope) * (rect > m_sacEnvelope ? ATTACK : DECAY);
    if (m_sacActive && m_sacEnvelope < THRESH_OFF) m_sacActive = false;
    else if (m_sacEnvelope > THRESH_ON) m_sacActive = true;
}

std::unique_ptr<Cart> makeSpeechSoundPak(const std::string& option, std::string& err) {
    const auto comma = option.find(',');
    const std::string mcuPath = option.substr(0, comma);
    const std::string spPath = comma == std::string::npos ? "" : option.substr(comma + 1);
    std::vector<uint8_t> mcu, sp;
    const std::string mcuFile = mcuPath.empty() ? firmwareDir() + "/" + SpeechSoundPak::MCU_ROM : mcuPath;
    if (!readAll(mcuFile, mcu)) {
        err = "Speech/Sound Cartridge: the TMS7040's ROM not found: put " + std::string(SpeechSoundPak::MCU_ROM) +
              " (4 KB) in " + firmwareDir() + "/ (or set MMEMU_COCO_FIRMWARE), or give its path";
        return nullptr;
    }
    if (mcu.size() != 0x1000) { err = "the TMS7040's ROM is 4 KB: " + mcuFile; return nullptr; }
    const std::string spFile = spPath.empty() ? firmwareDir() + "/" + SpeechSoundPak::SPEECH_ROM : spPath;
    if (!readAll(spFile, sp) && !spPath.empty()) { err = "can't read the SP0256-AL2 ROM " + spPath; return nullptr; }
    if (!sp.empty() && sp.size() != 0x800) { err = "the SP0256-AL2's ROM is 2 KB: " + spFile; return nullptr; }
    return std::unique_ptr<Cart>(new SpeechSoundPak(std::move(mcu), std::move(sp)));
}

// ---------------------------------------------------------------------------
// The CoCo Max Hi-Res Input Pack
// ---------------------------------------------------------------------------
void CocoMax::setPosition(int x, int y) {
    m_x = std::max(0, std::min(255, x));
    m_y = std::max(0, std::min(255, y));
}

bool CocoMax::read(uint16_t a, bool, bool, uint8_t& d) {
    if (a < 0xFF90 || a > 0xFF97) return false;
    d = m_result;
    switch (a & 7) {
        case 0: m_result = (uint8_t)m_y; break;
        case 1: m_result = (uint8_t)m_x; break;
        case 2: m_result = (m_buttons & 1) ? 0x00 : 0xFF; break;
        case 3: m_result = (m_buttons & 2) ? 0x00 : 0xFF; break;
        default: break;                             // not connected
    }
    return true;
}

bool CocoMax::peek(uint16_t a, bool, bool, uint8_t& d) const {
    if (a < 0xFF90 || a > 0xFF97) return false;
    d = m_result;
    return true;
}

CocoMax* findCocoMax(Cart* port) {
    if (auto* m = dynamic_cast<CocoMax*>(port)) return m;
    if (auto* mpi = dynamic_cast<MultiPak*>(port)) {
        for (int i = 0; i < 4; ++i)
            if (auto* m = dynamic_cast<CocoMax*>(mpi->slot(i))) return m;
    }
    return nullptr;
}

}  // namespace coco
