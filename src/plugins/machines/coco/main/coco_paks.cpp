// Bank-switched ROM Paks (the Games Master Cartridge, Tandy's banked Paks)
// and the Dragon's Delta and Cumana disk cartridges. After XRoar (gmc.c,
// deltados.c, cart.c, rom.c, xroar.c; GPL-3.0-or-later) and MAME
// (bus/coco/coco_pak.cpp, dragon_fdc.cpp, hash/dragon_cart.xml); mmsim's own
// code.

#include "coco_paks.h"
#include "coco_system.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include "plugins/devices/sn76489/main/sn76489.h"
#include <fstream>

namespace coco {

namespace {

constexpr uint64_t CART_HALF_PERIOD = 7159;     // CART on Q: a 1 kHz square wave (as RomPak)
constexpr size_t MAX_BANKED = 0x40000;          // XRoar's limit (256 KB)

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// An image padded with $FF to a power of two (XRoar's rombank)
std::vector<uint8_t> padPow2(std::vector<uint8_t> v, size_t limit) {
    if (v.size() > limit) v.resize(limit);
    size_t n = 1;
    while (n < v.size()) n <<= 1;
    v.resize(n, 0xFF);
    return v;
}

// XRoar's rom.c: carts it knows by CRC-32 and size (4 KB multiples)
struct Fingerprint {
    uint32_t crc;
    size_t size;
    const char* type;
};
const Fingerprint KNOWN[] = {
    // CoCo 3: a whole unbanked 32 KB
    {0x2fab4955, 0x8000, "rompak"},     // Arkanoid
    {0x899978e7, 0x8000, "rompak"},     // GFL Championship Football II
    {0xc8b64049, 0x8000, "rompak"},     // RAD Warrior
    {0x09c2e97d, 0x8000, "rompak"},     // Rampage!
    {0xccfd0a0c, 0x8000, "rompak"},     // Silpheed
    {0xe8e54cbe, 0x8000, "rompak"},     // Super Pitfall
    // Banked (GMC-compatible)
    {0x83bd6056, 0x8000, "gmc"},        // Mind Roll
    {0x878906fe, 0x8000, "gmc"},        // Mind Roll [f plane1]
    {0xa9680ede, 0x10000, "gmc"},       // Predator
    {0xdd94dd06, 0x20000, "gmc"},       // RoboCop
    {0xabe7bb9e, 0x4000, "gmc"},        // Blockdown (16 KB, its SN76489)
    {0x58716b7f, 0x10000, "gmc"},       // Dunjunz
};

// The first of `names` (as given, or with .rom / .bin / .dgn) of one of `sizes` in `dir`
bool findRom(const std::string& dir, const std::vector<std::string>& names, const std::vector<size_t>& sizes,
             std::vector<uint8_t>& out) {
    for (const auto& n : names)
        for (const char* ext : {".rom", ".bin", ".dgn", ""})
            if (readFile(dir + "/" + n + ext, out))
                for (size_t s : sizes)
                    if (out.size() == s) return true;
    out.clear();
    return false;
}

// A disk cartridge's ROM: `path`, or the first of `names` in the Dragon firmware directory
std::vector<uint8_t> dosRom(const std::string& path, const std::vector<std::string>& names, const std::vector<size_t>& sizes,
                            const std::string& what, std::string& err) {
    std::vector<uint8_t> rom;
    if (!path.empty()) {
        if (!readFile(path, rom) || rom.empty()) err = "can't read " + what + " ROM " + path;
        return rom;
    }
    const std::string dir = dragonFirmwareDir();
    if (findRom(dir, names, sizes, rom)) return rom;
    err = what + " ROM not found: put " + names[0] + ".rom in " + dir + "/ (or set MMEMU_DRAGON_FIRMWARE), or give its path";
    return rom;
}

}  // namespace

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// ---------------------------------------------------------------------------
// Banked ROM Paks
// ---------------------------------------------------------------------------
BankedPak::BankedPak(std::vector<uint8_t> image, std::string label, Scheme scheme)
    : m_rom(padPow2(std::move(image), MAX_BANKED)), m_label(std::move(label)), m_scheme(scheme) {
    if (m_scheme == Scheme::Gmc) m_psg.reset(new bbc::Sn76489());
}

BankedPak::~BankedPak() = default;

std::string BankedPak::describe() const {
    const std::string kb = std::to_string(m_rom.size() / 1024) + " KB";
    if (m_scheme == Scheme::Gmc) return "Games Master Cartridge (" + m_label + ", " + kb + ", SN76489)";
    return "Banked ROM Pak (" + m_label + ", " + kb + ", bank at $FF40)";
}

void BankedPak::reset() {
    m_bank = 0;
    if (m_psg) m_psg->reset();
    m_level = 0.0f;
    m_cartLevel = true;
    m_cartAcc = 0;
    if (setCart) setCart(true);
}

bool BankedPak::peek(uint16_t a, bool cts, bool, uint8_t& d) const {
    if (!cts || m_rom.empty()) return false;
    d = m_rom[(m_bank | (a & 0x3FFFu)) & (m_rom.size() - 1)];
    return true;
}

void BankedPak::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (!scs) return;
    if (m_scheme == Scheme::Gmc) {
        if (!(a & 1)) m_bank = ((uint32_t)d << 14) & ((uint32_t)(m_rom.size() - 1) & 0x3C000u);
        else m_psg->write(d);
        return;
    }
    if (a == 0xFF40) m_bank = (uint32_t)(((uint64_t)d * 0x4000u) % m_rom.size()) & ~0x3FFFu;
}

