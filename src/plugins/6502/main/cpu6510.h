#pragma once

#include "cpu6502.h"
#include "libdevices/main/isignal_line.h"
#include "libmem/main/ibus.h"
#include <string>

/**
 * MOS 6510 CPU — MOS 6502 with a built-in 6-bit I/O port at $00/$01.
 *
 * $00 = DDR  (Data Direction Register): 1 = output, 0 = input (floats high via pull-ups)
 * $01 = DATA: write drives output bits; read returns (DATA & DDR) | (~DDR & 0xFF)
 *
 * Port bit assignments:
 *   bit 0 = LORAM  (0 = RAM visible at $A000, 1 = BASIC ROM)
 *   bit 1 = HIRAM  (0 = RAM visible at $E000, 1 = KERNAL ROM)
 *   bit 2 = CHAREN (0 = Character ROM at $D000, 1 = I/O)
 *   bit 3 = Cassette write output   (not used for banking)
 *   bit 4 = Cassette sense input  (not used for banking)
 *   bit 5 = Cassette motor control    (not used for banking)
 *
 * The three banking lines are exposed as ISignalLine* so the PLA handler
 * (Phase 11.2) can observe them and reconfigure the memory map accordingly.
 *
 * Implementation note: because MOS6502::read/write are private, this class
 * installs a thin proxy bus (PortBus) that intercepts $00/$01 before passing
 * all other accesses through to the real bus.
 */
class MOS6510 : public MOS6502 {
public:
    MOS6510();
    ~MOS6510() override;

    const char* variantName() const override { return m_pinMask & 0x40 ? "MOS 8502" : "MOS 6510"; }

    /**
     * The 8502 (C128): a seventh port pin, bit 6, whose input level comes
     * from `bit6` (the C128's CAPS LOCK / ASCII-DIN key; low = pressed).
     * Bit 7 still reads 1.
     */
    void set8502(ISignalLine* bit6) { m_pinMask = 0x7F; m_bit6 = bit6; }

    // Override bus attachment to install the port proxy.
    void setDataBus(IBus* bus) override;
    void setCodeBus(IBus* bus) override;
    IBus* getDataBus() const override { return m_portBus; }
    IBus* getCodeBus() const override { return m_portBus; }

    // Banking signal outputs.
    ISignalLine* signalLoram()  { return &m_loram; }
    ISignalLine* signalHiram()  { return &m_hiram; }
    ISignalLine* signalCharen() { return &m_charen; }
    ISignalLine* signalCassetteWrite() { return &m_cassetteWrite; }
    ISignalLine* signalCassetteMotor() { return &m_cassetteMotor; }
    ISignalLine* signalCassetteSense() { return &m_cassetteSense; }

    /** ICore override: returns "loram", "hiram", or "charen" signal lines. */
    ISignalLine* getSignalLine(const char* name) override {
        std::string n(name);
        if (n == "loram")  return signalLoram();
        if (n == "hiram")  return signalHiram();
        if (n == "charen") return signalCharen();
        if (n == "cassette_write") return signalCassetteWrite();
        if (n == "cassette_motor") return signalCassetteMotor();
        if (n == "cassette_sense") return signalCassetteSense();
        return nullptr;
    }

    // Direct read-back of port registers (for debugger use).
    uint8_t portDDR()  const { return m_ddr; }
    uint8_t portData() const { return m_data; }

    /** Returns the internal proxy bus that intercepts $00/$01. */
    IBus* getPortBus() const { return m_portBus; }

    void setPhi1Source(std::function<int()> src) override { m_phi1 = std::move(src); }

    // Execution
    int  step()  override;
    void reset() override;

private:
    uint64_t     m_stepCounter = 0;

    // -----------------------------------------------------------------------
    // Simple edge-triggered signal line implementation.
    // -----------------------------------------------------------------------
    struct SignalLine : public ISignalLine {
        bool m_level = true; // pull-ups default high
        bool get()  const override { return m_level; }
        void set(bool level) override { m_level = level; }
        void pulse() override { m_level = !m_level; m_level = !m_level; }
    };

    // -----------------------------------------------------------------------
    // Proxy bus: sits between the CPU core and the real system bus.
    // Intercepts $00 (DDR) and $01 (DATA); passes everything else through.
    // -----------------------------------------------------------------------
    class PortBus : public IBus {
    public:
        explicit PortBus(MOS6510* owner, IBus* real)
            : m_owner(owner), m_real(real) {}

