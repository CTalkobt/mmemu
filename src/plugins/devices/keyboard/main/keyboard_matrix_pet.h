#pragma once

#include "libdevices/main/iport_device.h"
#include <string>
#include <map>
#include <cstdint>
#include <functional>

/**
 * PET Keyboard Matrix (8x8).
 */
class PetKeyboardMatrix : public IPortDevice {
public:
    enum class Layout {
        GRAPHICS,
        BUSINESS
    };

    PetKeyboardMatrix(Layout layout = Layout::GRAPHICS);

    void keyDown(const std::string& keyName);
    /** True if the layout has a key of this name. */
    bool hasKey(const std::string& keyName) const { return m_keyMap.count(keyName) != 0; }
    void keyUp(const std::string& keyName);

    void setLogger(void* handle, void (*logFn)(void*, int, const char*)) {
        m_logger = handle;
        m_logNamed = logFn;
    }

    // IPortDevice implementation
    uint8_t readPort() override;
    void writePort(uint8_t val) override;
    void setDdr(uint8_t ddr) override;

private:
    Layout m_layout;
    uint8_t m_matrix[10]; // 10 rows, each with 8 column bits
    uint8_t m_selectedRow = 0x0F;
    
    struct KeyPos { uint8_t col; uint8_t bit; };
    std::map<std::string, KeyPos> m_keyMap;

    void initKeyMap();

    void* m_logger = nullptr;
    void (*m_logNamed)(void*, int, const char*) = nullptr;
};
