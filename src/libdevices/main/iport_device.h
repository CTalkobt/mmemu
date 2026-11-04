#pragma once

#include <cstdint>

/**
 * Interface for devices that attach to parallel I/O ports (like VIA or CIA).
 */
class IPortDevice {
public:
    virtual ~IPortDevice() {}

    /**
     * Read the current state of the port from the peripheral's perspective.
     */
    virtual uint8_t readPort() = 0;

    /**
     * The port state readPort() would return, without side effects (for
     * IOHandler::ioPeek). Default: readPort(), for peripherals whose reads
     * change nothing; override it otherwise.
     */
    virtual uint8_t peekPort() { return readPort(); }

    /**
     * Write data from the CPU/Controller to the peripheral via the port.
     */
    virtual void writePort(uint8_t val) = 0;

    /**
     * Inform the peripheral of the Data Direction Register (DDR) state.
     * 1 = Output (CPU -> Port), 0 = Input (Port -> CPU).
     */
    virtual void setDdr(uint8_t ddr) = 0;
};
