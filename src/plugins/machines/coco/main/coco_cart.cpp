// The CoCo's cartridge port devices: ROM Paks, the Multi-Pak Interface, the
// Orchestra-90, the Deluxe RS-232 Pak and the FD-500's glue logic. The
// Multi-Pak's routing and the FD-500 latch after XRoar (mpi.c, rsdos.c) and
// the Color Computer Technical Reference Manual.

#include "coco_cart.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include "plugins/devices/acia6551/main/acia6551.h"

namespace coco {

namespace {
constexpr uint64_t CART_HALF_PERIOD = 7159;     // 1 kHz square wave on CART (14.31818 MHz ticks)
constexpr uint32_t ACIA_CLOCK = 894886;         // the ACIA ticks in CPU cycles (16 ticks)
}

// ---------------------------------------------------------------------------
// ROM Pak
// ---------------------------------------------------------------------------
RomPak::RomPak(std::vector<uint8_t> image, std::string label, bool autostart)
    : m_rom(std::move(image)), m_label(std::move(label)), m_autostart(autostart) {
    if (m_rom.size() > 0x4000) m_rom.resize(0x4000);
}

std::string RomPak::describe() const {
    return "ROM Pak (" + m_label + ", " + std::to_string(m_rom.size() / 1024) + " KB)";
}

void RomPak::reset() {
    m_level = true;
    m_acc = 0;
    if (setCart) setCart(true);
}

bool RomPak::read(uint16_t a, bool cts, bool scs, uint8_t& d) { return peek(a, cts, scs, d); }

bool RomPak::peek(uint16_t a, bool cts, bool, uint8_t& d) const {
    if (!cts || m_rom.empty()) return false;
    d = m_rom[(a & 0x3FFF) % m_rom.size()];
    return true;
}

void RomPak::clock(uint64_t ticks) {
    if (!m_autostart) return;
    m_acc += ticks;
    while (m_acc >= CART_HALF_PERIOD) {
        m_acc -= CART_HALF_PERIOD;
        m_level = !m_level;
        if (setCart) setCart(m_level);
    }
}

// ---------------------------------------------------------------------------
// Orchestra-90
// ---------------------------------------------------------------------------
Orchestra90::Orchestra90(std::vector<uint8_t> rom) : m_rom(std::move(rom)) {}

void Orchestra90::reset() { m_left = m_right = 0x80; }

bool Orchestra90::read(uint16_t a, bool cts, bool scs, uint8_t& d) { return peek(a, cts, scs, d); }

bool Orchestra90::peek(uint16_t a, bool cts, bool, uint8_t& d) const {
    if (!cts || m_rom.empty()) return false;
    d = m_rom[(a & 0x3FFF) % m_rom.size()];
    return true;
}

void Orchestra90::write(uint16_t a, bool, bool, uint8_t d) {
    if (a == 0xFF7A) m_left = d;
    else if (a == 0xFF7B) m_right = d;
}

float Orchestra90::audio() const {
    return ((float)m_left - 128.0f + (float)m_right - 128.0f) / 512.0f;
}

// ---------------------------------------------------------------------------
// Deluxe RS-232 Pak
// ---------------------------------------------------------------------------
DeluxeRs232::DeluxeRs232() : m_acia(new Acia6551(0xFF60)) {
    m_acia->setName("RS-232 Pak");
    m_acia->setClockHz(ACIA_CLOCK);
}

DeluxeRs232::~DeluxeRs232() = default;

void DeluxeRs232::reset() {
    m_acia->reset();
    irqUpdate();
}

void DeluxeRs232::irqUpdate() {
    const bool irq = m_acia->irqAsserted();
    if (irq == m_irq) return;
    m_irq = irq;
    if (setCart) setCart(!irq);              // /IRQ pulls CART low
}

bool DeluxeRs232::read(uint16_t a, bool, bool, uint8_t& d) {
    if (a < 0xFF68 || a > 0xFF6B) return false;
    m_acia->ioRead(nullptr, a, &d);
    irqUpdate();
    return true;
}

bool DeluxeRs232::peek(uint16_t a, bool, bool, uint8_t& d) const {
    if (a < 0xFF68 || a > 0xFF6B) return false;
    m_acia->ioPeek(nullptr, a, &d);
    return true;
}

void DeluxeRs232::write(uint16_t a, bool, bool, uint8_t d) {
    if (a < 0xFF68 || a > 0xFF6B) return;
    m_acia->ioWrite(nullptr, a, d);
    irqUpdate();
}

void DeluxeRs232::clock(uint64_t ticks) {
    m_acc += ticks;
    if (m_acc < 16 * 64) return;            // in steps of 64 cycles
    m_acia->tick(m_acc / 16);
    m_acc %= 16;
    irqUpdate();
}

bool DeluxeRs232::attachSerial(const std::string& spec) { return m_acia->attachSerial(spec); }

// ---------------------------------------------------------------------------
// FD-500
// ---------------------------------------------------------------------------
Fd500::Fd500(std::vector<uint8_t> diskBasic)
    : m_fdc(new Wd17xx(Wd17xx::Variant::WD2793, 0xFF48)), m_rom(std::move(diskBasic)) {
    m_fdc->setClockHz(14318180);                  // clock() counts master (14.31818 MHz) ticks
    m_fdc->setReadyMode(Wd17xx::ReadyMode::Always);
    m_fdc->onDrq([this](bool on) { setDrq(on); });
    m_fdc->onIntrq([this](bool on) { setIntrq(on); });
}

Fd500::~Fd500() { m_fdc->flush(); }

std::string Fd500::describe() const {
    std::string s = "FD-500 disk controller (Disk BASIC)";
    for (int i = 0; i < 4; ++i)
        if (!m_paths[i].empty()) {
            const auto slash = m_paths[i].find_last_of('/');
            s += "; drive " + std::to_string(i) + ": " + (slash == std::string::npos ? m_paths[i] : m_paths[i].substr(slash + 1));
        }
    return s;
}

void Fd500::reset() {
    // The board's reset leaves the controller idle (as XRoar): the master reset's Restore
    // is ended at once by a force interrupt without INTRQ, so no NMI before Disk BASIC is up
    m_fdc->reset();
    m_fdc->writeRegister(0, 0xD0);
    m_drq = m_intrq = false;
    latchWrite(0);
}

bool Fd500::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (cts) return peek(a, cts, scs, d);
    if (!scs || !(a & 8)) return false;
    d = m_fdc->readRegister(a & 3);
    return true;
}

