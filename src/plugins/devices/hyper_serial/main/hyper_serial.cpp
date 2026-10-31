#include "hyper_serial.h"
#include "include/util/logging.h"
#include <cstdio>
#include <cstdlib>

/** "hyperserial" logger: $D6CF exit trigger. */
static spdlog::logger* hsLog() {
    static auto logger = LogRegistry::instance().getLogger("hyperserial");
    return logger.get();
}

HyperSerialLogger::HyperSerialLogger() : m_baseAddr(0xD6C0), m_logFile(nullptr) {
    setLogFile("hyper_serial.log");
}

HyperSerialLogger::~HyperSerialLogger() {
    if (m_logFile) {
        fclose(m_logFile);
    }
}

void HyperSerialLogger::setLogFile(const std::string& path) {
    if (m_logFile) fclose(m_logFile);
    m_logPath = path;
    m_logFile = fopen(path.c_str(), "w");
}

void HyperSerialLogger::reset() {}

bool HyperSerialLogger::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    uint32_t offset = addr - m_baseAddr;
    
    if (offset == 0) { // $D6C0 status
        *val = 0x03; // Tx and Rx ready
        return true;
    }
    return false;
}

bool HyperSerialLogger::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    uint32_t offset = addr - m_baseAddr;
    
    if (offset == 1) { // $D6C1 data
        if (m_logFile) {
            fputc(val, m_logFile);
            fflush(m_logFile);
        }
        return true;
    }
    
    if (offset == 0x0F) { // $D6CF exit trigger
        hsLog()->debug("exit trigger write at ${:04X} val=${:02X}", addr, val);
        if (val == 0x42) {
            hsLog()->info("halt requested via $D6CF");
            m_haltRequested = true;
        }
        return true;
    }

    return false;
}
