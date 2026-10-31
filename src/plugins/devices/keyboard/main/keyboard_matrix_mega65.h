#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/iport_device.h"
#include "libdevices/main/isignal_line.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstring>

/**
 * MEGA65 Keyboard Matrix (8×8).
 * 
 * Based on C65 matrix. Wired to CIA1:
 * - CIA1 Port A: Column select (Output, Active-Low)
 * - CIA1 Port B: Row sense (Input, Active-Low)
 * 
 * Extends the C64 topology with MEGA65-specific keys and RESTORE/CAPS logic.
 */
class KbdMega65 : public IOHandler, public IKeyboardMatrix {
public:
    KbdMega65();
    ~KbdMega65() override;

    // IOHandler
    const char* name()     const override { return "Mega65Keyboard"; }
    uint32_t    baseAddr() const override { return 0; }
    uint32_t    addrMask() const override { return 0; }
    bool ioRead (IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t)  override { return false; }
    void reset() override;
    void tick(uint64_t cycles) override;

    // IKeyboardMatrix
    void keyDown(int row, int col) override;
    void keyUp(int row, int col) override;
    void clearKeys() override;
    bool pressKeyByName(const std::string& keyName, bool down) override;
    void enqueueText(const std::string& text) override;
    IPortDevice* getPort(int index) override;

    // RESTORE and CAPS logic
    void setRestoreLine(ISignalLine* line) { m_sigRestore = line; }
    void setCapsLockLedLine(ISignalLine* line) { m_sigCapsLockLed = line; }

    /**
     * Route typed-text key events (from enqueueText) through the machine's key
     * handler, so they also reach the $D610 key queue that BASIC 65 reads.
     * Without a sink, events only drive the CIA matrix.
     */
    void setKeySink(std::function<bool(const std::string&, bool)> sink) { m_keySink = std::move(sink); }

private:
    class ColumnPort : public IPortDevice {
    public:
        explicit ColumnPort(KbdMega65* p) : m_parent(p) {}
        uint8_t readPort() override { return m_val; }
        void    writePort(uint8_t v) override;
        void    setDdr(uint8_t ddr) override { m_ddr = ddr; }
        uint8_t m_val = 0xFF;
        uint8_t m_ddr = 0;
    private:
        KbdMega65* m_parent;
    };

    class RowPort : public IPortDevice {
    public:
        explicit RowPort(KbdMega65* p) : m_parent(p) {}
        uint8_t readPort() override;
        void    writePort(uint8_t v) override { m_val = v; }
        void    setDdr(uint8_t ddr) override { m_ddr = ddr; }
        uint8_t m_val = 0xFF;
        uint8_t m_ddr = 0;
    private:
        KbdMega65* m_parent;
    };

    void updateRowVal();
    bool pressCombo(const std::vector<std::string>& keys, bool down);
    void queueKey(const std::string& keyName, bool shifted);

    struct TypeEvent {
        std::string keyName;
        bool        down;
    };
    std::vector<TypeEvent> m_typeQueue;
    uint64_t               m_typeTimer = 0;
    std::function<bool(const std::string&, bool)> m_keySink;

    uint8_t    m_matrix[8]; // Each byte is a column (bits are rows)
    uint8_t    m_rowVal = 0xFF;
    ColumnPort m_colPort;
    RowPort    m_rowPort;

    ISignalLine* m_sigRestore = nullptr;
    ISignalLine* m_sigCapsLockLed = nullptr;

    std::map<std::string, std::pair<int,int>> m_keyMap; // {row, col}
};