bool Fd500::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (cts) {
        if (m_rom.empty()) return false;
        d = m_rom[(a & 0x3FFF) % m_rom.size()];
        return true;
    }
    if (!scs || !(a & 8)) return false;
    d = m_fdc->peekRegister(a & 3);
    return true;
}

void Fd500::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (!scs) return;
    if (a & 8) m_fdc->writeRegister(a & 3, d);
    else latchWrite(d);
}

void Fd500::clock(uint64_t ticks) { m_fdc->tick(ticks); }

bool Fd500::mountDisk(int drive, const std::string& path) {
    if (drive < 0 || drive > 3 || !m_fdc->mountDisk(drive, path)) return false;
    m_paths[drive] = path;
    return true;
}

bool Fd500::ejectDisk(int drive) {
    if (drive < 0 || drive > 3 || m_paths[drive].empty()) return false;
    m_fdc->ejectDisk(drive);
    m_paths[drive].clear();
    return true;
}

std::string Fd500::diskPath(int drive) const { return drive >= 0 && drive < 4 ? m_paths[drive] : ""; }

bool Fd500::diskStatus(int drive, int& track, bool& motor) const {
    if (drive < 0 || drive > 3 || m_paths[drive].empty()) return false;
    track = m_fdc->driveCylinder(drive);
    motor = (m_latch & 0x08) && m_fdc->selectedDrive() == drive;
    return true;
}

void Fd500::latchWrite(uint8_t v) {
    m_latch = v;
    const int drive = (v & 1) ? 0 : (v & 2) ? 1 : (v & 4) ? 2 : (v & 0x40) ? 3 : -1;
    const bool side = (v & 0x40) && drive != 3;
    m_fdc->selectDrive(drive);
    m_fdc->setSide(side ? 1 : 0);
    m_fdc->setMotor(v & 0x08);
    m_fdc->setDoubleDensity(!(v & 0x20));
    // With double density selected (bit 5 low) INTRQ reaches /NMI
    if (!(v & 0x20) && m_intrq && setNmi) setNmi(true);
    m_haltEnable = (v & 0x80) && !m_intrq;
    updateHalt();
}

void Fd500::updateHalt() {
    if (setHalt) setHalt(m_haltEnable && !m_drq);
}

void Fd500::setDrq(bool on) {
    m_drq = on;
    updateHalt();
}

