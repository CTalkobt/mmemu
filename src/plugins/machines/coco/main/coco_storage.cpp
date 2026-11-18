// The CoCo SDC, Burke & Burke, Disto Super Controller II and Becker port
// cartridges on the CoCo's cartridge port (#356).

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
    std::string s = "Burke & Burke CoCo XT-RTC hard disk interface ($FF50) and DS1215 clock";
    if (!m_rom.empty()) s += " with a " + std::to_string(m_rom.size() / 1024) + " KB ROM";
    for (int i = 0; i < 2; ++i)
        if (!m_hdc.imagePath(i).empty())
            s += "; drive " + std::to_string(i) + ": " + leaf(m_hdc.imagePath(i)) + " (" +
                 std::to_string(m_hdc.cylinders(i)) + " cyl, " + std::to_string(m_hdc.heads(i)) + " heads)";
    return s;
}

bool BurkeBurke::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (cts) return peek(a, cts, scs, d);
    if (!scs) return false;
    // The DS1215 sees every /SCS cycle ($FF40-$FF5F): A2 low writes A0 to it,
    // A2 high reads its bit on D0 (once its pattern has opened it)
    uint8_t bit = 0;
    const bool clockAnswers = m_clock.access(a, false, bit);
    if ((a & 0xFFFC) == BASE) {
        d = m_hdc.read(a & 3);
        return true;
    }
    if (clockAnswers) d = bit;      // D1-D7 undriven (unverified: 0 here)
    return clockAnswers;
}

bool BurkeBurke::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (cts) {
        // The BIOS ROM socket: A4-A6 inverted by the interface (schematic: U3D-F)
        if (m_rom.empty()) return false;
        d = m_rom[(size_t)((a ^ 0x0070) & 0x3FFF) % m_rom.size()];
        return true;
    }
    if (!scs || (a & 0xFFFC) != BASE) return false;
    d = m_hdc.peek(a & 3);
    return true;
}

void BurkeBurke::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (!scs) return;
    // A write cycle with A2 low is a DS1215 write too (/WE = A2); with A2
    // high it's neither (/OE needs R/W high)
    uint8_t unused = 0;
    if (!(a & 0x04)) m_clock.access(a, true, unused);
    if ((a & 0xFFFC) == BASE) m_hdc.write(a & 3, d);
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

// ---------------------------------------------------------------------------
// Disto Super Controller II
// ---------------------------------------------------------------------------
DistoSc2::DistoSc2(std::vector<uint8_t> dos, bool altRegisters) : Fd500(std::move(dos)), m_alt(altRegisters) {
    m_fdc->setVariant(Wd17xx::Variant::WD1773);
    m_fdc->setName("WD1773");
}

std::string DistoSc2::describe() const {
    std::string s = std::string("Disto Super Controller II (WD1773, no-halt buffer at ") + (m_alt ? "$FF58" : "$FF74") + ")";
    if (m_meb) s += "; MEB: " + m_meb->describe();
    for (int i = 0; i < 4; ++i)
        if (!m_paths[i].empty()) s += "; drive " + std::to_string(i) + ": " + leaf(m_paths[i]);
    return s;
}

void DistoSc2::reset() {
    // The control register (a 74HC175) and the counter clear with RESET; the 6116 keeps its bytes
    m_ctrl = 0;
    m_count = 0;
    Fd500::reset();
    if (m_meb) m_meb->reset();
    updateCart();
}

void DistoSc2::plugMeb(std::unique_ptr<MebDevice> dev) {
    m_meb = std::move(dev);
    m_mebFirq = false;
    if (m_meb) m_meb->setFirq = [this](bool on) { m_mebFirq = on; updateCart(); };
    updateCart();
}

int DistoSc2::bufferRegister(uint16_t a, bool cts, bool scs) const {
    if (cts) return -1;
    if (m_alt) return scs && (a & 0x18) == 0x18 ? (a >> 1) & 1 : -1;
    return !scs && a >= 0xFF74 && a <= 0xFF77 ? (a >> 1) & 1 : -1;
}

bool DistoSc2::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    const int r = bufferRegister(a, cts, scs);
    if (r == 0) { d = m_buf[m_count++]; return true; }
    if (r == 1) { d = controlRead(); return true; }
    if (scs && (a & 0x18) == 0x10) {
        if (!m_meb) return false;
        d = m_meb->read(a & 7);
        return true;
    }
    if (scs && (a & 0x18) == 0x18) return false;     // J3 normal: $FF58-$FF5F not decoded
    return Fd500::read(a, cts, scs, d);
}

bool DistoSc2::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    const int r = bufferRegister(a, cts, scs);
    if (r == 0) { d = m_buf[m_count]; return true; }
    if (r == 1) { d = controlRead(); return true; }
    if (scs && (a & 0x18) == 0x10) {
        if (!m_meb) return false;
        d = m_meb->peek(a & 7);
        return true;
    }
    if (scs && (a & 0x18) == 0x18) return false;
    return Fd500::peek(a, cts, scs, d);
}

