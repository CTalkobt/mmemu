#pragma once

#include <cstdint>
#include <cstddef>
#include "libmem/main/ibus.h"

class ISignalLine;
class IMapController;

/**
 * Register width.
 */
enum class RegWidth {
    R8 = 1,
    R16 = 2,
    R32 = 4
};

/**
 * Register flags.
 */
#define REGFLAG_PC          0x01  // Program counter
#define REGFLAG_SP          0x02  // Stack pointer
#define REGFLAG_STATUS      0x04  // Flags / status register
#define REGFLAG_ACCUMULATOR 0x08  // Primary accumulator
#define REGFLAG_INTERNAL    0x10  // Hidden from UI
#define REGFLAG_SHADOW      0x20  // Alternate/shadow register

/**
 * ISA capabilities bitmask.
 */
#define ISA_CAP_HARVARD       0x0001
#define ISA_CAP_IO_PORT_SPACE 0x0002
#define ISA_CAP_32BIT_ADDR    0x0004
#define ISA_CAP_MULTI_PREFIX  0x0008
#define ISA_CAP_16BIT_DATA    0x0010
#define ISA_CAP_THUMB_MODE    0x0020

/**
 * Register descriptor.
 */
struct RegDescriptor {
    const char* name;
    RegWidth    width;
    uint32_t    flags;
    const char* alias;      // e.g., "F" for 6502 "P"
    const char* flagNames;  // For REGFLAG_STATUS: 8 chars MSB→LSB (e.g. "NV-BDIZC"), or nullptr
};

class ExecutionObserver;

/**
 * Interface for CPU register access.
 */
class ICpuRegs {
public:
    virtual ~ICpuRegs() {}

    // Register descriptor table
    virtual int                  regCount()             const = 0;
    virtual const RegDescriptor* regDescriptor(int idx) const = 0;
    virtual uint32_t             regRead (int idx)      const = 0;
    virtual void                 regWrite(int idx, uint32_t val) = 0;

    // Named register helpers
    virtual uint32_t regReadByName (const char* name) const;
    virtual void     regWriteByName(const char* name, uint32_t val);
    virtual int      regIndexByName(const char* name) const;
};

/**
 * Interface for CPU disassembly.
 */
class ICpuDisasm {
public:
    virtual ~ICpuDisasm() {}

    virtual int disassembleOne  (IBus* bus, uint32_t addr, char* buf, int bufsz) = 0;
    virtual int disassembleEntry(IBus* bus, uint32_t addr, void* entryOut)      = 0;

    /**
     * Like disassembleEntry() but without the text: fills addr, bytes, the
     * call/return/branch/illegal flags, targetAddr and stackOp; mnemonic,
     * operands and complete may be left empty. Called on every step while a
     * debugger is attached, so cores should override it with a cheap decode.
     */
    virtual int decodeEntry(IBus* bus, uint32_t addr, void* entryOut) {
        return disassembleEntry(bus, addr, entryOut);
    }
};

/**
 * Abstract interface for a CPU core.
 */
class ICore : public ICpuRegs, public ICpuDisasm, public ISnapshotable {
public:
    virtual ~ICore();

    // ISA identification
    virtual const char* isaName()     const = 0;
    virtual const char* variantName() const = 0;
    virtual uint32_t    isaCaps()     const = 0;

    // Convenience accessors
    virtual uint32_t pc() const = 0;
    virtual void     setPc(uint32_t addr) = 0;
    virtual uint32_t sp() const = 0;

    // Execution
    virtual int  step()  = 0;   // Returns cycle count
    virtual void reset() = 0;

    // Interrupts
    virtual void triggerIrq()   = 0;
    virtual void triggerNmi()   = 0;
    virtual void triggerReset() = 0;
    virtual void setIrqLine(bool asserted) { if (asserted) triggerIrq(); }
    virtual void setNmiLine(bool asserted) { if (asserted) triggerNmi(); }
    virtual void setHaltLine(bool asserted) { (void)asserted; }
    virtual bool isHalted() const { return false; }

    /** Return a named signal line output (e.g. "loram"/"hiram"/"charen" for MOS6510). */
    virtual ISignalLine* getSignalLine(const char*) { return nullptr; }

    // Step-over / step-out heuristics
    virtual int  isCallAt    (IBus* bus, uint32_t addr) = 0;
    virtual bool isReturnAt  (IBus* bus, uint32_t addr) = 0;
    virtual bool isProgramEnd(IBus* bus)                = 0;

    // Helper to write a call harness
    virtual int writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) = 0;

    // Bus attachment
    virtual void setDataBus(IBus* bus) = 0;
    virtual void setCodeBus(IBus* bus) = 0;
    virtual void setIoBus  (IBus* bus) = 0;

    virtual IBus* getDataBus() const = 0;
    virtual IBus* getCodeBus() const = 0;
    virtual IBus* getIoBus()   const = 0;

    virtual uint64_t cycles() const = 0;

    // Memory map support (MEGA65 MAP instruction)
    virtual void          setMapMmu(IMapController*) { }
    virtual IMapController* getMapMmu() const { return nullptr; }

    void setObserver(ExecutionObserver* obs) { m_observer = obs; }
    ExecutionObserver* getObserver() const { return m_observer; }

    virtual void setLogger(void* handle, void (*logFn)(void*, int, const char*)) {
        m_logger = handle;
        m_logNamed = logFn;
    }

    virtual void log(int level, const char* msg) const {
        if (m_logger && m_logNamed) m_logNamed(m_logger, level, msg);
    }

protected:
    ExecutionObserver* m_observer = nullptr;
    void*              m_logger = nullptr;
    void               (*m_logNamed)(void*, int, const char*) = nullptr;
};
