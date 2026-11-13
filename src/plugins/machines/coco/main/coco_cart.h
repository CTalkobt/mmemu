#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Wd17xx;

class Acia6551;

namespace coco {

/**
 * A device in the CoCo's cartridge port, or in a slot of a Multi-Pak.
 *
 * The port sees every CPU cycle: `cts` is the SAM's /CTS ($C000-$FEFF,
 * the cartridge ROM), `scs` its /SCS ($FF40-$FF5F, the cartridge's I/O);
 * with neither, a cartridge may still decode the address itself (the
 * Orchestra-90 at $FF7A, the Deluxe RS-232 Pak at $FF68, the Multi-Pak's
 * register at $FF7F). A read returns true when the cartridge drives the
 * data bus.
 *
 * Lines back to the machine: CART (PIA1 CB1, the FIRQ), /NMI and /HALT.
 * Time passes through clock(), in 14.31818 MHz ticks (16 a slow cycle).
 */
class Cart {
public:
    virtual ~Cart() = default;
    /** "rompak", "mpi", "orch90", "rs232", "fd500". */
    virtual std::string kind() const = 0;
    /** What it is, for the UI: "ROM Pak (game.ccc)". */
    virtual std::string describe() const { return kind(); }
    virtual void reset() {}
    virtual bool read(uint16_t a, bool cts, bool scs, uint8_t& d) { (void)a; (void)cts; (void)scs; (void)d; return false; }
    /** A read without side effects (debugger). */
    virtual bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const { (void)a; (void)cts; (void)scs; (void)d; return false; }
    virtual void write(uint16_t a, bool cts, bool scs, uint8_t d) { (void)a; (void)cts; (void)scs; (void)d; }
    virtual void clock(uint64_t ticks) { (void)ticks; }
    /** Sound it puts on the machine's audio, -1..1 (the Orchestra-90's DACs). */
    virtual float audio() const { return 0.0f; }
    /** Connect a serial port's far end (CLI `serial`): false if it has none. */
    virtual bool attachSerial(const std::string& spec) { (void)spec; return false; }
    /** A disk image in drive 0-3 (a disk controller's): false if it has none. */
    virtual bool mountDisk(int drive, const std::string& path) { (void)drive; (void)path; return false; }
    virtual bool ejectDisk(int drive) { (void)drive; return false; }
    /** The image in drive 0-3, "" for none; its head's track and the motor (the LED). */
    virtual std::string diskPath(int drive) const { (void)drive; return ""; }
    virtual bool diskStatus(int drive, int& track, bool& motor) const { (void)drive; (void)track; (void)motor; return false; }

