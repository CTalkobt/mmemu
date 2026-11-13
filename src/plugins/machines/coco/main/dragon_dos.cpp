// DragonDOS, Dragon Data's disk cartridge: a WD2797 and a latch. Its
// decoding and latch after XRoar (dragondos.c; GPL-3.0-or-later) and the
// DragonDOS manual; mmsim's own code.

#include "coco_cart.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"

namespace coco {

DragonDos::DragonDos(std::vector<uint8_t> rom)
    : m_fdc(new Wd17xx(Wd17xx::Variant::WD2797, 0xFF40)), m_rom(std::move(rom)) {
    m_fdc->setClockHz(14318180);
    m_fdc->setReadyMode(Wd17xx::ReadyMode::Always);
    // DRQ is the CART line (PIA1 CB1, FIRQ); INTRQ is /NMI when the latch enables it
    m_fdc->onDrq([this](bool on) { if (setCart) setCart(on); });
    m_fdc->onIntrq([this](bool on) { setIntrq(on); });
}

DragonDos::~DragonDos() { m_fdc->flush(); }

std::string DragonDos::describe() const {
    std::string s = "DragonDOS disk controller (WD2797)";
    for (int i = 0; i < 4; ++i)
        if (!m_paths[i].empty()) {
            const auto slash = m_paths[i].find_last_of('/');
            s += "; drive " + std::to_string(i + 1) + ": " + (slash == std::string::npos ? m_paths[i] : m_paths[i].substr(slash + 1));
        }
    return s;
}

void DragonDos::setClockHz(double hz) { m_fdc->setClockHz((uint32_t)hz); }

void DragonDos::reset() {
    // The cartridge's reset leaves the controller idle (no Restore, no INTRQ)
    m_fdc->reset();
    m_fdc->writeRegister(0, 0xD0);
    latchWrite(0);
    if (setCart) setCart(false);
    if (setNmi) setNmi(false);
}

bool DragonDos::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (cts) return peek(a, cts, scs, d);
    if (!scs || (a & 0x0C)) return false;
    d = m_fdc->readRegister(a & 3);
    return true;
}

bool DragonDos::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (cts) {
        if (m_rom.empty()) return false;
        d = m_rom[(a & 0x3FFF) % m_rom.size()];
        return true;
    }
    if (!scs || (a & 0x0C)) return false;
    d = m_fdc->peekRegister(a & 3);
    return true;
}

void DragonDos::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (!scs) return;
    if (!(a & 0x0C)) m_fdc->writeRegister(a & 3, d);       // $FF40-$FF43
    else if (a & 8) latchWrite(d);                          // $FF48-$FF4F
}

void DragonDos::clock(uint64_t ticks) { m_fdc->tick(ticks); }

bool DragonDos::mountDisk(int drive, const std::string& path) {
    if (drive < 0 || drive > 3 || !m_fdc->mountDisk(drive, path)) return false;
    m_paths[drive] = path;
    return true;
}

bool DragonDos::ejectDisk(int drive) {
    if (drive < 0 || drive > 3 || m_paths[drive].empty()) return false;
    m_fdc->ejectDisk(drive);
    m_paths[drive].clear();
    return true;
}

std::string DragonDos::diskPath(int drive) const { return drive >= 0 && drive < 4 ? m_paths[drive] : ""; }

bool DragonDos::diskStatus(int drive, int& track, bool& motor) const {
    if (drive < 0 || drive > 3 || m_paths[drive].empty()) return false;
    track = m_fdc->driveCylinder(drive);
    motor = (m_latch & 0x04) && m_fdc->selectedDrive() == drive;
    return true;
}

void DragonDos::latchWrite(uint8_t v) {
    m_latch = v;
    m_fdc->selectDrive(v & 3);
    m_fdc->setMotor(v & 0x04);
    m_fdc->setDoubleDensity(!(v & 0x08));
}

// INTRQ rising reaches /NMI only with the latch's NMI enable; falling always releases it
void DragonDos::setIntrq(bool on) {
    if (!setNmi) return;
    if (!on) setNmi(false);
    else if (m_latch & 0x20) setNmi(true);
}

}  // namespace coco