void Fd500::setIntrq(bool on) {
    m_intrq = on;
    if (on) {
        m_haltEnable = false;
        updateHalt();
        if (!(m_latch & 0x20) && setNmi) setNmi(true);
    } else if (setNmi) {
        setNmi(false);
    }
}

// ---------------------------------------------------------------------------
// Multi-Pak Interface
// ---------------------------------------------------------------------------
MultiPak::MultiPak() = default;

std::string MultiPak::describe() const {
    std::string s = "Multi-Pak Interface (switch " + std::to_string(m_switch + 1) + ")";
    for (int i = 0; i < 4; ++i)
        if (m_slots[i]) s += "; slot " + std::to_string(i + 1) + ": " + m_slots[i]->describe();
    return s;
}

void MultiPak::insert(int n, std::unique_ptr<Cart> cart) {
    n &= 3;
    m_slots[n] = std::move(cart);
    m_cart[n] = true;
    m_nmi[n] = m_halt[n] = false;
    Cart* c = m_slots[n].get();
    if (!c) return;
    c->setCart = [this, n](bool level) {
        m_cart[n] = level;
        if (n == ctsSlot() && setCart) setCart(level);
    };
    c->setNmi = [this, n](bool on) {
        m_nmi[n] = on;
        if (setNmi) setNmi(m_nmi[0] || m_nmi[1] || m_nmi[2] || m_nmi[3]);
    };
    c->setHalt = [this, n](bool on) {
        m_halt[n] = on;
        if (setHalt) setHalt(m_halt[0] || m_halt[1] || m_halt[2] || m_halt[3]);
    };
}

void MultiPak::select(uint8_t v) {
    m_reg = v & 0x33;
    if (setCart) setCart(m_cart[ctsSlot()]);
}

void MultiPak::reset() {
    for (auto& s : m_slots) if (s) s->reset();
    select((uint8_t)(m_switch << 4 | m_switch));
}

bool MultiPak::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (a == 0xFF7F) {
        d = m_reg;
        return true;
    }
    if (cts) return m_slots[ctsSlot()] && m_slots[ctsSlot()]->read(a, true, false, d);
    if (scs) return m_slots[scsSlot()] && m_slots[scsSlot()]->read(a, false, true, d);
    bool driven = false;
    for (auto& s : m_slots) if (s && s->read(a, false, false, d)) driven = true;
    return driven;
}

bool MultiPak::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (a == 0xFF7F) {
        d = m_reg;
        return true;
    }
    if (cts) return m_slots[ctsSlot()] && m_slots[ctsSlot()]->peek(a, true, false, d);
    if (scs) return m_slots[scsSlot()] && m_slots[scsSlot()]->peek(a, false, true, d);
    for (auto& s : m_slots) if (s && s->peek(a, false, false, d)) return true;
    return false;
}

void MultiPak::write(uint16_t a, bool cts, bool scs, uint8_t d) {
    if (a == 0xFF7F) {
        select(d);
        return;
    }
    if (cts) { if (m_slots[ctsSlot()]) m_slots[ctsSlot()]->write(a, true, false, d); return; }
    if (scs) { if (m_slots[scsSlot()]) m_slots[scsSlot()]->write(a, false, true, d); return; }
    for (auto& s : m_slots) if (s) s->write(a, false, false, d);
}

void MultiPak::clock(uint64_t ticks) {
    for (auto& s : m_slots) if (s) s->clock(ticks);
}

float MultiPak::audio() const {
    float v = 0.0f;
    for (auto& s : m_slots) if (s) v += s->audio();
    return v;
}

bool MultiPak::attachSerial(const std::string& spec) {
    for (auto& s : m_slots) if (s && s->attachSerial(spec)) return true;
    return false;
}

bool MultiPak::mountDisk(int drive, const std::string& path) {
    for (auto& s : m_slots) if (s && s->mountDisk(drive, path)) return true;
    return false;
}

bool MultiPak::ejectDisk(int drive) {
    for (auto& s : m_slots) if (s && s->ejectDisk(drive)) return true;
    return false;
}

std::string MultiPak::diskPath(int drive) const {
    for (auto& s : m_slots) if (s && !s->diskPath(drive).empty()) return s->diskPath(drive);
    return "";
}

bool MultiPak::diskStatus(int drive, int& track, bool& motor) const {
    for (auto& s : m_slots) if (s && s->diskStatus(drive, track, motor)) return true;
    return false;
}

}  // namespace coco
