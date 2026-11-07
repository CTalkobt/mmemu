// C128 cartridges, after VICE src/c128/cart/ (generic.c, comal80.c,
// magicdesk128.c, warpspeed128.c, partner128.c, gmod2c128.c, c128cart.c)
// and src/core/flash040core.c, m93c86.c; GPL-2.0-or-later, VICE team.

#include "c128_carts.h"
#include <algorithm>
#include <cstring>

namespace c128 {

namespace {

uint32_t be16(const std::vector<uint8_t>& img, size_t o) { return (uint32_t)(img[o] << 8 | img[o + 1]); }

struct Chip { uint16_t bank, start, size; size_t data; };

// The CHIP packets of a .crt
bool crtChips(const std::vector<uint8_t>& img, std::vector<Chip>& chips, std::string& err) {
    size_t o = std::max<uint32_t>(be16(img, 0x10) << 16 | be16(img, 0x12), 0x40);
    while (o + 16 <= img.size() && std::memcmp(&img[o], "CHIP", 4) == 0) {
        const uint32_t len = be16(img, o + 4) << 16 | be16(img, o + 6);
        Chip c{(uint16_t)be16(img, o + 10), (uint16_t)be16(img, o + 12), (uint16_t)be16(img, o + 14), o + 16};
        if (o + 16 + c.size > img.size() || len < 16) { err = "truncated CHIP packet"; return false; }
        chips.push_back(c);
        o += len;
    }
    if (chips.empty()) { err = "no CHIP packets"; return false; }
    return true;
}

// A raw image of `size` bytes, after a 2-byte load address if there is one
// (util_file_load with UTIL_FILE_LOAD_SKIP_ADDRESS)
bool rawOfSize(const std::vector<uint8_t>& img, size_t size, std::vector<uint8_t>& out) {
    if (img.size() != size && img.size() != size + 2) return false;
    out.assign(img.end() - (long)size, img.end());
    return true;
}

// ---------------------------------------------------------------------------
// Generic (generic.c): 32 KB, banked in by CR
// ---------------------------------------------------------------------------
class GenericCart : public C128Cart {
public:
    GenericCart(std::vector<uint8_t> rom, std::string type) : m_rom(std::move(rom)), m_type(std::move(type)) {}
    std::string type() const override { return m_type; }
    uint8_t romRead(uint16_t a, bool) override { return m_rom[a & 0x7FFF]; }
private:
    std::vector<uint8_t> m_rom;
    std::string m_type;
};

// ---------------------------------------------------------------------------
// Banks of 32 KB (VICE's ext_function_rom with ext_function_rom_bank)
// ---------------------------------------------------------------------------
class BankedCart : public C128Cart {
protected:
    explicit BankedCart(int banks) : m_rom((size_t)banks * 0x8000, 0xFF) {}
    uint8_t bankByte(uint16_t a) const { return m_rom[(size_t)m_bank * 0x8000 + (a & 0x7FFF)]; }
    std::vector<uint8_t> m_rom;
    int m_bank = 0;
};

// Comal 80 (comal80.c): 6 x 16 KB at $C000; $DE00 bits 6-5 pick the chip
// (U1, -, U2, U3), bit 4 its half
class Comal80Cart : public BankedCart {
public:
    explicit Comal80Cart(const std::vector<uint8_t>& raw) : BankedCart(6) {
        for (int n = 0; n < 6; ++n)
            std::copy(raw.begin() + n * 0x4000, raw.begin() + (n + 1) * 0x4000, m_rom.begin() + 0x4000 + 0x8000 * n);
        store(0);
    }
    std::string type() const override { return "Comal 80 (C128)"; }
    uint8_t romRead(uint16_t a, bool) override { return bankByte(a); }
    bool io1(uint16_t a, const uint8_t* wr, bool peek, uint8_t& v) override {
        if (wr) { store(*wr); return false; }
        if (peek) v = m_rom[0x1E00 + (a & 0xFF)];
        else v = (uint8_t)(m_reg | (((m_reg >> 4) & 1) << 4));
        return true;
    }
    bool io2(uint16_t a, const uint8_t* wr, bool, uint8_t& v) override {
        if (wr) return false;
        v = m_rom[0x1F00 + (a & 0xFF)];
        return true;
    }
    void reset() override { store(0); }
private:
    void store(uint8_t v) {
        static const int ROMOFFSET[4] = {0, 0, 2, 4};
        m_reg = v & 0xF0;
        m_bank = ROMOFFSET[(v >> 5) & 3] | ((v >> 4) & 1);
    }
    uint8_t m_reg = 0;
};

// Magic Desk 128 (magicdesk128.c): up to 64 x 16 KB at $8000; $DE00 picks
// the bank (bits 0-5, masked to the size). A read of $DE00 also latches
// what is on the bus (open here: $FF).
class MagicDesk128Cart : public BankedCart {
public:
    MagicDesk128Cart(const std::vector<uint8_t>& raw, int banks) : BankedCart(64) {
        for (int i = 0; i < banks; ++i)
            std::copy(raw.begin() + i * 0x4000, raw.begin() + (i + 1) * 0x4000, m_rom.begin() + 0x8000 * i);
        int p = 1;
        while (p < banks) p <<= 1;
        m_mask = p - 1;
    }
    std::string type() const override { return "Magic Desk 128"; }
    uint8_t romRead(uint16_t a, bool) override { return bankByte(a); }
    bool io1(uint16_t, const uint8_t* wr, bool peek, uint8_t& v) override {
        if (wr) { m_reg = *wr & 0x3F; m_bank = m_reg & m_mask; return false; }
        if (peek) { v = (uint8_t)m_bank; return true; }
        m_bank = 0xFF & m_mask;                     // the floating bus goes into the latch
        return false;
    }
    void reset() override { m_reg = 0; m_bank = 0; }
private:
    int m_mask = 0;
    uint8_t m_reg = 0;
};

// Warp Speed 128 (warpspeed128.c): 16 KB, mirrored to 32 KB; $DE00 and
// $DF00 read the ROM at $9E00 / $9F00
class WarpSpeed128Cart : public C128Cart {
public:
    explicit WarpSpeed128Cart(const std::vector<uint8_t>& raw) : m_rom(0x8000) {
        std::copy(raw.begin(), raw.begin() + 0x4000, m_rom.begin());
        std::copy(raw.begin(), raw.begin() + 0x4000, m_rom.begin() + 0x4000);
    }
    std::string type() const override { return "Warp Speed 128"; }
    uint8_t romRead(uint16_t a, bool) override { return m_rom[a & 0x7FFF]; }
    bool io1(uint16_t a, const uint8_t* wr, bool, uint8_t& v) override {
        if (wr) return false;
        v = m_rom[0x1E00 + (a & 0xFF)];
        return true;
    }
    bool io2(uint16_t a, const uint8_t* wr, bool, uint8_t& v) override {
        if (wr) return false;
        v = m_rom[0x1F00 + (a & 0xFF)];
        return true;
    }
private:
    std::vector<uint8_t> m_rom;
};

// Partner 128 (partner128.c): 16 KB ROM mirrored; $DE00-$DE7F a 128-byte
// window of 8 KB RAM, $DE80-$DEFF the ROM's $9E80-$9EFF and a register
// (bits 0-5 the RAM bank, bit 6 releases the freeze); $D600-$D60F writes
// also go to the RAM. Its freeze button raises NMI with the KERNAL's NMI
// vector forced to $DEDE until the release.
class Partner128Cart : public C128Cart {
public:
    explicit Partner128Cart(const std::vector<uint8_t>& raw) : m_rom(0x8000), m_ram(64 * 128, 0xFF) {
        std::copy(raw.begin(), raw.begin() + 0x4000, m_rom.begin());
        std::copy(raw.begin(), raw.begin() + 0x4000, m_rom.begin() + 0x4000);
    }
    std::string type() const override { return "Partner 128"; }
    uint8_t romRead(uint16_t a, bool) override { return m_rom[a & 0x7FFF]; }
    bool io1(uint16_t a, const uint8_t* wr, bool, uint8_t& v) override {
        a &= 0xFF;
        if (wr) {
            if (a < 0x80) m_ram[(size_t)m_bank * 128 + a] = *wr;
            else {
                m_reg = *wr & 0x7F;
                m_bank = *wr & 0x3F;
                if (*wr & 0x40) m_forced = false;    // release the freeze
            }
            return false;
        }
        v = a < 0x80 ? m_ram[(size_t)m_bank * 128 + a] : m_rom[0x1E80 + (a & 0x7F)];
        return true;
    }
    void iod6Write(uint16_t a, uint8_t v) override {
        if ((a & 0xFF) < 0x10) m_ram[(size_t)m_bank * 128 + (a & 0x0F)] = v;
    }
    void reset() override { m_reg = 0; m_bank = 0; m_forced = false; }
    bool freeze() override { m_forced = true; return true; }
    bool nmiVectorForced() const override { return m_forced; }
private:
    std::vector<uint8_t> m_rom, m_ram;
    uint8_t m_reg = 0;
    int m_bank = 0;
    bool m_forced = false;
};

// ---------------------------------------------------------------------------
// AM29F040 flash (flash040core.c, FLASH040_TYPE_NORMAL): commands after
// $5555 <- $AA, $2AAA <- $55 (addresses masked with $7FFF); program, chip
// and sector erase (64 KB sectors), autoselect. Erases complete at once.
// ---------------------------------------------------------------------------
class Flash040 {
public:
    std::vector<uint8_t> data = std::vector<uint8_t>(0x80000, 0xFF);
    void reset() { m_state = m_base = READ; m_program = 0; }
    uint8_t read(uint32_t a) {
        uint8_t v;
        switch (m_state) {
            case AUTOSELECT:
                if ((a & 0xFF) == 0) v = 0x01;              // AMD
                else if ((a & 0xFF) == 1) v = 0xA4;         // 29F040
                else if ((a & 0xFF) == 2) v = 0;
                else v = data[a];
                break;
            case PROGRAM_ERROR:
                v = (uint8_t)(((m_program ^ 0x80) & 0x80) | ((m_toggle ^= 0x40) & 0x40) | 0x20);
                break;
            default: v = data[a]; break;
        }
        return v;
    }
    void store(uint32_t a, uint8_t b) {
        const bool m1 = (a & 0x7FFF) == 0x5555, m2 = (a & 0x7FFF) == 0x2AAA;
        switch (m_state) {
            case READ: if (m1 && b == 0xAA) m_state = MAGIC1; break;
            case MAGIC1: m_state = (m2 && b == 0x55) ? MAGIC2 : m_base; break;
            case MAGIC2:
                if (!m1) { m_state = m_base; break; }
                switch (b) {
                    case 0x90: m_state = m_base = AUTOSELECT; break;
                    case 0xF0: m_state = m_base = READ; break;
                    case 0xA0: m_state = PROGRAM; break;
                    case 0x80: m_state = ERASE1; break;
                    default: m_state = m_base; break;
                }
                break;
            case PROGRAM: {
                m_program = b;
                data[a] &= b;
                m_state = data[a] == b ? m_base : PROGRAM_ERROR;
                break;
            }
            case ERASE1: m_state = (m1 && b == 0xAA) ? ERASE2 : m_base; break;
            case ERASE2: m_state = (m2 && b == 0x55) ? ERASE_SELECT : m_base; break;
            case ERASE_SELECT:
                if (m1 && b == 0x10) std::fill(data.begin(), data.end(), 0xFF);
                else if (b == 0x30) std::fill(data.begin() + (a & 0x70000), data.begin() + (a & 0x70000) + 0x10000, 0xFF);
                m_state = m_base;
                break;
            case PROGRAM_ERROR:
            case AUTOSELECT:
                if (m1 && b == 0xAA) m_state = MAGIC1;
                if (b == 0xF0) m_state = m_base = READ;
                break;
        }
    }
private:
    enum State { READ, MAGIC1, MAGIC2, AUTOSELECT, PROGRAM, PROGRAM_ERROR, ERASE1, ERASE2, ERASE_SELECT };
    State m_state = READ, m_base = READ;
    uint8_t m_program = 0, m_toggle = 0;
};

// M93C86 serial EEPROM, 2 KB in 16-bit words (m93c86.c)
class M93C86 {
public:
    std::vector<uint8_t> data = std::vector<uint8_t>(2048, 0xFF);
    uint8_t readData() {
        if (!m_cs) return 0;
        if (m_cmd == ISBUSY) { m_cmd = ISREADY; return 0; }
        if (m_cmd == ISREADY) { m_cmd = 0; return 1; }
        return (uint8_t)m_out;
    }
    void writeData(uint8_t v) { if (m_cs) m_in = v & 1; }
    void writeSelect(uint8_t v) {
        if (!m_cs && v && !m_clock) resetInput();
        else if (m_cs && !v && (m_cmd == WRITE || m_cmd == WRAL || m_cmd == ERAL)) m_cmd = ISBUSY;
        m_cs = v;
        if (!m_cs && (m_cmd == READ || m_cmd == READDUMMY || m_cmd == READDATA)) m_cmd = 0;
    }
    void writeClock(uint8_t v) {
        if (m_cs && v && !m_clock) {
            if (m_cmd == READDUMMY) {
                m_outReg = data[m_addr << 1];
                m_out = (m_outReg >> 7) & 1;
                m_outReg = (m_outReg << 1) & 0xFF;
                m_outCount = 1;
                m_cmd = READDATA;
            } else if (m_cmd == READDATA) {
                m_out = (m_outReg >> 7) & 1;
                m_outReg = (m_outReg << 1) & 0xFF;
                ++m_outCount;
                if (m_outCount == 8) m_outReg = data[(m_addr << 1) + 1];
                else if (m_outCount == 16) {
                    m_addr = (m_addr + 1) & 0x3FF;
                    m_outReg = data[m_addr << 1];
                    m_outCount = 0;
                }
            } else {
                shiftIn();
            }
        }
        m_clock = v;
    }
private:
    enum { CMD00 = 1, WRITE, READ, ERASE, WEN, WDS, ERAL, WRAL, READDUMMY, READDATA, ISBUSY, ISREADY };
    void resetInput() { m_inReg = 0; m_inCount = 0; }
    void shiftIn() {
        m_inReg = (m_inReg << 1) | (unsigned)m_in;
        ++m_inCount;
        switch (m_inCount) {
            case 1: if (!m_in) resetInput(); break;
            case 3:
                switch (m_inReg) {
                    case 4: m_cmd = CMD00; break;
                    case 5: m_cmd = WRITE; break;
                    case 6: m_cmd = READ; break;
                    case 7: m_cmd = ERASE; break;
                }
                break;
            case 5:
                if (m_cmd == CMD00) {
                    switch (m_inReg) {
                        case 0x10: m_cmd = WDS; break;
                        case 0x11: m_cmd = WRAL; break;
                        case 0x12: m_cmd = ERAL; break;
                        case 0x13: m_cmd = WEN; m_wen = true; break;
                    }
                }
                break;
            case 13:
                switch (m_cmd) {
                    case READ: m_cmd = READDUMMY; m_addr = m_inReg & 0x3FF; resetInput(); break;
                    case WDS: m_wen = false; resetInput(); m_cmd = 0; break;
                    case WEN: m_wen = true; resetInput(); m_cmd = 0; break;
                    case ERASE:
                        if (m_wen) { m_addr = m_inReg & 0x3FF; data[m_addr << 1] = 0xFF; data[(m_addr << 1) + 1] = 0xFF; }
                        else m_cmd = 0;
                        resetInput();
                        break;
                    case ERAL:
                        if (m_wen) std::fill(data.begin(), data.end(), 0xFF);
                        else m_cmd = 0;
                        resetInput();
                        break;
                    default: break;
                }
                break;
            case 29:
                if (m_cmd == WRITE || m_cmd == WRAL) {
                    if (m_wen) {
                        const uint8_t d0 = (m_inReg >> 8) & 0xFF, d1 = m_inReg & 0xFF;
                        if (m_cmd == WRITE) {
                            m_addr = (m_inReg >> 16) & 0x3FF;
                            data[m_addr << 1] = d0;
                            data[(m_addr << 1) + 1] = d1;
                        } else {
                            for (size_t i = 0; i < data.size(); i += 2) { data[i] = d0; data[i + 1] = d1; }
                        }
                    } else {
                        m_cmd = 0;
                    }
                    resetInput();
                }
                break;
        }
    }
    uint8_t m_cs = 0, m_clock = 0, m_in = 0, m_out = 0;
    unsigned m_inReg = 0, m_inCount = 0, m_outReg = 0, m_outCount = 0;
    int m_cmd = 0, m_addr = 0;
    bool m_wen = false;
};

// GMod2-C128 (gmod2c128.c): 512 KB flash in 32 banks of 16 KB, the bank at
// both $8000 and $C000; $DE00 write: bits 0-4 the bank, bit 6 EEPROM chip
// select, 5 clock, 4 data in; read: bit 7 the EEPROM's data out while
// selected
class GMod2C128Cart : public C128Cart {
public:
    explicit GMod2C128Cart(const std::vector<uint8_t>& flash) { std::copy(flash.begin(), flash.end(), m_flash.data.begin()); }
    std::string type() const override { return "Gmod2-C128"; }
    uint8_t romRead(uint16_t a, bool peek) override {
        const uint32_t at = (a & 0x3FFFu) + ((uint32_t)m_bank << 14);
        return peek ? m_flash.data[at] : m_flash.read(at);
    }
    void romWrite(uint16_t a, uint8_t v) override { m_flash.store((a & 0x3FFFu) + ((uint32_t)m_bank << 14), v); }
    bool io1(uint16_t, const uint8_t* wr, bool peek, uint8_t& v) override {
        if (wr) {
            m_bank = *wr & 0x1F;
            m_cs = (*wr >> 6) & 1;
            m_eeprom.writeSelect(m_cs);
            if (m_cs) {
                m_eeprom.writeData((*wr >> 4) & 1);
                m_eeprom.writeClock((*wr >> 5) & 1);
            }
            return false;
        }
        if (peek) { v = (uint8_t)(m_eeprom.readData() << 7); return true; }
        if (!m_cs) return false;
        v = (uint8_t)((m_eeprom.readData() << 7) | 0x7F);
        return true;
    }
    void reset() override {
        m_cs = 0;
        m_eeprom.writeSelect(0);
        m_flash.reset();
    }
    // for tests
    M93C86& eeprom() { return m_eeprom; }
private:
    Flash040 m_flash;
    M93C86 m_eeprom;
    int m_bank = 0;
    uint8_t m_cs = 0;
};

}  // namespace

// A generic function ROM into 32 KB (generic.c): a raw image loads the
// largest power of two from 32 KB down to 1 KB it holds (after a 2-byte load
// address if there is one), mirrored; a .crt has up to two CHIP packets at
// $8000 and $C000 of up to 16 KB, each mirrored in its half.
bool functionRomImage(const std::vector<uint8_t>& img, std::vector<uint8_t>& out, std::string& type, std::string& err) {
    out.assign(0x8000, 0xFF);
    auto mirror = [&](size_t base, size_t len, size_t span) {
        for (size_t l = len; l < span; l *= 2) std::copy(out.begin() + (long)base, out.begin() + (long)(base + l), out.begin() + (long)(base + l));
    };
    if (img.size() >= 0x40 && std::memcmp(img.data(), "C128 CARTRIDGE  ", 16) == 0) {
        if (be16(img, 0x16) != 0) { err = "not a generic C128 .crt"; return false; }
        std::vector<Chip> chips;
        if (!crtChips(img, chips, err)) return false;
        if (chips.size() > 2) { err = "more than two CHIP packets"; return false; }
        for (const Chip& c : chips) {
            if ((c.start != 0x8000 && c.start != 0xC000) || !c.size || c.size > 0x4000) { err = "unsupported CHIP packet"; return false; }
            const size_t base = c.start == 0xC000 ? 0x4000 : 0;
            std::copy(img.begin() + (long)c.data, img.begin() + (long)(c.data + c.size), out.begin() + (long)base);
            size_t p2 = 1;
            while (p2 < c.size) p2 *= 2;
            mirror(base, p2 == c.size ? c.size : p2, 0x4000);
        }
        std::string name(img.begin() + 0x20, img.begin() + 0x40);
        name = name.substr(0, name.find('\0'));
        type = "C128 .crt" + (name.empty() ? std::string() : ": " + name);
        return true;
    }
    for (size_t len = 0x8000; len >= 0x400; len /= 2) {
        if (img.size() != len && img.size() != len + 2) continue;
        std::copy(img.end() - (long)len, img.end(), out.begin());
        mirror(0, len, 0x8000);
        type = std::to_string(len / 1024) + " KB function ROM";
        return true;
    }
    err = "not a C128 cartridge (a raw 1-32 KB image, or a .crt with a C128 CARTRIDGE header)";
    return false;
}

std::unique_ptr<C128Cart> makeCart(const std::vector<uint8_t>& img, std::string& err) {
    std::vector<uint8_t> raw;
    const bool crt = img.size() >= 0x40 && std::memcmp(img.data(), "C128 CARTRIDGE  ", 16) == 0;
    if (!crt) {
        if (img.size() <= 0x8000 + 2) {
            std::vector<uint8_t> rom;
            std::string type;
            if (!functionRomImage(img, rom, type, err)) return nullptr;
            return std::make_unique<GenericCart>(std::move(rom), type);
        }
        if (rawOfSize(img, 0x18000, raw)) return std::make_unique<Comal80Cart>(raw);
        for (size_t size = 0x100000; size >= 0x10000; size >>= 1)
            if (rawOfSize(img, size, raw)) return std::make_unique<MagicDesk128Cart>(raw, (int)(size / 0x4000));
        err = "not a C128 cartridge (a raw 1-32 KB function ROM, 96 KB Comal 80, 64 KB-1 MB Magic Desk 128, or a C128 .crt)";
        return nullptr;
    }
    const uint32_t hw = be16(img, 0x16);
    std::string name(img.begin() + 0x20, img.begin() + 0x40);
    name = name.substr(0, name.find('\0'));
    std::vector<Chip> chips;
    switch (hw) {
        case 0: {
            std::vector<uint8_t> rom;
            std::string type;
            if (!functionRomImage(img, rom, type, err)) return nullptr;
            return std::make_unique<GenericCart>(std::move(rom), type);
        }
        case 1: case 2: {                                  // Warp Speed 128, Partner 128: one 16 KB chip at $8000
            if (!crtChips(img, chips, err)) return nullptr;
            if (chips[0].start != 0x8000 || chips[0].size != 0x4000) { err = "expected a 16 KB CHIP at $8000"; return nullptr; }
            raw.assign(img.begin() + (long)chips[0].data, img.begin() + (long)chips[0].data + 0x4000);
            if (hw == 1) return std::make_unique<WarpSpeed128Cart>(raw);
            return std::make_unique<Partner128Cart>(raw);
        }
        case 3: {                                          // Comal 80: six 16 KB chips at $C000
            if (!crtChips(img, chips, err)) return nullptr;
            if (chips.size() < 6) { err = "Comal 80 needs 6 CHIP packets"; return nullptr; }
            raw.assign(0x18000, 0xFF);
            for (int n = 0; n < 6; ++n) {
                if (chips[n].start != 0xC000 || chips[n].size != 0x4000) { err = "expected 16 KB CHIPs at $C000"; return nullptr; }
                std::copy(img.begin() + (long)chips[n].data, img.begin() + (long)chips[n].data + 0x4000, raw.begin() + n * 0x4000);
            }
            return std::make_unique<Comal80Cart>(raw);
        }
        case 4: {                                          // Magic Desk 128: up to 64 16 KB chips at $8000
            if (!crtChips(img, chips, err)) return nullptr;
            const int banks = (int)std::min<size_t>(chips.size(), 64);
            raw.assign((size_t)banks * 0x4000, 0xFF);
            for (int n = 0; n < banks; ++n) {
                if (chips[n].start != 0x8000 || chips[n].size != 0x4000) { err = "expected 16 KB CHIPs at $8000"; return nullptr; }
                std::copy(img.begin() + (long)chips[n].data, img.begin() + (long)chips[n].data + 0x4000, raw.begin() + n * 0x4000);
            }
            return std::make_unique<MagicDesk128Cart>(raw, banks);
        }
        case 5: {                                          // GMod2-C128: 16 KB chips by bank number
            if (!crtChips(img, chips, err)) return nullptr;
            raw.assign(0x80000, 0xFF);
            for (const Chip& c : chips) {
                if (c.bank > 31 || c.size != 0x4000) { err = "expected 16 KB CHIPs, banks 0-31"; return nullptr; }
                std::copy(img.begin() + (long)c.data, img.begin() + (long)c.data + 0x4000, raw.begin() + (long)c.bank * 0x4000);
            }
            return std::make_unique<GMod2C128Cart>(raw);
        }
        default:
            err = "unsupported C128 .crt hardware type " + std::to_string(hw);
            return nullptr;
    }
}

}  // namespace c128
