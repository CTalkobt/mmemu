#pragma once

#include "libdevices/main/iport_device.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include <vector>
#include <map>

/**
 * Standard Commodore 8x8 Keyboard Matrix.
 * Wired to two I/O ports.
 */
class KbdVic20 : public IKeyboardMatrix {
public:
    KbdVic20();
    virtual ~KbdVic20() = default;

    // IKeyboardDevice interface
    void keyDown(int row, int col) override;
    void keyUp(int row, int col) override;
    void clearKeys() override;
    bool pressKeyByName(const std::string& keyName, bool down) override;
    void enqueueText(const std::string& text) override;
    void tick(uint64_t cycles);
    IPortDevice* getPort(int index) override {
        if (index == 0) return (IPortDevice*)&m_colPort;
        if (index == 1) return (IPortDevice*)&m_rowPort;
        return nullptr;
    }

    /**
     * Port implementation for the "Output" port (Columns).
     * Wired to VIA2 Port B ($9120) — CPU writes column select here.
     */
    class ColumnPort : public IPortDevice {
    public:
        ColumnPort(KbdVic20* parent) : m_parent(parent) {}
        // Only bits set as outputs drive the columns; an input bit floats
        // high, so a joystick sharing the line (right, on PB7) can be read.
        uint8_t readPort() override { return driven(); }
        void writePort(uint8_t val) override { m_val = val; m_parent->updateMatrix(); }
        void setDdr(uint8_t ddr) override { m_ddr = ddr; m_parent->updateMatrix(); }
        uint8_t driven() const { return m_val | (uint8_t)~m_ddr; }
        uint8_t m_val = 0xFF;
        uint8_t m_ddr = 0xFF;
    private:
        KbdVic20* m_parent;
    };

    /**
     * Port implementation for the "Input" port (Rows).
     * Wired to VIA2 Port A ($9121) — CPU reads row sense here.
     */
    class RowPort : public IPortDevice {
    public:
        RowPort(KbdVic20* parent) : m_parent(parent) {}
        uint8_t readPort() override { return m_parent->m_rowVal; }
        void writePort(uint8_t val) override { (void)val; }
        void setDdr(uint8_t ddr) override { (void)ddr; }
    private:
        KbdVic20* m_parent;
    };

    ColumnPort* getColumnPort() { return &m_colPort; }
    RowPort* getRowPort() { return &m_rowPort; }

private:
    void updateMatrix();
    bool pressCombo(const std::vector<std::string>& keys, bool down);

    struct TypeEvent {
        std::string keyName;
        bool        down;
    };
    std::vector<TypeEvent> m_typeQueue;
    uint64_t               m_typeTimer = 0;

    uint8_t m_matrix[8]; // 8 rows, each byte is 8 columns
    uint8_t m_rowVal = 0xFF;
    
    ColumnPort m_colPort;
    RowPort    m_rowPort;

    std::map<std::string, std::pair<int, int>> m_keyMap;
};
