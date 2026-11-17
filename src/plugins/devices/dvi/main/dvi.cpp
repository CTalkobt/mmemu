// The Tandy Disk/Video Interface (see dvi.h; refs/dvi/FINDINGS.md has the
// service manual's details). mmsim's own code.

#include "dvi.h"
#include "plugins/devices/wd17xx/main/floppy_disk.h"
#include "plugins/z80/main/cpu_z80.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace {

constexpr size_t RAW_DVI_DISK = 40 * 18 * 256;      // 184320: a raw image, read as JVC

}  // namespace

class Dvi::MemBus : public IBus {
public:
    explicit MemBus(Dvi* d) : m_d(d), m_cfg{16, 8, BusRole::DATA, true, 0xFFFF} {}
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "dvi"; }
    uint8_t read8(uint32_t a) override { return m_d->memRead((uint16_t)a); }
    uint8_t peek8(uint32_t a) override { return m_d->memRead((uint16_t)a); }
    void write8(uint32_t a, uint8_t v) override { m_d->memWrite((uint16_t)a, v); }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    Dvi* m_d;
    BusConfig m_cfg;
};

class Dvi::IoBus : public IBus {
public:
    explicit IoBus(Dvi* d) : m_d(d), m_cfg{16, 8, BusRole::IO_PORT, true, 0xFFFF} {}
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "dvi-io"; }
    uint8_t read8(uint32_t a) override { return m_d->ioRead((uint16_t)a); }
    uint8_t peek8(uint32_t) override { return 0xFF; }
    void write8(uint32_t a, uint8_t v) override { m_d->ioWrite((uint16_t)a, v); }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

private:
    Dvi* m_d;
    BusConfig m_cfg;
};

Dvi::Dvi(std::vector<uint8_t> rom, std::vector<uint8_t> cg)
    : m_rom(std::move(rom)), m_cg(std::move(cg)), m_fdc(Wd17xx::Variant::FD1793, 0x40) {
    m_rom.resize(0x400, 0xFF);                  // A10, A11 grounded: the first 1 KB
    m_cg.resize(0x800, 0x00);                   // A11 grounded: the first 2 KB
    m_ram1.assign(0x800, 0);
    m_ram2.assign(0x800, 0);
    m_aram.assign(0x800, 0);
    m_cram.assign(0x800, 0x20);
    m_mem = std::make_unique<MemBus>(this);
    m_io = std::make_unique<IoBus>(this);
    m_cpu = std::make_unique<Z80>();
    m_cpu->setDataBus(m_mem.get());
    m_cpu->setIoBus(m_io.get());
    m_fdc.setName("DVI FDC");
    m_fdc.setClockHz((uint32_t)Z80_HZ);
    m_fdc.setFdcClockHz(1000000);
    m_fdc.setReadyMode(Wd17xx::ReadyMode::Always);       // READY tied high
    m_fdc.setDoubleDensity(true);                        // DDEN grounded
    for (int d = 0; d < 2; ++d) m_fdc.setDriveGeometry(d, 40, 1);
    m_fdc.onIntrq([this](bool on) { m_cpu->setNmiLine(on); });
    powerOn();
}

Dvi::~Dvi() { m_fdc.flush(); }

void Dvi::powerOn() {
    m_sts = 0;
    stsWrite(0);
    m_crtcAddr = 0;
    std::memset(m_crtc, 0, sizeof m_crtc);
    m_crtc[0] = 63;                  // a sane frame until the ROM programs it
    m_crtc[4] = 31;
    m_crtc[6] = 25;
    m_crtc[9] = 7;
    m_ctrl = 0x9B;
    m_outA = m_inA = m_portB = 0;
    m_obf = m_ibf = m_inte1 = m_inte2 = false;
    m_frameStart = m_time;
    m_fdcLead = 0;
    m_fdc.reset();
    m_cpu->reset();
    updateInt();
}

// --- Memory -----------------------------------------------------------------

uint8_t Dvi::memRead(uint16_t a) const {
    switch (a >> 13) {
    case 0:                                      // 0000-1FFF: M38 on A11, A12
        if (a < 0x0800) return bank1() ? m_ram1[a & 0x7FF] : m_rom[a & 0x3FF];
        if (a < 0x1000) return m_ram2[a & 0x7FF];
        return 0xFF;
    case 6: return (uint8_t)(0xFC | (m_aram[a & 0x7FF] & 0x03));   // ARAM: D2-D7 pulled up
    case 7: return m_cram[a & 0x7FF];
    default: return 0xFF;
    }
}

