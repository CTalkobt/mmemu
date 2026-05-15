#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/iport_device.h"
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdio>

/**
 * C64 Keyboard Matrix (8×8).
 *
 * Implements both IOHandler (for IORegistry registration) and IKeyboardMatrix
 * (for JsonMachineLoader kbdWiring).  CIA1 Port A drives columns; Port B reads rows.
 */
class KbdC64 : public IOHandler, public IKeyboardMatrix {
public:
    KbdC64();
    ~KbdC64() override = default;

    // IOHandler
    const char* name()     const override { return "C64Keyboard"; }
    uint32_t    baseAddr() const override { return 0; }
    uint32_t    addrMask() const override { return 0; }
    bool ioRead (IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t)  override { return false; }
    void reset() override { KbdC64::clearKeys(); }
    void tick(uint64_t cycles) override;

    // IKeyboardMatrix
    void keyDown(int row, int col) override {
        if (col >= 0 && col < 8 && row >= 0 && row < 8)
            m_matrix[col] &= ~(uint8_t)(1 << row);
        updateMatrix();
    }
    void keyUp(int row, int col) override {
        if (col >= 0 && col < 8 && row >= 0 && row < 8)
            m_matrix[col] |= (uint8_t)(1 << row);
        updateMatrix();
    }
    void clearKeys() override {
        std::memset(m_matrix, 0xFF, sizeof(m_matrix));
        updateMatrix();
    }
    bool pressKeyByName(const std::string& keyName, bool down) override;
    void enqueueText(const std::string& text) override;
    IPortDevice* getPort(int index) override {
        return index == 0 ? (IPortDevice*)&m_colPort : (IPortDevice*)&m_rowPort;
    }

    // Direct accessors (kept for compatibility)
    IPortDevice* getColumnPort() { return &m_colPort; }
    IPortDevice* getRowPort()    { return &m_rowPort; }

private:
    // CIA1 Port A device: CPU writes column-select here.
    class ColumnPort : public IPortDevice {
    public:
        explicit ColumnPort(KbdC64* p) : m_parent(p) {}
        uint8_t readPort() override { return m_val; }
        void    writePort(uint8_t v) override { m_val = v; m_parent->updateMatrix(); }
        void    setDdr(uint8_t) override {}
        uint8_t m_val = 0xFF;
    private:
        KbdC64* m_parent;
    };

    // CIA1 Port B device: CPU reads row-sense here.
    class RowPort : public IPortDevice {
    public:
        explicit RowPort(KbdC64* p) : m_parent(p) {}
        uint8_t readPort() override { return m_parent->m_rowVal; }
        void    writePort(uint8_t) override {}
        void    setDdr(uint8_t) override {}
    private:
        KbdC64* m_parent;
    };

    bool pressCombo(const std::vector<std::string>& keys, bool down);
    void updateMatrix();

    struct TypeEvent {
        std::string keyName;
        bool        down;
    };
    std::vector<TypeEvent> m_typeQueue;
    uint64_t               m_typeTimer = 0;

    uint8_t    m_matrix[8];
    uint8_t    m_rowVal = 0xFF;
    ColumnPort m_colPort;
    RowPort    m_rowPort;

    std::map<std::string, std::pair<int,int>> m_keyMap; // {col, row}
};
