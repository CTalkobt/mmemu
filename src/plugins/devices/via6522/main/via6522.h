#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/isignal_line.h"
#include <string>
#include <functional>

/**
 * MOS 6522 Versatile Interface Adapter (VIA).
 * Used in VIC-20, C64 (as 6526 CIA successor/relative), and many other systems.
 */
class VIA6522 : public IOHandler {
public:
    VIA6522() : m_name("6522"), m_baseAddr(0) {
        m_ca1Conduit.m_via = this;
        m_cb1Conduit.m_via = this;
        m_pb7Proxy.m_reg = &m_regs[ORB]; m_pb7Proxy.m_bit = 7;
        for (int i = 0; i < 7; ++i) { m_pbProxy[i].m_reg = &m_regs[ORB]; m_pbProxy[i].m_bit = i; }
        VIA6522::reset();
    }
    VIA6522(const std::string& name, uint32_t baseAddr);
    virtual ~VIA6522() = default;
    ISignalLine* getSignalLine(const char* name) override {
        std::string n(name);
        if (n == "ca1") return &m_ca1Conduit;
        if (n == "ca2") return &m_ca2Conduit;
        if (n == "cb1") return &m_cb1Conduit;
        if (n == "cb2") return &m_cb2Conduit;
        if (n == "pb7") return &m_pb7Proxy;
        if (n.size() == 3 && n[0] == 'p' && n[1] == 'b' && n[2] >= '0' && n[2] <= '6')
            return &m_pbProxy[n[2] - '0'];
        return nullptr;
    }

    // Configuration
    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr) override { m_baseAddr = addr; }

    // IOHandler interface
    const char* name() const override { return m_name.c_str(); }
    uint32_t baseAddr() const override { return m_baseAddr; }
    uint32_t addrMask() const override { return 0x000F; } // 16 registers

    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // Peripheral attachment
    void setPortADevice(IPortDevice* device) override { m_portADevice = device; }
    void setPortBDevice(IPortDevice* device) override { m_portBDevice = device; }
    void setIrqLine(ISignalLine* line) override { m_irqLine = line; }
    void setCA1Line(ISignalLine* line) override { m_ca1Line = line; }
    void setCA2Line(ISignalLine* line) override { m_ca2Line = line; }
    void setCB1Line(ISignalLine* line) override { m_cb1Line = line; }
    void setCB2Line(ISignalLine* line) override { m_cb2Line = line; }

    void setPortAWriteCallback(std::function<void(uint8_t)> cb) { m_portAWriteCb = cb; }
    void setPortBWriteCallback(std::function<void(uint8_t)> cb) { m_portBWriteCb = cb; }

    // Register constants
    enum Reg {
        ORB   = 0x0,
        ORA   = 0x1,
        DDRB  = 0x2,
        DDRA  = 0x3,
        T1CL  = 0x4,
        T1CH  = 0x5,
        T1LL  = 0x6,
        T1LH  = 0x7,
        T2CL  = 0x8,
        T2CH  = 0x9,
        SR    = 0xA,
        ACR   = 0xB,
        PCR   = 0xC,
        IFR   = 0xD,
        IER   = 0xE,
        IORA2 = 0xF  // ORA without handshake
    };

private:
    struct SignalLine : public ISignalLine {
        bool m_level = true;
        bool get() const override { return m_level; }
        void set(bool level) override { m_level = level; }
        void pulse() override { m_level = !m_level; m_level = !m_level; }
    };
    // Read-only proxy for a single bit of a register byte (used for port-pin signal lines).
    struct RegBitSignal : public ISignalLine {
        const uint8_t* m_reg = nullptr;
        uint8_t        m_bit = 0;
        bool get() const override { return m_reg ? ((*m_reg >> m_bit) & 1) != 0 : false; }
        void set(bool) override {}
        void pulse() override {}
    };
    // Immediate-edge conduit for CA1/CB1: sets IFR as soon as the active edge occurs,
    // without waiting for tick() to poll.
    struct CA1Conduit : public ISignalLine {
        VIA6522* m_via  = nullptr;
        bool     m_level = true;
        bool get() const override { return m_level; }
        void set(bool level) override;
        void pulse() override { set(false); set(true); }
    };
    friend struct CA1Conduit;
    struct CB1Conduit : public ISignalLine {
        VIA6522* m_via  = nullptr;
        bool     m_level = true;
        bool get() const override { return m_level; }
        void set(bool level) override;
        void pulse() override { set(false); set(true); }
    };
    friend struct CB1Conduit;
    CA1Conduit   m_ca1Conduit;
    CB1Conduit   m_cb1Conduit;
    SignalLine   m_ca2Conduit, m_cb2Conduit;
    RegBitSignal m_pb7Proxy;
    RegBitSignal m_pbProxy[7]; // pb0–pb6 read-only proxies into ORB
    void updateIrq();
    void driveCA2(bool level);
    void driveCB2(bool level);

    std::string m_name;
    uint32_t    m_baseAddr;

    uint8_t m_regs[16];

    // Latches for Timer 1
    uint16_t m_t1Latch = 0;
    uint16_t m_t1Counter = 0;
    bool     m_t1Active = false;

    // Timer 2
    uint16_t m_t2Counter = 0;
    bool     m_t2Active = false;

    IPortDevice* m_portADevice = nullptr;
    IPortDevice* m_portBDevice = nullptr;
    ISignalLine* m_irqLine  = nullptr;
    ISignalLine* m_ca1Line  = nullptr;  // CA1 input
    ISignalLine* m_ca2Line  = nullptr;  // CA2 input/output (depends on PCR)
    ISignalLine* m_cb1Line  = nullptr;  // CB1 input
    ISignalLine* m_cb2Line  = nullptr;  // CB2 input/output (depends on PCR)

    std::function<void(uint8_t)> m_portAWriteCb;
    std::function<void(uint8_t)> m_portBWriteCb;

    bool m_ca1Prev = true;  // last sampled CA1 level
    bool m_ca2Prev = true;
    bool m_cb1Prev = true;
    bool m_cb2Prev = true;

    bool m_lastIrq = false;
};