    std::function<void(bool)> setCart;     ///< CART (true = high)
    std::function<void(bool)> setNmi;      ///< /NMI asserted
    std::function<void(bool)> setHalt;     ///< /HALT asserted
};

/**
 * A ROM Pak (.rom / .ccc): up to 16 KB at $C000-$FEFF (smaller images
 * repeat). Its CART line is tied to the Q clock, so once BASIC enables
 * the cartridge FIRQ (PIA1 CRB) the first edge starts it at $C000: here a
 * 1 kHz square wave, which reaches the PIA as soon as BASIC looks.
 */
class RomPak : public Cart {
public:
    RomPak(std::vector<uint8_t> image, std::string label, bool autostart = true);
    std::string kind() const override { return "rompak"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void clock(uint64_t ticks) override;
    const std::string& label() const { return m_label; }
    size_t size() const { return m_rom.size(); }

private:
    std::vector<uint8_t> m_rom;
    std::string m_label;
    bool m_autostart;
    bool m_level = true;
    uint64_t m_acc = 0;
};

/**
 * The Orchestra-90 (Orchestra-90/CC): two 8-bit DACs, left at $FF7A and
 * right at $FF7B (write only), mixed to mono; its ROM (the Orchestra-90
 * program, optional) as a ROM Pak.
 */
class Orchestra90 : public Cart {
public:
    explicit Orchestra90(std::vector<uint8_t> rom = {});
    std::string kind() const override { return "orch90"; }
    std::string describe() const override { return "Orchestra-90 stereo DACs"; }
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    float audio() const override;
    uint8_t left() const { return m_left; }
    uint8_t right() const { return m_right; }

private:
    std::vector<uint8_t> m_rom;
    uint8_t m_left = 0x80, m_right = 0x80;
};

/**
 * The Deluxe RS-232 Program Pak: a 6551 ACIA at $FF68-$FF6B (its own
 * 1.8432 MHz crystal sets the rates), its /IRQ on the CART line.
 */
class DeluxeRs232 : public Cart {
public:
    DeluxeRs232();
    ~DeluxeRs232() override;
    std::string kind() const override { return "rs232"; }
    std::string describe() const override { return "Deluxe RS-232 Program Pak (6551 at $FF68)"; }
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    bool attachSerial(const std::string& spec) override;
    Acia6551& acia() { return *m_acia; }

private:
    void irqUpdate();
    std::unique_ptr<Acia6551> m_acia;
    uint64_t m_acc = 0;
    bool m_irq = false;
};

/**
 * The Radio Shack FD-500/501 disk controller cartridge: Disk BASIC in its
 * ROM ($C000), a latch at $FF40 and a WD1793 (FD1773 on the 501) at
 * $FF48-$FF4B.
 *
 * Latch ($FF40-$FF47, write): bits 0-2 drive select 0-2, 3 motor on, 4
 * write precompensation, 5 density (1 = single; 0 also lets the
 * controller's INTRQ through to /NMI), 6 drive select 3 (side select on
 * double-sided drives), 7 HALT enable: while set, the CPU halts until the
 * controller raises DRQ (a byte is ready) or INTRQ (which also clears it).
 *
 * The controller is a WD2793 (as XRoar's; the FD-501 has an FD1773) with
 * READY tied high, clocked from the machine; its images are JVC, VDK or
 * DMK (`mountDisk`, CLI `disk mount 1 game.dsk` for DRIVE 0, or `-i`).
 */
class Fd500 : public Cart {
public:
    explicit Fd500(std::vector<uint8_t> diskBasic);
    ~Fd500() override;
    std::string kind() const override { return "fd500"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    bool mountDisk(int drive, const std::string& path) override;
    bool ejectDisk(int drive) override;
    std::string diskPath(int drive) const override;
    bool diskStatus(int drive, int& track, bool& motor) const override;

    /** From the controller (also for tests driving the latch logic alone). */
    void setDrq(bool on);
    void setIntrq(bool on);
    uint8_t latch() const { return m_latch; }
    Wd17xx& controller() { return *m_fdc; }

private:
    void latchWrite(uint8_t v);
    void updateHalt();
    std::unique_ptr<Wd17xx> m_fdc;
    std::string m_paths[4];
    std::vector<uint8_t> m_rom;
    uint8_t m_latch = 0;
    bool m_drq = false, m_intrq = false, m_haltEnable = false;
};

/**
 * The Multi-Pak Interface (26-3024): four slots. Its register at $FF7F
 * routes /CTS (the ROM, bits 4-5) and /SCS (the I/O, bits 0-1) to a slot
 * each; the front switch sets both at reset (slot 4 by default). Other
 * cycles reach every slot. CART comes from the slot /CTS selects; /NMI and
 * /HALT from any slot.
 */
class MultiPak : public Cart {
public:
    MultiPak();
    std::string kind() const override { return "mpi"; }
    std::string describe() const override;
    void reset() override;
    bool read(uint16_t a, bool cts, bool scs, uint8_t& d) override;
    bool peek(uint16_t a, bool cts, bool scs, uint8_t& d) const override;
    void write(uint16_t a, bool cts, bool scs, uint8_t d) override;
    void clock(uint64_t ticks) override;
    float audio() const override;
    bool attachSerial(const std::string& spec) override;
    bool mountDisk(int drive, const std::string& path) override;
    bool ejectDisk(int drive) override;
    std::string diskPath(int drive) const override;
    bool diskStatus(int drive, int& track, bool& motor) const override;

    /** Slot 0-3 (the labels say 1-4). */
    void insert(int slot, std::unique_ptr<Cart> cart);
    Cart* slot(int n) const { return (n >= 0 && n < 4) ? m_slots[n].get() : nullptr; }
    /** The front switch (0-3): both selects at reset. */
    void setSwitch(int n) { m_switch = n & 3; }
    int switchPosition() const { return m_switch; }
    uint8_t selectRegister() const { return m_reg; }
    int ctsSlot() const { return (m_reg >> 4) & 3; }
    int scsSlot() const { return m_reg & 3; }

private:
    void select(uint8_t v);
    std::unique_ptr<Cart> m_slots[4];
    bool m_cart[4] = {true, true, true, true}, m_nmi[4] = {}, m_halt[4] = {};
    int m_switch = 3;
    uint8_t m_reg = 0x33;
};

}  // namespace coco
