// The CoCo SDC and Burke & Burke cartridges on the CoCo's cartridge port (#356).

#include "coco_storage.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"

namespace coco {

namespace {
std::string leaf(const std::string& p) {
    const auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}
}  // namespace

// ---------------------------------------------------------------------------
// CoCo SDC
// ---------------------------------------------------------------------------
CocoSdcCart::CocoSdcCart(std::vector<uint8_t> flash, std::string cardDir, int dipBank) : m_fd(std::vector<uint8_t>()) {
    m_fd.controller().setVariant(Wd17xx::Variant::FD1793);
    m_fd.setNmi = [this](bool on) { if (setNmi) setNmi(on); };
    m_fd.setHalt = [this](bool on) { if (setHalt) setHalt(on); };
    m_sdc.insertFloppy = [this](int d, std::unique_ptr<FloppyDisk> disk) {
        m_fd.controller().insertDisk(d, std::move(disk));
        return true;
    };
    m_sdc.removeFloppy = [this](int d) { return m_fd.controller().removeDisk(d); };
    m_sdc.floppy = [this](int d) { return m_fd.controller().disk(d); };
    m_sdc.loadFlash(flash);
    m_sdc.setDipBank(dipBank);
    m_sdc.setCardRoot(cardDir);       // power-on: STARTUP.CFG mounts through the hooks
}

std::string CocoSdcCart::describe() const {
    std::string s = "CoCo SDC (";
    s += m_sdc.hasFlash() ? "flash bank " + std::to_string(m_sdc.bank()) : std::string("no firmware");
    s += m_sdc.cardRoot().empty() ? ", no SD card" : ", SD card " + m_sdc.cardRoot();
    s += ")";
    for (int i = 0; i < 2; ++i)
        if (!m_sdc.imagePath(i).empty())
            s += "; drive " + std::to_string(i) + ": " + leaf(m_sdc.imagePath(i)) + (m_sdc.isHardDisk(i) ? " (LBA)" : "");
    return s;
}

void CocoSdcCart::reset() {
    m_sdc.reset();
    m_fd.reset();
}

bool CocoSdcCart::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (cts) return peek(a, cts, scs, d);
    if (!scs || a >= 0xFF50) return false;
    if (m_sdc.read(a & 0x0F, d)) return true;
    return (a & 0x0C) == 0x08 && m_fd.read(a, false, true, d);
}

bool CocoSdcCart::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (cts) {
        if (!m_sdc.hasFlash()) return false;
        d = m_sdc.romByte(a);
        return true;
    }
    if (!scs || a >= 0xFF50) return false;
    if (m_sdc.peek(a & 0x0F, d)) return true;
    return (a & 0x0C) == 0x08 && m_fd.peek(a, false, true, d);
}

void CocoSdcCart::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (!scs || a >= 0xFF50) return;
    const int reg = a & 0x0F;
    if (reg == 0) {
        // $43 enters command mode; anything else is the drive latch (and leaves it)
        if (!m_sdc.writeLatch(d)) m_fd.write(a, false, true, d);
        return;
    }
    if (m_sdc.write(reg, d)) return;
    if ((reg & 0x0C) == 0x08) m_fd.write(a, false, true, d);
}

bool CocoSdcCart::mountDisk(int drive, const std::string& path) { return m_sdc.mount(drive, path); }

bool CocoSdcCart::ejectDisk(int drive) {
    if (m_sdc.imagePath(drive).empty()) return false;
    m_sdc.eject(drive);
    return true;
}

std::string CocoSdcCart::diskPath(int drive) const { return m_sdc.imagePath(drive); }

bool CocoSdcCart::diskStatus(int drive, int& track, bool& motor) const {
    if (m_sdc.imagePath(drive).empty()) return false;
    Wd17xx& fdc = const_cast<Fd500&>(m_fd).controller();
    track = fdc.driveCylinder(drive);
    motor = (m_fd.latch() & 0x08) && fdc.selectedDrive() == drive;
    return true;
}

// ---------------------------------------------------------------------------
// Burke & Burke CoCo XT
// ---------------------------------------------------------------------------
std::string BurkeBurke::describe() const {
    std::string s = "Burke & Burke CoCo XT hard disk interface ($FF50)";
    if (!m_rom.empty()) s += " with a " + std::to_string(m_rom.size() / 1024) + " KB ROM";
    for (int i = 0; i < 2; ++i)
        if (!m_hdc.imagePath(i).empty())
            s += "; drive " + std::to_string(i) + ": " + leaf(m_hdc.imagePath(i)) + " (" +
                 std::to_string(m_hdc.cylinders(i)) + " cyl, " + std::to_string(m_hdc.heads(i)) + " heads)";
    return s;
}

bool BurkeBurke::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (cts) return peek(a, cts, scs, d);
    if (!scs || (a & 0xFFFC) != BASE) return false;
    d = m_hdc.read(a & 3);
    return true;
}

bool BurkeBurke::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (cts) {
        // The BIOS ROM socket: A4-A6 inverted by the interface (manual A.1.1)
        if (m_rom.empty()) return false;
        d = m_rom[(size_t)((a ^ 0x0070) & 0x3FFF) % m_rom.size()];
        return true;
    }
    if (!scs || (a & 0xFFFC) != BASE) return false;
    d = m_hdc.peek(a & 3);
    return true;
}

void BurkeBurke::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (scs && (a & 0xFFFC) == BASE) m_hdc.write(a & 3, d);
}

bool BurkeBurke::mountDisk(int drive, const std::string& path) { return m_hdc.mount(drive, path); }

bool BurkeBurke::ejectDisk(int drive) {
    if (m_hdc.imagePath(drive).empty()) return false;
    m_hdc.eject(drive);
    return true;
}

bool BurkeBurke::diskStatus(int drive, int& track, bool& motor) const {
    if (m_hdc.imagePath(drive).empty()) return false;
    track = 0;
    motor = false;
    return true;
}

}  // namespace coco