void BankedPak::clock(uint64_t ticks) {
    m_cartAcc += ticks;
    while (m_cartAcc >= CART_HALF_PERIOD) {
        m_cartAcc -= CART_HALF_PERIOD;
        m_cartLevel = !m_cartLevel;
        if (setCart) setCart(m_cartLevel);
    }
    if (!m_psg) return;
    // The SN76489 runs from its own 4 MHz (bbc::Sn76489 counts 2 MHz cycles)
    const uint64_t hz = (uint64_t)m_hz;
    m_psgAcc += ticks * 2000000u;
    const uint64_t cycles = m_psgAcc / hz;
    m_psgAcc %= hz;
    if (!cycles) return;
    m_psg->advance(cycles);
    float buf[64];
    int n;
    while ((n = m_psg->pull(buf, 64)) > 0) m_level = buf[n - 1];
}

// ---------------------------------------------------------------------------
// DeltaDOS
// ---------------------------------------------------------------------------
DeltaDos::DeltaDos(std::vector<uint8_t> rom)
    : m_fdc(new Wd17xx(Wd17xx::Variant::WD2791, 0xFF40)), m_rom(std::move(rom)) {
    m_fdc->setClockHz(14318180);
    m_fdc->setReadyMode(Wd17xx::ReadyMode::Always);
}

DeltaDos::~DeltaDos() { m_fdc->flush(); }

std::string DeltaDos::describe() const {
    std::string s = "Premier Delta disk system (WD2791)";
    for (int i = 0; i < 4; ++i)
        if (!m_paths[i].empty()) s += "; drive " + std::to_string(i + 1) + ": " + baseName(m_paths[i]);
    return s;
}

void DeltaDos::setClockHz(double hz) { m_fdc->setClockHz((uint32_t)hz); }

void DeltaDos::reset() {
    // Idle after reset, as DragonDOS's: the master reset's Restore ended by a force
    // interrupt (written complemented: the 2791's bus is inverted)
    m_fdc->reset();
    m_fdc->writeRegister(0, (uint8_t)~0xD0);
    latchWrite(0);
}

bool DeltaDos::read(uint16_t a, bool cts, bool scs, uint8_t& d) {
    if (cts) return peek(a, cts, scs, d);
    if (!scs || (a & 4)) return false;
    d = m_fdc->readRegister(a & 3);
    return true;
}

bool DeltaDos::peek(uint16_t a, bool cts, bool scs, uint8_t& d) const {
    if (cts) {
        if (m_rom.empty()) return false;
        d = m_rom[(a & 0x3FFF) % m_rom.size()];
        return true;
    }
    if (!scs || (a & 4)) return false;
    d = m_fdc->peekRegister(a & 3);
    return true;
}

void DeltaDos::write(uint16_t a, bool, bool scs, uint8_t d) {
    if (!scs) return;
    if (!(a & 4)) m_fdc->writeRegister(a & 3, d);
    else latchWrite(d);
}

void DeltaDos::clock(uint64_t ticks) { m_fdc->tick(ticks); }

bool DeltaDos::mountDisk(int drive, const std::string& path) {
    if (drive < 0 || drive > 3 || !m_fdc->mountDisk(drive, path)) return false;
    m_paths[drive] = path;
    return true;
}