void Dvi::memWrite(uint16_t a, uint8_t v) {
    switch (a >> 13) {
    case 0:
        if (a < 0x0800) { if (bank1()) m_ram1[a & 0x7FF] = v; }
        else if (a < 0x1000) m_ram2[a & 0x7FF] = v;
        break;
    case 6: m_aram[a & 0x7FF] = v & 0x03; break;
    case 7: m_cram[a & 0x7FF] = v; break;
    default: break;
    }
}

// --- I/O ----------------------------------------------------------------------

uint8_t Dvi::ioRead(uint16_t port) {
    const uint8_t p = (uint8_t)port;
    if (p & 0x80) return 0xFF;
    switch ((p >> 5) & 3) {
    case 0:                                     // CRTC: R/W = A0, RS = A1
        if ((p & 3) == 3) return (m_crtcAddr >= 14 && m_crtcAddr <= 15) ? m_crtc[m_crtcAddr] : 0x00;
        return 0xFF;
    case 1: {                                   // STS
        uint8_t v = (uint8_t)(m_portB & 0x0F);
        v |= 0x10;                              // D4 not driven
        if (vsret()) v |= 0x20;
        if (m_ibf) v |= 0x40;
        if (m_fdc.headLoaded()) v |= 0x80;      // MOTOR ON = HLD
        return v;
    }
    case 2:                                     // FDC
        if (!(p & 0x10)) fdcWait();
        return m_fdc.readRegister(p & 3);
    default:                                    // 8255: /ACK
        m_obf = false;
        updateInt();
        return m_outA;
    }
}

void Dvi::ioWrite(uint16_t port, uint8_t v) {
    const uint8_t p = (uint8_t)port;
    if (p & 0x80) return;
    switch ((p >> 5) & 3) {
    case 0:
        if (p & 1) return;                      // R/W high: a read cycle on the CRTC
        if (p & 2) { if (m_crtcAddr < 16) m_crtc[m_crtcAddr] = v; }
        else m_crtcAddr = v & 0x1F;
        return;
    case 1: stsWrite(v); return;
    case 2:
        if (!(p & 0x10)) fdcWait();
        m_fdc.writeRegister(p & 3, v);
        return;
    default:                                    // 8255: /STB
        m_inA = v;
        m_ibf = true;
        return;
    }
}

void Dvi::stsWrite(uint8_t v) {
    m_sts = v;
    const int drive = (v & 0x10) ? 0 : (v & 0x20) ? 1 : -1;
    m_fdc.selectDrive(drive);
}

// An access at 40H-4FH: M1 holds /WAIT until DRQ or INTRQ. The FDC runs on
// meanwhile; the time is the Z80's (m_fdcLead keeps it from running twice).
void Dvi::fdcWait() {
    uint64_t waited = 0;
    while (!m_fdc.drq() && !m_fdc.intrq() && waited < (uint64_t)Z80_HZ * 4) {
        tickFdc(64);
        m_fdcLead += 64;
        waited += 64;
    }
}

void Dvi::tickFdc(uint64_t cycles) {
    while (cycles) {
        const uint64_t n = std::min<uint64_t>(cycles, 512);
        m_fdc.setMotor(m_fdc.headLoaded());     // the drives' motors run on HLD
        m_fdc.tick(n);
        cycles -= n;
    }
    m_fdc.setMotor(m_fdc.headLoaded());
}

void Dvi::updateInt() { m_cpu->setIrqLine(m_obf); }

// --- The portable's side --------------------------------------------------------

uint8_t Dvi::hostPeek(uint8_t port) const {
    switch (port & 3) {
    case 0: return m_inA;
    case 1: return m_portB;
    case 2: {
        uint8_t v = 0;                           // PC0-PC2: the box's reset line, low while it runs
        if (!m_obf) v |= 0x80;                   // /OBF
        if (m_inte1) v |= 0x40;
        if (m_ibf) v |= 0x20;
        if (m_inte2) v |= 0x10;
        if ((m_inte1 && !m_obf) || (m_inte2 && m_ibf)) v |= 0x08;    // INTRA
        return v;
    }
    default: return 0xFF;
    }
}

uint8_t Dvi::hostIn(uint8_t port) {
    const uint8_t v = hostPeek(port);
    if ((port & 3) == 0) m_ibf = false;          // reading port A empties the input latch
    return v;
}

