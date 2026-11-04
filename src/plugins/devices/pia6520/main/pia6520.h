#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/isignal_line.h"
#include "libdevices/main/iport_device.h"
#include <cstdint>
#include <string>
#include <functional>

/**
 * MOS 6520 Peripheral Interface Adapter (PIA).
 */
class PIA6520 : public IOHandler {
public:
    PIA6520();
    PIA6520(const std::string& name, uint32_t baseAddr);
    virtual ~PIA6520() = default;
    ISignalLine* getSignalLine(const char* name) override {
        std::string n(name);
        if (n == "ca1") return &m_ca1Conduit;
        if (n == "cb1") return &m_cb1Conduit;
        return nullptr;
    }

    // Configuration
    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr)       override { m_baseAddr = addr; }

    // IOHandler interface
    const char* name() const override { return m_name.c_str(); }
    uint32_t baseAddr() const override { return m_baseAddr; }
    uint32_t addrMask() const override { return 0x0003; } // 4 registers

    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t cycles) override;

    // Peripheral attachment
    void setPortADevice(IPortDevice* device) override { m_portADevice = device; }
    void setPortBDevice(IPortDevice* device) override { m_portBDevice = device; }

    // Line signals
    void setIrqALine(ISignalLine* line) override { m_irqALine = line; }
    void setIrqBLine(ISignalLine* line) override { m_irqBLine = line; }
    void setCA1Line (ISignalLine* line) override { m_ca1Line  = line; }
    void setCA2Line (ISignalLine* line) override { m_ca2Line  = line; }
    void setCB1Line (ISignalLine* line) override { m_cb1Line  = line; }
    void setCB2Line (ISignalLine* line) override { m_cb2Line  = line; }

    // Manual signal triggers (for tests/HLE)
    void setCA1(bool level);
    void setCA2(bool level);
    void setCB1(bool level);
    void setCB2(bool level);

    void setPortAWriteCallback(std::function<void(uint8_t)> cb) { m_portAWriteCb = cb; }
    void setPortBWriteCallback(std::function<void(uint8_t)> cb) { m_portBWriteCb = cb; }

    // Snapshots
    struct Snapshot {
        uint8_t ora, ddra, cra;
        uint8_t orb, ddrb, crb;
        bool ca1Prev, ca2Prev, cb1Prev, cb2Prev;
    };
    Snapshot getSnapshot() const;
    void restoreSnapshot(const Snapshot& s);

    void setLogger(void* handle, void (*logFn)(void*, int, const char*)) {
        m_logger = handle;
        m_logNamed = logFn;
    }

private:
    bool readReg(uint32_t addr, uint8_t* val, bool peek);
    struct SignalLine : public ISignalLine {
        bool m_level = true;
        bool get() const override { return m_level; }
        void set(bool level) override { m_level = level; }
        void pulse() override { m_level = !m_level; m_level = !m_level; }
    };
    // Immediate-edge conduits: invoke setCA1/setCB1 as soon as the signal changes.
    struct CA1Conduit : public ISignalLine {
        PIA6520* m_owner = nullptr;
        bool     m_level = true;
        bool get() const override { return m_level; }
        void set(bool level) override;
        void pulse() override { set(false); set(true); }
    };
    friend struct CA1Conduit;
    struct CB1Conduit : public ISignalLine {
        PIA6520* m_owner = nullptr;
        bool     m_level = true;
        bool get() const override { return m_level; }
        void set(bool level) override;
        void pulse() override { set(false); set(true); }
    };
    friend struct CB1Conduit;
    CA1Conduit m_ca1Conduit;
    CB1Conduit m_cb1Conduit;
    void updateIrq();
    void driveCA2(bool level);
    void driveCB2(bool level);

    std::string m_name;
    uint32_t m_baseAddr;
    void* m_logger = nullptr;
    void (*m_logNamed)(void*, int, const char*) = nullptr;

    uint8_t m_ora = 0;
    uint8_t m_ddra = 0;
    uint8_t m_cra = 0;
    uint8_t m_orb = 0;
    uint8_t m_ddrb = 0;
    uint8_t m_crb = 0;

    IPortDevice* m_portADevice = nullptr;
    IPortDevice* m_portBDevice = nullptr;

    class ISignalLine* m_irqALine = nullptr;
    class ISignalLine* m_irqBLine = nullptr;
    class ISignalLine* m_ca1Line = nullptr;
    class ISignalLine* m_ca2Line = nullptr;
    class ISignalLine* m_cb1Line = nullptr;
    class ISignalLine* m_cb2Line = nullptr;

    std::function<void(uint8_t)> m_portAWriteCb;
    std::function<void(uint8_t)> m_portBWriteCb;

    bool m_ca1Prev = true;
    bool m_ca2Prev = true;
    bool m_cb1Prev = true;
    bool m_cb2Prev = true;

    bool m_lastIrqA = false;
    bool m_lastIrqB = false;
};
