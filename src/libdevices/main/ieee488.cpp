#include "ieee488.h"
#include <algorithm>

SimpleIEEE488Bus::SimpleIEEE488Bus() {
    SimpleIEEE488Bus::reset();
}

void SimpleIEEE488Bus::reset() {
    for (int i = 0; i < 8; ++i) {
        m_signals[i] = true; // High (inactive)
    }
    m_data = 0xFF; // Logic 0
}

void SimpleIEEE488Bus::setData(uint8_t val) {
    if (m_data != val) {
        m_data = val;
        for (auto* d : m_devices) {
            d->onDataWrite(this, val);
        }
    }
}

uint8_t SimpleIEEE488Bus::getData() const {
    return m_data;
}

void SimpleIEEE488Bus::setSignal(Signal sig, bool level) {
    if (m_signals[sig] != level) {
        m_signals[sig] = level;
        notifyChange(sig);
    }
}

bool SimpleIEEE488Bus::getSignal(Signal sig) const {
    return m_signals[sig];
}

void SimpleIEEE488Bus::registerDevice(Device* device) {
    m_devices.push_back(device);
}

void SimpleIEEE488Bus::notifyChange(Signal sig) {
    for (auto* d : m_devices) {
        d->onSignalChange(this, sig);
    }
}

bool SimpleIEEE488Bus::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;
    *val = m_data;
    return true;
}

bool SimpleIEEE488Bus::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;
    setData(val);
    return true;
}