void Dvi::hostOut(uint8_t port, uint8_t v) {
    switch (port & 3) {
    case 0:                                      // port A: to the box
        m_outA = v;
        m_obf = true;
        updateInt();
        break;
    case 1: m_portB = v; break;
    case 2: break;                               // port C: inputs / handshake in mode 2
    default:
        if (v & 0x80) {                          // mode set
            m_ctrl = v;
            m_obf = m_ibf = false;
            m_inte1 = m_inte2 = false;
            m_portB = 0;
            updateInt();
        } else {                                 // port C bit set/reset: INTE1 = PC6, INTE2 = PC4
            const int bit = (v >> 1) & 7;
            const bool on = v & 1;
            if (bit == 6) m_inte1 = on;
            if (bit == 4) m_inte2 = on;
        }
        break;
    }
}

// --- Time ---------------------------------------------------------------------

int Dvi::frameLines() const {
    const int n = (m_crtc[4] + 1) * ((m_crtc[9] & 0x1F) + 1) + (m_crtc[5] & 0x1F);
    return std::max(n, 1);
}

int Dvi::displayLines() const { return m_crtc[6] * ((m_crtc[9] & 0x1F) + 1); }

bool Dvi::vsret() const {
    const uint64_t lc = std::max<uint64_t>(lineCycles(), 1);
    const int line = (int)(((m_time - m_frameStart) / lc) % (uint64_t)frameLines());
    return line >= displayLines();
}

uint64_t Dvi::cyclesToRetrace() const {
    const uint64_t lc = std::max<uint64_t>(lineCycles(), 1);
    const uint64_t frame = lc * (uint64_t)frameLines();
    const uint64_t at = (m_time - m_frameStart) % frame;
    const uint64_t retrace = lc * (uint64_t)displayLines();
    return at < retrace ? retrace - at : 0;
}

void Dvi::advance(uint64_t ticks) {
    m_frac += (double)ticks * Z80_HZ / m_hostHz;
    uint64_t budget = (uint64_t)m_frac;
    m_frac -= (double)budget;
    while (budget) {
        uint64_t t;
        if ((m_sts & 0x40) && !vsret()) {
            t = std::max<uint64_t>(std::min<uint64_t>(cyclesToRetrace(), budget), 1);   // BUSRQ: held off the bus
        } else {
            m_fdcLead = 0;
            t = (uint64_t)m_cpu->step();
            if (t == 0) t = 4;
            t += m_fdcLead;                       // the wait at 40H-4FH
        }
        tickFdc(t > m_fdcLead ? t - m_fdcLead : 0);
        m_fdcLead = 0;
        m_time += t;
        budget = t >= budget ? 0 : budget - t;
    }
}

// --- Disks --------------------------------------------------------------------

bool Dvi::mountDisk(int drive, const std::string& path, std::string* err) {
    if (drive < 0 || drive > 1) return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { if (err) *err = "can't open '" + path + "'"; return false; }
    const auto size = (size_t)f.tellg();
    auto d = FloppyDisk::load(path, err, size == RAW_DVI_DISK ? FloppyFormat::JVC : FloppyFormat::Auto);
    if (!d) return false;
    m_fdc.insertDisk(drive, std::move(d));
    return true;
}

void Dvi::ejectDisk(int drive) { m_fdc.removeDisk(drive); }

std::string Dvi::diskPath(int drive) const {
    FloppyDisk* d = m_fdc.disk(drive);
    return d ? d->path() : "";
}

void Dvi::flush() { m_fdc.flush(); }

// --- Video --------------------------------------------------------------------

