#pragma once

// ROM Paks over 16 KB (bank-switched cartridges) and the Dragon's other disk
// cartridges (#355). Their decoding after XRoar (gmc.c, deltados.c, cart.c,
// rom.c; GPL-3.0-or-later) and MAME (bus/coco/coco_pak.cpp,
// dragon_fdc.cpp; BSD-3-Clause); mmsim's own code.

#include "coco_cart.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Wd17xx;

namespace bbc {
class Sn76489;
}

namespace coco {

/**
 * A bank-switched ROM Pak: the image (up to 256 KB, padded with $FF to a
 * power of two) seen 16 KB at a time at $C000-$FEFF, the bank chosen by a
 * register in the cartridge's I/O ($FF40-$FF5F, write only; reset: bank 0).
 * Its CART line is tied to Q (autostart), as a plain ROM Pak's.
 *
 *  - Gmc: John Linville's Games Master Cartridge, as XRoar's gmc.c. A
 *    write to an even address of $FF40-$FF5F selects bank (D << 14)
 *    masked to the image; an odd address is its SN76489 (4 MHz), mixed
 *    into the machine's sound. XRoar runs every unknown image over 16 KB
 *    as one (Tandy's own banked Paks, RoboCop and Predator, among them).
 *  - Tandy: MAME's "CoCo Program PAK (Banked)" (coco_pak.cpp): only $FF40
 *    selects, bank D (x 16 KB, modulo the image); no sound.
 */
class BankedPak : public Cart {
public:
    enum class Scheme { Gmc, Tandy };
    BankedPak(std::vector<uint8_t> image, std::string label, Scheme scheme);
    ~BankedPak() override;
    std::string kind() const override { return m_scheme == Scheme::Gmc ? "gmc" : "banked"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override { return peek(a, cts, scs, d); }
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    void setClockHz(double hz) override { m_hz = hz; }
    float audio() const override { return m_level; }

    Scheme scheme() const { return m_scheme; }
    /** The image as padded (a power of two). */
    size_t size() const { return m_rom.size(); }
    /** The bank's offset in the image. */
    uint32_t bankOffset() const { return m_bank; }
    bbc::Sn76489* psg() const { return m_psg.get(); }

private:
    std::vector<uint8_t> m_rom;
    std::string m_label;
    Scheme m_scheme;
    uint32_t m_bank = 0;
    std::unique_ptr<bbc::Sn76489> m_psg;       // the GMC's
    double m_hz = 14318180.0;
    uint64_t m_psgAcc = 0;
    float m_level = 0.0f;
    bool m_cartLevel = true;
    uint64_t m_cartAcc = 0;
};

/**
 * Premier Microsystems' Delta disk system (DeltaDOS), as XRoar's deltados.c
 * and MAME's premier_fdc: its ROM ($C000), a WD2791 (inverted data bus) at
 * $FF40-$FF43 and a latch (74LS174, write only) where A2 is set
 * ($FF44-$FF47; XRoar mirrors both through $FF5F, MAME decodes $FF44
 * alone). Latch: bits 0-1 drive select, 2 side select, 3 the 8" clock
 * (MAME's note; a 2 MHz controller clock here), 4 density (1 = double).
 * No motor bit (the motors run), READY tied high (XRoar: assumed), and
 * neither DRQ nor INTRQ is wired to the CPU: DeltaDOS polls the status.
 */
class DeltaDos : public Cart {
public:
    explicit DeltaDos(std::vector<uint8_t> rom);
    ~DeltaDos() override;
    std::string kind() const override { return "deltados"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    void setClockHz(double hz) override;
    bool mountDisk(int drive, const std::string& path) override;
    bool ejectDisk(int drive) override;
    std::string diskPath(int drive) const override;
    bool diskStatus(int drive, int& track, bool& motor) const override;

    uint8_t latch() const { return m_latch; }
    Wd17xx& controller() { return *m_fdc; }

private:
    void latchWrite(uint8_t v);
    std::unique_ptr<Wd17xx> m_fdc;
    std::string m_paths[4];
    std::vector<uint8_t> m_rom;
    uint8_t m_latch = 0;
};

/** CRC-32 (IEEE) of `n` bytes. */
uint32_t crc32(const uint8_t* p, size_t n);

/**
 * What a ROM image needs (XRoar's cart.c heuristic): "rompak" (a plain
 * ROM Pak, up to 32 KB) or "gmc" (banked). Images XRoar's database knows
 * by CRC (padded with $FF to a 4 KB multiple) keep their type: the CoCo 3's
 * unbanked 32 KB carts are "rompak", Mind Roll, RoboCop, Predator,
 * Blockdown and Dunjunz "gmc"; any other image over 16 KB is "gmc".
 */
std::string romCartType(const std::vector<uint8_t>& image);

/** A ROM cartridge of `type` ("", auto: romCartType; "rompak", "gmc", "banked"). */
std::unique_ptr<Cart> makeRomCart(std::vector<uint8_t> image, const std::string& label, const std::string& type,
                                  std::string& err);

/**
 * The cartridges of this file by name, for makeCart: gmc, banked (option:
 * the image), deltados, cumana, superdos, dosplus (option: the ROM, else
 * found in dragonFirmwareDir(); the last three on DragonDOS's hardware).
 * nullptr with `err` empty if `device` isn't one.
 */
std::unique_ptr<Cart> makePakCart(const std::string& device, const std::string& option, std::string& err);

}  // namespace coco