        const BusConfig& config() const override { return m_real->config(); }
        const char*      name()   const override { return m_real->name(); }

        uint8_t read8(uint32_t addr) override {
            if (addr == 0x0000) return m_owner->m_ddr;
            if (addr == 0x0001) return m_owner->portRead();
            return m_real->read8(addr);
        }

        void write8(uint32_t addr, uint8_t val) override {
            if (addr <= 0x0001) {
                if (addr == 0x0000) m_owner->ddrWrite(val);
                else                m_owner->dataWrite(val);
                // The RAM under the port gets what the VIC-II left on the bus
                // in Phi1, not the CPU's data (VICE zero_store)
                if (m_owner->m_phi1) {
                    const int v = m_owner->m_phi1();
                    if (v >= 0) m_real->write8(addr, (uint8_t)v);
                }
                return;
            }
            m_real->write8(addr, val);
        }

        uint8_t peek8(uint32_t addr) override {
            if (addr == 0x0000) return m_owner->m_ddr;
            if (addr == 0x0001) return m_owner->portRead();
            return m_real->peek8(addr);
        }

        // Delegate snapshot / write-log helpers to the real bus.
        void     reset()                                    override { m_real->reset(); }
        size_t   stateSize()                          const override { return m_real->stateSize(); }
        void     saveState(uint8_t* buf)              const override { m_real->saveState(buf); }
        void     loadState(const uint8_t* buf)              override { m_real->loadState(buf); }
        int      writeCount()                         const override { return m_real->writeCount(); }
        void     getWrites(uint32_t* a, uint8_t* b,
                           uint8_t* c, int max)       const override { m_real->getWrites(a,b,c,max); }
        void     clearWriteLog()                            override { m_real->clearWriteLog(); }
        // Cartridge ROM overlays and halt requests belong to the real bus too
        // (the CLI, GUI and MCP attach cartridges through the CPU's data bus)
        void     addRomOverlay(uint32_t base, uint32_t size,
                               const uint8_t* data)         override { m_real->addRomOverlay(base, size, data); }
        void     removeRomOverlay(uint32_t base)            override { m_real->removeRomOverlay(base); }
        bool     isHaltRequested()                          override { return m_real->isHaltRequested(); }
        bool     isPlainMemory(uint32_t addr)         const override { return addr > 1 && m_real->isPlainMemory(addr); }

    private:
        MOS6510* m_owner;
        IBus*    m_real;
    };

    // -----------------------------------------------------------------------
    // Port state
    // -----------------------------------------------------------------------
    uint8_t     m_ddr     = 0x00; // all inputs on power-up
    uint8_t     m_data    = 0x3F; // C64 pull-ups: bits 0-5 high
    uint8_t     m_pinMask = 0x3F; // port pins (0x7F on the 8502)
    ISignalLine* m_bit6   = nullptr;
    // As VICE's pport: the last value driven on each pin (an input keeps it),
    // and the charge on the unconnected pins (6 and 7; 7 on the 8502), which
    // read back what was last driven on them until it falls off
    uint8_t     m_dataOut = 0x3F;
    uint8_t     m_cap     = 0x00;            // the charged bits
    uint64_t    m_capFall[8] = {};           // the cycle each bit's charge is gone
    static constexpr uint64_t FALL_OFF_CYCLES = 350000;   // VICE's C64 6510
    uint8_t unconnected() const { return (uint8_t)(0xC0 & ~m_pinMask); }
    void charge(uint8_t bits, uint8_t values);

    SignalLine  m_loram;
    SignalLine  m_hiram;
    SignalLine  m_charen;
    SignalLine  m_cassetteWrite;
    SignalLine  m_cassetteMotor;
    SignalLine  m_cassetteSense;

    PortBus*    m_portBus = nullptr;
    std::function<int()> m_phi1;        // the VIC-II's Phi1 bus value
    IBus*       m_realBus = nullptr;

public:
    // Internal helpers (called from PortBus)
    uint8_t portRead();

    void ddrWrite(uint8_t val);
    void dataWrite(uint8_t val);

private:
    void updateSignals();
    void installBus(IBus* bus);
};