void Dvi::render(uint32_t* out) const {
    constexpr uint32_t BLACK = 0xFF000000, WHITE = 0xFFE8E8E8;
    std::fill(out, out + (size_t)OUT_W * OUT_H, BLACK);
    const int cols = std::min<int>(m_crtc[1], cols80() ? 80 : 40);
    const int rows = std::min<int>(m_crtc[6], 25);
    const int scan = (m_crtc[9] & 0x1F) + 1;
    const int dotW = cols80() ? 1 : 2;
    const uint16_t start = (uint16_t)((m_crtc[12] << 8) | m_crtc[13]);
    const uint16_t cursor = (uint16_t)((m_crtc[14] << 8) | m_crtc[15]);
    const int curMode = (m_crtc[10] >> 5) & 3;         // 0 steady, 1 off, 2/3 blink
    const int curStart = m_crtc[10] & 0x1F, curEnd = m_crtc[11] & 0x1F;
    const uint64_t frame = std::max<uint64_t>(lineCycles() * (uint64_t)frameLines(), 1);
    const uint64_t frames = m_time / frame;
    const bool blinkOn = (frames / 32) & 1;            // VSYNC / 64: about 1 Hz
    const bool curBlink = curMode == 2 ? ((frames / 16) & 1) : curMode == 3 ? ((frames / 32) & 1) : true;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const uint16_t ma = (uint16_t)(start + r * m_crtc[1] + c);
            const uint8_t code = m_cram[ma & 0x7FF], attr = m_aram[ma & 0x7FF];
            const bool isCursor = (ma & 0x3FFF) == (cursor & 0x3FFF) && curMode != 1 && curBlink;
            for (int ra = 0; ra < scan && ra < 8 + 8; ++ra) {
                const int y = r * 8 + ra;
                if (ra >= 8 || y * 2 + 1 >= OUT_H) continue;     // RA3 = 1: blank
                uint8_t dots = m_cg[((size_t)code << 3) | (size_t)ra];
                // ARAM: bit 0 clear = reverse, bit 1 set = blink (01 is plain text: the boot ROM's fill)
                if (attr & 0x02 && blinkOn) dots = 0;
                if (!(attr & 0x01)) dots = (uint8_t)~dots;
                if (isCursor && ra >= curStart && ra <= curEnd) dots = (uint8_t)~dots;
                for (int b = 0; b < 8; ++b) {
                    if (!(dots & (0x80 >> b))) continue;
                    const int x = (c * 8 + b) * dotW;
                    for (int dx = 0; dx < dotW; ++dx) {
                        out[(size_t)(y * 2) * OUT_W + x + dx] = WHITE;
                        out[(size_t)(y * 2 + 1) * OUT_W + x + dx] = WHITE;
                    }
                }
            }
        }
}

std::string Dvi::rowText(int y) const {
    const int cols = std::min<int>(m_crtc[1], 80);
    const uint16_t start = (uint16_t)((m_crtc[12] << 8) | m_crtc[13]);
    std::string s;
    for (int c = 0; c < cols; ++c) {
        const uint8_t code = m_cram[(uint16_t)(start + y * m_crtc[1] + c) & 0x7FF];
        s += (code >= 0x20 && code < 0x7F) ? (char)code : '.';
    }
    return s;
}

// --- Snapshots ------------------------------------------------------------------

namespace {
void putBytes(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) { v.insert(v.end(), b.begin(), b.end()); }
}  // namespace

std::vector<uint8_t> Dvi::saveState() const {
    std::vector<uint8_t> v = {'D', 'V', 'I', 1};
    putBytes(v, m_ram1);
    putBytes(v, m_ram2);
    putBytes(v, m_aram);
    putBytes(v, m_cram);
    v.push_back(m_sts);
    v.push_back(m_crtcAddr);
    v.insert(v.end(), m_crtc, m_crtc + 16);
    v.push_back(m_ctrl);
    v.push_back(m_outA);
    v.push_back(m_inA);
    v.push_back(m_portB);
    v.push_back((uint8_t)(m_obf | m_ibf << 1 | m_inte1 << 2 | m_inte2 << 3));
    std::vector<uint8_t> cpu(m_cpu->stateSize());
    m_cpu->saveState(cpu.data());
    putBytes(v, cpu);
    return v;
}

bool Dvi::loadState(const std::vector<uint8_t>& s) {
    const size_t need = 4 + 4 * 0x800 + 2 + 16 + 5 + m_cpu->stateSize();
    if (s.size() < need || s[0] != 'D' || s[1] != 'V' || s[2] != 'I' || s[3] != 1) return false;
    size_t p = 4;
    for (auto* m : {&m_ram1, &m_ram2, &m_aram, &m_cram}) {
        std::copy(s.begin() + (long)p, s.begin() + (long)(p + 0x800), m->begin());
        p += 0x800;
    }
    m_sts = s[p++];
    stsWrite(m_sts);
    m_crtcAddr = s[p++];
    std::copy(s.begin() + (long)p, s.begin() + (long)(p + 16), m_crtc);
    p += 16;
    m_ctrl = s[p++];
    m_outA = s[p++];
    m_inA = s[p++];
    m_portB = s[p++];
    const uint8_t f = s[p++];
    m_obf = f & 1; m_ibf = f & 2; m_inte1 = f & 4; m_inte2 = f & 8;
    m_cpu->loadState(s.data() + p);
    updateInt();
    return true;
}
