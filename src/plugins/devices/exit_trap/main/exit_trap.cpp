#include "exit_trap.h"
#include "include/mmemu_plugin_api.h"
#include <memory>

ExitTrapDevice::ExitTrapDevice(uint32_t addr) : m_baseAddr(addr), m_haltRequested(false) {}

bool ExitTrapDevice::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    if (addr == m_baseAddr && val == 0x42) {
        m_haltRequested = true;
        return true;
    }
    return false;
}