bool DeltaDos::ejectDisk(int drive) {
    if (drive < 0 || drive > 3 || m_paths[drive].empty()) return false;
    m_fdc->ejectDisk(drive);
    m_paths[drive].clear();
    return true;
}

std::string DeltaDos::diskPath(int drive) const { return drive >= 0 && drive < 4 ? m_paths[drive] : ""; }

bool DeltaDos::diskStatus(int drive, int& track, bool& motor) const {
    if (drive < 0 || drive > 3 || m_paths[drive].empty()) return false;
    track = m_fdc->driveCylinder(drive);
    motor = m_fdc->selectedDrive() == drive;
    return true;
}

void DeltaDos::latchWrite(uint8_t v) {
    m_latch = v;
    m_fdc->selectDrive(v & 3);
    m_fdc->setSide((v & 4) ? 1 : 0);
    m_fdc->setFdcClockHz((v & 8) ? 2000000 : 1000000);
    m_fdc->setDoubleDensity(v & 0x10);
    m_fdc->setMotor(true);
}

// ---------------------------------------------------------------------------
// Making them
// ---------------------------------------------------------------------------
std::string romCartType(const std::vector<uint8_t>& image) {
    std::vector<uint8_t> v = image;
    while (v.size() & 0xFFF) v.push_back(0xFF);
    const uint32_t c = crc32(v.data(), v.size());
    for (const auto& k : KNOWN)
        if (k.crc == c && k.size == v.size()) return k.type;
    return image.size() > 0x4000 ? "gmc" : "rompak";
}

std::unique_ptr<Cart> makeRomCart(std::vector<uint8_t> image, const std::string& label, const std::string& type,
                                  std::string& err) {
    if (image.empty()) { err = "empty image"; return nullptr; }
    const std::string t = type.empty() ? romCartType(image) : type;
    if (t == "gmc") return std::unique_ptr<Cart>(new BankedPak(std::move(image), label, BankedPak::Scheme::Gmc));
    if (t == "banked") return std::unique_ptr<Cart>(new BankedPak(std::move(image), label, BankedPak::Scheme::Tandy));
    if (t == "rompak") return std::unique_ptr<Cart>(new RomPak(std::move(image), label));
    err = "no ROM cartridge type '" + t + "' (rompak, gmc, banked)";
    return nullptr;
}

std::unique_ptr<Cart> makePakCart(const std::string& device, const std::string& option, std::string& err) {
    if (device == "gmc" || device == "banked") {
        std::vector<uint8_t> rom;
        if (option.empty() || !readFile(option, rom) || rom.empty()) { err = device + " needs the image's path"; return nullptr; }
        return makeRomCart(std::move(rom), baseName(option), device, err);
    }
    if (device == "deltados") {
        // XRoar's romlist "delta": Delta System 2.0 / 1A (8 KB)
        auto rom = dosRom(option, {"delta2", "delta1a", "delta19", "delta", "deltados"}, {0x2000, 0x4000}, "DeltaDOS", err);
        if (rom.empty()) return nullptr;
        return std::unique_ptr<Cart>(new DeltaDos(std::move(rom)));
    }
    if (device == "cumana") {
        // Cumana DOS 2.0 (16 KB) on DragonDOS-compatible hardware (XRoar's "dragondos_compat",
        // MAME's dragon_fdc slot)
        auto rom = dosRom(option, {"cdos20", "CDOS20", "CumanaDOSv2.0"}, {0x4000, 0x2000}, "Cumana DOS", err);
        if (rom.empty()) return nullptr;
        return std::unique_ptr<Cart>(new DragonDos(std::move(rom), "Cumana DOS"));
    }
    if (device == "superdos" || device == "dosplus") {
        // DragonDOS-compatible DOSes on DragonDOS's own hardware (XRoar's romlists superdos,
        // dosplus in "dragondos_compat"): only the ROM differs
        const bool sd = device == "superdos";
        auto rom = sd ? dosRom(option, {"sdose6", "PNP - SuperDOS E6", "sdose8", "sdose5", "sdose4"}, {0x2000, 0x4000}, "SuperDOS", err)
                      : dosRom(option, {"dplus49b", "dplus48", "dosplus-4.8", "DOSPLUS", "dplus50"}, {0x2000, 0x4000}, "DOSplus", err);
        if (rom.empty()) return nullptr;
        return std::unique_ptr<Cart>(new DragonDos(std::move(rom), sd ? "SuperDOS" : "DOSplus"));
    }
    return nullptr;
}

}  // namespace coco
