// The Dragon Professional (Alpha): PIA2, the AY-3-8912 and the built-in
// WD2797 on a Dragon 64's board. The wiring after XRoar (dragon/dragonpro.c;
// GPL-3.0-or-later) and MAME (trs/dgnalpha.cpp, from Phill Harvey-Smith's
// traces); mmsim's own code.

#include "dragon_pro.h"
#include "coco_system.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"

namespace coco {

namespace {
constexpr uint64_t AY_HZ = 1000000;        // the PSG's clock
constexpr uint64_t AY_STEP = 16;           // SpectrumAy::step() is 16 of its clocks
}

DragonProHw::DragonProHw() : fdc(new Wd17xx(Wd17xx::Variant::WD2797, 0xFF2C)) {
    fdc->setReadyMode(Wd17xx::ReadyMode::Always);
    disk.reset(new DragonProDisk(this));
}

DragonProHw::~DragonProHw() { fdc->flush(); }

// Port A is an output only with R7 bit 6 set; as an input the pull-ups hold it high
void DragonProHw::diskControl() {
    ioa = (ay.reg(7) & 0x40) ? ay.reg(14) : 0xFF;
    const int drive = (ioa & 1) ? 0 : (ioa & 2) ? 1 : (ioa & 4) ? 2 : (ioa & 8) ? 3 : 0;
    fdc->selectDrive(drive);
    fdc->setMotor(ioa & 0x10);
    fdc->setDoubleDensity(!(ioa & 0x20));
    fdc->setFdcClockHz((ioa & 0x80) ? 2000000 : 1000000);
}

// BDIR/BC1: 11 latch the register, 10 write it, 01 read it onto port B, 00 idle
void DragonProHw::psgCycle() {
    const uint8_t pa = pia2.portA();
    const bool bdir = pa & 1, bc1 = pa & 2;
    pia2.b.inSink = pia2.b.inSource = 0xFF;
    const uint8_t d = pia2.portB();
    if (bdir && bc1) {
        ay.selectRegister(d);
    } else if (bdir) {
        ay.write(d);
        if (ay.current() == 7 || ay.current() == 14) diskControl();
    } else if (bc1) {
        const uint8_t r = ay.current() == 14 ? ioa : ay.read();
        pia2.b.inSink = pia2.b.inSource = r;
    }
}

// ---------------------------------------------------------------------------
// On the board
// ---------------------------------------------------------------------------
void System::wireDragonPro() {
    m_pro.reset(new DragonProHw());
    DragonProHw* p = m_pro.get();
    p->fdc->setClockHz((uint32_t)m_masterHz);
    p->pia2.irqDelay = PIA_IRQ_DELAY;
    p->pia2.onIrq = [this]() { updateLines(); };
    p->pia2.a.postWrite = [this]() {
        m_pro->psgCycle();
        dragonRomSelect();
    };
    // DRQ is PIA2 CB1; INTRQ rising reaches /NMI while CA2 (the NMI enable) is high
    p->fdc->onDrq([this](bool on) {
        m_pro->pia2.advance(m_dev);
        m_pro->pia2.setCb1(on);
    });
    p->fdc->onIntrq([this](bool on) {
        if (!on) m_proNmi = false;
        else if (m_pro->pia2.ca2()) m_proNmi = true;
        proNmiUpdate();
    });
}

void System::proNmiUpdate() {
    if (m_cpu) m_cpu->setNmiLine(m_nmi || m_proNmi);
}

void System::proReset() {
    DragonProHw* p = m_pro.get();
    p->pia2.reset();
    p->pia2.a.inSink = p->pia2.b.inSink = 0xFF;
    p->pia2.b.inSource = 0;
    p->ay.reset();
    p->ayLevel = 0.0f;
    p->diskControl();
    // Idle after reset (no Restore, so no INTRQ before the boot ROM is up)
    p->fdc->reset();
    p->fdc->writeRegister(0, 0xD0);
    m_proNmi = false;
}

bool System::proRead(uint16_t a, uint8_t& d) {
    if ((a & 0x0C) == 0x0C) {
        d = m_pro->fdc->readRegister(~a & 3);       // reversed: $FF2F is the command/status register
        return true;
    }
    if (a & 4) {
        d = m_pro->pia2.read(a);
        return true;
    }
    return false;
}

bool System::proPeek(uint16_t a, uint8_t& d) const {
    if ((a & 0x0C) == 0x0C) {
        d = m_pro->fdc->peekRegister(~a & 3);
        return true;
    }
    if (a & 4) {
        d = m_pro->pia2.peek(a);
        return true;
    }
    return false;
}

bool System::proWrite(uint16_t a, uint8_t v) {
    if ((a & 0x0C) == 0x0C) {
        m_pro->fdc->writeRegister(~a & 3, v);
        return true;
    }
    if (a & 4) {
        m_pro->pia2.write(a, v);
        soundChanged();
        return true;
    }
    return false;
}

void System::proClock(uint64_t ticks) {
    DragonProHw* p = m_pro.get();
    p->fdc->tick(ticks);
    p->pia2.advance(m_dev);
    const uint64_t period = (uint64_t)m_masterHz * AY_STEP;
    p->ayAcc += ticks * AY_HZ;
    if (p->ayAcc < period) return;
    while (p->ayAcc >= period) {
        p->ayAcc -= period;
        p->ayLevel = p->ay.step();
    }
    soundChanged();
}

float System::proSound() const { return m_pro ? m_pro->ayLevel - 0.5f : 0.0f; }

bool System::proFirq() const { return m_pro && m_pro->pia2.irq(); }

// ---------------------------------------------------------------------------
// Its drives for the CLI / GUI
// ---------------------------------------------------------------------------
bool DragonProDisk::mountDisk(int unit, const std::string& path) {
    const int drive = unit - 1;
    if (drive < 0 || drive > 3 || !m_hw->fdc->mountDisk(drive, path)) return false;
    m_hw->paths[drive] = path;
    return true;
}

void DragonProDisk::ejectDisk(int unit) {
    const int drive = unit - 1;
    if (drive < 0 || drive > 3 || m_hw->paths[drive].empty()) return;
    m_hw->fdc->ejectDisk(drive);
    m_hw->paths[drive].clear();
}

bool DragonProDisk::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    const int drive = unit - 1;
    sector = 0;
    if (drive < 0 || drive > 3 || m_hw->paths[drive].empty()) return false;
    track = m_hw->fdc->driveCylinder(drive);
    led = m_hw->fdc->motor() && m_hw->fdc->selectedDrive() == drive;
    return true;
}

std::string DragonProDisk::getMountedDiskPath(int unit) {
    const int drive = unit - 1;
    return drive >= 0 && drive < 4 ? m_hw->paths[drive] : "";
}

void DragonProDisk::getDeviceInfo(DeviceInfo& out) const {
    IOHandler::getDeviceInfo(out);
    out.state.push_back({"controller", "WD2797 at $FF2C-$FF2F (registers reversed)"});
    out.state.push_back({"psg port A", std::to_string(m_hw->ioa)});
    for (int i = 0; i < 4; ++i)
        if (!m_hw->paths[i].empty()) out.state.push_back({"drive " + std::to_string(i + 1), m_hw->paths[i]});
}

}  // namespace coco