void DistoSc2::write(uint16_t a, bool cts, bool scs, uint8_t d) {
    const int r = bufferRegister(a, cts, scs);
    if (r == 0) { m_buf[m_count++] = d; return; }
    if (r == 1) { controlWrite(d); return; }
    if (scs && (a & 0x18) == 0x10) {
        if (m_meb) m_meb->write(a & 7, d);
        return;
    }
    if (scs && (a & 0x18) == 0x18) return;
    Fd500::write(a, cts, scs, d);
}

void DistoSc2::clock(uint64_t ticks) {
    Fd500::clock(ticks);
    if (m_meb) m_meb->clock(ticks);
}

void DistoSc2::controlWrite(uint8_t v) {
    m_ctrl = v;
    m_count = 0;
    updateHalt();
    if (setNmi) setNmi(m_intrq && (m_latch & 0x20) && nmiAllowed());
    updateCart();
    if ((m_ctrl & CTRL_BUFFERED) && m_drq) setDrq(true);     // a DRQ already up is served now
}

void DistoSc2::setDrq(bool on) {
    Fd500::setDrq(on);
    if (!on || !(m_ctrl & CTRL_BUFFERED)) return;
    // Buffered mode: the board answers the WD1773's DRQ itself, the counter counting
    if (m_ctrl & CTRL_READ) m_buf[m_count++] = m_fdc->readRegister(3);
    else m_fdc->writeRegister(3, m_buf[m_count++]);
}

void DistoSc2::setIntrq(bool on) {
    Fd500::setIntrq(on);
    updateCart();
}

void DistoSc2::updateCart() {
    const bool low = ((m_ctrl & CTRL_FIRQ) && m_intrq) || m_mebFirq;
    if (low == m_cartLow) return;
    m_cartLow = low;
    if (setCart) setCart(!low);
}

// ---------------------------------------------------------------------------
// Becker port
// ---------------------------------------------------------------------------
std::string BeckerCart::describe() const {
    std::string s = "Becker port ($FF41/$FF42) to the built-in DriveWire 4 server";
    if (!m_rom.empty()) s += ", ROM " + (m_label.empty() ? std::string("at $C000") : m_label);
    if (m_cow) s += ", copy on write";
    const std::string drives = m_dw.describe();
    if (!drives.empty()) s += "; " + drives;
    return s;
}

bool BeckerCart::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (cts || !scs) return peek(a, cts, scs, d);
    if (a == STATUS) {
        d = m_dw.hasOutput() ? 0x02 : 0x00;
        return true;
    }
    if (a == DATA) {
        d = m_dw.send();
        return true;
    }
    return false;
}

bool BeckerCart::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (cts) {
        if (m_rom.empty()) return false;
        d = m_rom[(a & 0x3FFF) % m_rom.size()];
        return true;
    }
    if (!scs) return false;
    if (a == STATUS) {
        d = m_dw.hasOutput() ? 0x02 : 0x00;
        return true;
    }
    if (a == DATA) {
        d = m_dw.peekOutput();
        return true;
    }
    return false;
}

void BeckerCart::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (scs && a == DATA) m_dw.receive(d);
}

void BeckerCart::clock(uint64_t ticks) {
    m_acc += ticks;
    if (m_acc < 14318) return;                      // in steps of about a millisecond
    const double s = (double)m_acc / m_hz;
    m_acc = 0;
    m_dw.advance(s);
    const auto& st = m_dw.stats();
    const uint64_t count = st.reads + st.writes + st.rereads;
    if (count != m_lastCount) {
        m_lastCount = count;
        m_sinceAccess = 0;
    } else {
        m_sinceAccess += s;
    }
}

bool BeckerCart::mountDisk(int drive, const std::string& path) {
    return m_dw.mount(drive, path, m_cow ? DriveWireServer::Mode::CopyOnWrite : DriveWireServer::Mode::WriteThrough);
}

BeckerCart* findBecker(Cart* port) {
    if (auto* b = dynamic_cast<BeckerCart*>(port)) return b;
    if (auto* mpi = dynamic_cast<MultiPak*>(port)) {
        for (int i = 0; i < 4; ++i)
            if (auto* b = dynamic_cast<BeckerCart*>(mpi->slot(i))) return b;
    }
    return nullptr;
}

bool BeckerCart::diskStatus(int drive, int& track, bool& motor) const {
    if (!m_dw.mounted(drive)) return false;
    const auto& st = m_dw.stats();
    const bool last = st.lastDrive == drive;
    track = last ? (int)(st.lastLsn / 18) : 0;
    motor = last && m_sinceAccess < 0.5;
    return true;
}

}  // namespace coco
