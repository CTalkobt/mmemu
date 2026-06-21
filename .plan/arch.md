# mmsim: Multi-Machine Simulator — Implementation Plan

## 1. Goals

Build a universal 8/16-bit emulation platform where:

- Any CPU core can be combined with any memory model and any set of devices.
- New CPUs, devices, and complete machine presets are shipped as `.so`/`.dylib`/`.dll` plugins loaded at runtime.
- The host (this repo) provides all infrastructure; plugins provide only their ISA-specific logic.
- A developer writing a plugin sees a clear, stable ABI and needs no knowledge of the host internals.

**Open source (this repo):** all infrastructure, 65xx CPU cores, CBM/X16 machine presets, all existing devices.
**Commercial plugins (private repos):** Z80, M6809, 68000, ARM, AVR, 8051, PIC, TMS320 cores and their machine presets.

---

## 2. Core Interfaces

Everything else in the system composes these three.

### 2.1 `IBus` — Address / Memory Bus

```cpp
// src/libmem/main/ibus.h

enum class BusRole { DATA, CODE, IO_PORT, DMA };

struct BusConfig {
    uint32_t addrBits;     // 16, 24, 28, 32
    uint32_t dataBits;     // 8, 16, 32
    BusRole  role;
    bool     littleEndian;
    uint32_t addrMask;     // (addrBits == 32) ? 0xFFFFFFFF : ((1u << addrBits) - 1)
};

class IBus {
public:
    virtual ~IBus() {}

    virtual const BusConfig& config() const = 0;
    virtual const char*      name()   const = 0;  // "data", "code", "io_port"

    virtual uint8_t  read8 (uint32_t addr) = 0;
    virtual void     write8(uint32_t addr, uint8_t val) = 0;

    // Multi-byte helpers — default: assemble from read8/write8
    virtual uint16_t read16 (uint32_t addr);
    virtual void     write16(uint32_t addr, uint16_t val);
    virtual uint32_t read32 (uint32_t addr);
    virtual void     write32(uint32_t addr, uint32_t val);

    // Side-effect-free read for debugger/disassembler
    virtual uint8_t peek8(uint32_t addr) { return read8(addr); }

    virtual void reset() {}

    // Snapshot support
    virtual size_t stateSize()             const { return 0; }
    virtual void   saveState(uint8_t *buf) const { (void)buf; }
    virtual void   loadState(const uint8_t *buf) { (void)buf; }

    // Write-log for debug snapshot/diff
    virtual int  writeCount()                                               const { return 0; }
    virtual void getWrites(uint32_t *addrs, uint8_t *before,
                            uint8_t *after, int max)                        const {}
    virtual void clearWriteLog() {}

    ExecutionObserver* observer = nullptr;
};
```

**Concrete implementations:**

| Class             | Use case                                                      | Addr bits    |
|-------------------|---------------------------------------------------------------|--------------|
| `Memory6502Bus`   | 6502 family — wraps existing `memory_t` unchanged             | 16           |
| `FlatMemoryBus`   | Z80, small systems — heap array + IORegistry overlay          | configurable |
| `PagedMemoryBus`  | Banked 16-bit machines (C128 MMU, BBC paged ROM, NES mappers) | 16 + bank    |
| `SparseMemoryBus` | Wide address spaces — lazy 4 KB page allocation               | configurable |

`SparseMemoryBus(28)` is exactly what the current `far_pages[65536]` implements — generalized. Any architecture needing a sparse wide address space (45GS02 at 28-bit, 65C816 at 24-bit, ARM at 32-bit) uses this class.

---

### 2.2 `ICore` — CPU Core

```cpp
// src/libcore/main/icore.h

enum class RegWidth { R8 = 1, R16 = 2, R32 = 4 };

struct RegDescriptor {
    const char *name;       // "A", "PC", "BC", "CPSR"
    RegWidth    width;
    uint32_t    flags;      // REGFLAG_* bitmask
    const char *alias;      // e.g. "F" for 6502 "P"; nullptr if none
};

#define REGFLAG_PC          0x01  // program counter
#define REGFLAG_SP          0x02  // stack pointer
#define REGFLAG_STATUS      0x04  // flags / status register
#define REGFLAG_ACCUMULATOR 0x08  // primary accumulator
#define REGFLAG_INTERNAL    0x10  // hidden from UI (e.g. eom_prefix)
#define REGFLAG_SHADOW      0x20  // alternate/shadow register (Z80 AF', BC', ...)

#define ISA_CAP_HARVARD       0x0001  // separate code/data address spaces
#define ISA_CAP_IO_PORT_SPACE 0x0002  // separate I/O port space (Z80 IN/OUT)
#define ISA_CAP_32BIT_ADDR    0x0004  // address space > 16 bits
#define ISA_CAP_MULTI_PREFIX  0x0008  // multi-byte opcode prefixes (Z80 CB/DD/ED/FD)
#define ISA_CAP_16BIT_DATA    0x0010  // native 16-bit data (65C816, Z80)
#define ISA_CAP_THUMB_MODE    0x0020  // ARM Thumb / mode-switching

class ICore {
public:
    virtual ~ICore() {}

    // ISA identification
    virtual const char *isaName()     const = 0;  // "6502", "z80", "arm-cortex-m0"
    virtual const char *variantName() const = 0;  // "45GS02", "Z80B", "STM32F1"
    virtual uint32_t    isaCaps()     const = 0;  // ISA_CAP_* bitmask

    // Register descriptor table
    virtual int                  regCount()             const = 0;
    virtual const RegDescriptor *regDescriptor(int idx) const = 0;
    virtual uint32_t             regRead (int idx)      const = 0;
    virtual void                 regWrite(int idx, uint32_t val) = 0;

    // Named register helpers — default: scan descriptor table
    virtual uint32_t regReadByName (const char *name) const;
    virtual void     regWriteByName(const char *name, uint32_t val);
    virtual int      regIndexByName(const char *name) const;

    // Convenience accessors (resolve via REGFLAG_PC / REGFLAG_SP)
    virtual uint32_t pc() const = 0;
    virtual void     setPc(uint32_t addr) = 0;
    virtual uint32_t sp() const = 0;

    // Execution
    virtual int  step()  = 0;   // advance one instruction; returns cycle count
    virtual void reset() = 0;

    // Interrupts
    virtual void triggerIrq()   = 0;
    virtual void triggerNmi()   = 0;
    virtual void triggerReset() = 0;

    // Disassembly
    virtual int disassembleOne  (IBus *bus, uint32_t addr, char *buf, int bufsz) = 0;
    virtual int disassembleEntry(IBus *bus, uint32_t addr, void *entryOut)      = 0;

    // Snapshot support
    virtual size_t stateSize()             const = 0;
    virtual void   saveState(uint8_t *buf) const = 0;
    virtual void   loadState(const uint8_t *buf) = 0;

    // Step-over / step-out heuristics — ISA-specific
    virtual int  isCallAt    (IBus *bus, uint32_t addr) = 0;  // returns insn size or 0
    virtual bool isReturnAt  (IBus *bus, uint32_t addr) = 0;
    virtual bool isProgramEnd(IBus *bus)                = 0;  // RTS on empty stack, etc.

    // Write a "call routine; halt/terminate" stub at scratchAddr
    // Returns number of bytes written
    virtual int writeCallHarness(IBus *bus, uint32_t scratch, uint32_t routineAddr) = 0;

    // Bus attachment
    virtual void setDataBus(IBus *bus) = 0;
    virtual void setCodeBus(IBus *bus) = 0;  // no-op for Von Neumann CPUs
    virtual void setIoBus  (IBus *bus) = 0;  // no-op unless ISA_CAP_IO_PORT_SPACE

    virtual uint64_t cycles() const = 0;

    ExecutionObserver *observer = nullptr;
};
```

**6502 family:** `MOS6502` class implements `ICore`. `isCallAt()` checks opcode `$20`. `writeCallHarness()` writes `$20 lo hi $60`.

---

### 2.3 `MachineDescriptor` — Machine Configuration

```cpp
// src/libcore/main/machine_desc.h

struct CpuSlot {
    std::string slotName;    // "main", "z80", "coprocessor"
    ICore      *cpu;         // owned
    IBus       *dataBus;
    IBus       *codeBus;     // same as dataBus for Von Neumann
    IBus       *ioBus;       // nullptr unless ISA_CAP_IO_PORT_SPACE
    bool        active;      // for machines where only one CPU runs at a time
    uint64_t    clockDiv;    // relative to master clock (1 = same speed)
};

struct BusSlot {
    std::string busName;     // "system", "fast", "cartridge"
    IBus       *bus;         // owned
};

struct MachineDescriptor {
    std::string machineId;      // "c64", "c128", "mega65", "frankenstein:user1"
    std::string displayName;
    std::string licenseClass;   // "open", "commercial"

    std::vector<CpuSlot>  cpus;
    std::vector<BusSlot>  buses;

    IORegistry *ioRegistry = nullptr;

    // Lifecycle hooks
    std::function<void(MachineDescriptor&)> onReset;
    std::function<void(MachineDescriptor&)> onInit;

    /** Key injection hook — set by machines that have a keyboard matrix. */
    std::function<bool(const std::string& keyName, bool down)> onKey;

    /** Joystick injection hook. */
    std::function<void(int port, uint8_t bits)> onJoystick;

    // Scheduler: which CPU runs next and for how many cycles
    std::function<int(MachineDescriptor&)> schedulerStep;

    std::vector<MemOverlay> overlays;
    std::vector<uint8_t*>   roms; // Buffers owned by the machine
};
```

---

## 3. Plugin ABI

### 3.1 Public Header

```c
// src/include/mmemu_plugin_api.h

#define MMEMU_PLUGIN_API_VERSION  0x00010000u
#define MMEMU_PLUGIN_API_MAJOR(v) ((v) >> 16)
#define MMEMU_PLUGIN_API_MINOR(v) ((v) & 0xFFFFu)

typedef struct {
    uint32_t apiVersion;
    const char* pluginId;
    const char* displayName;
    const char* version;

    const char* const* deps;                 // Null-terminated list of required plugin IDs
    const char* const* supportedMachineIds;  // Null-terminated list; nullptr = all machines

    int coreCount;
    struct CorePluginInfo* cores;

    int toolchainCount;
    struct ToolchainPluginInfo* toolchains;

    int deviceCount;
    struct DevicePluginInfo* devices;

    int machineCount;
    struct MachinePluginInfo* machines;
} SimPluginManifest;

// Every plugin .so exports this symbol:
SimPluginManifest *mmemuPluginInit(const SimPluginHostAPI *host);
```

`SimPluginHostAPI` provides host services (logging, registry access).

### 3.2 Plugin Loader

```cpp
// src/plugin_loader/main/plugin_loader.h

class PluginLoader {
public:
    static PluginLoader& instance();

    bool load(const std::string& path);
    void loadFromDir(const std::string& dir);
    void unloadAll();

    // Host-provided extension registration stubs
    void setPaneRegisterFn(void (*fn)(const PluginPaneInfo*));
    void setCommandRegisterFn(void (*fn)(const PluginCommandInfo*));
    void setMcpToolRegisterFn(void (*fn)(const PluginMcpToolInfo*));
};
```

### 3.3 License Enforcement

License checking is entirely the plugin's responsibility. The host enforces nothing.

---

### 3.4 Writing a Plugin — Minimal Example (Z80)

```cpp
// In mmemu-plugin-z80/z80_core.cpp

class CPUZ80 : public ICore {
    uint16_t m_af, m_bc, m_de, m_hl;
    uint16_t m_af2, m_bc2, m_de2, m_hl2;  // shadow set
    uint16_t m_ix, m_iy, m_sp, m_pc;
    IBus *m_memBus = nullptr;
    IBus *m_ioBus  = nullptr;

    static const RegDescriptor sRegs[];

public:
    const char *isaName()     const override { return "z80"; }
    const char *variantName() const override { return "Z80"; }
    uint32_t    isaCaps()     const override {
        return ISA_CAP_IO_PORT_SPACE | ISA_CAP_16BIT_DATA | ISA_CAP_MULTI_PREFIX;
    }

    int         regCount()             const override { return 18; }
    const RegDescriptor *regDescriptor(int i) const override { return &sRegs[i]; }
    uint32_t    regRead (int i)        const override { /* ... */ }
    void        regWrite(int i, uint32_t v)   override { /* ... */ }

    uint32_t pc()            const override { return m_pc; }
    void     setPc(uint32_t a)    override { m_pc = (uint16_t)a; }
    uint32_t sp()            const override { return m_sp; }

    void setDataBus(IBus *b) override { m_memBus = b; }
    void setIoBus  (IBus *b) override { m_ioBus  = b; }
    void setCodeBus(IBus *) override {}  // Von Neumann

    int step() override {
        uint8_t op = m_memBus->read8(m_pc++);
        // dispatch to prefix tables or base table
        return 0; // return cycle count
    }

    int  isCallAt    (IBus *bus, uint32_t addr) override { return 0; }
    bool isReturnAt  (IBus *bus, uint32_t addr) override { return false; }
    bool isProgramEnd(IBus *bus)                override { return false; }

    int writeCallHarness(IBus *bus, uint32_t scratch, uint32_t target) override {
        return 4;
    }

    uint64_t cycles() const override { return 0; }
    // ... reset, triggerIrq, triggerNmi, saveState, loadState, disassembleOne
};

// Plugin entry point
extern "C" SimPluginManifest *mmemuPluginInit(const SimPluginHostAPI *host) {
    static CorePluginInfo cores[] = {{
        "z80", "Z80", "commercial",
        []() -> ICore* { return new CPUZ80(); }
    }};
    static SimPluginManifest manifest = {
        MMEMU_PLUGIN_API_VERSION, "z80-core", "Z80 Core", "1.0.0",
        nullptr, nullptr,
        1, cores, 0, nullptr, 0, nullptr, 0, nullptr
    };
    return &manifest;
}
```

---

## 4. Memory Architecture

### 4.1 Categories of Extended Memory

#### Category A — CPU-Native Bank Registers

The CPU emits a full physical address; the bus sees the wide address directly.

Use `SparseMemoryBus(24)` for 65C816, `SparseMemoryBus(28)` for 45GS02, `SparseMemoryBus(32)` for ARM.

#### Category B — External Banking Hardware (Window Mapping)

The CPU sees a 16-bit address space; an external chip remaps a window to different physical pages.

Use `PagedMemoryBus`.

---

## 5. Toolchain Interfaces

### 5.1 `IDisassembler`

```cpp
// src/libtoolchain/main/idisasm.h

struct DisasmEntry {
    uint32_t    addr;
    int         bytes;        // number of bytes consumed
    std::string mnemonic;     // e.g. "LDA"
    std::string operands;     // e.g. "#$42"
    std::string complete;     // full formatted string
    bool        isCall;
    bool        isReturn;
    bool        isBranch;
    uint32_t    targetAddr;   // branch/call destination
};

class IDisassembler {
public:
    virtual const char *isaName() const = 0;
    virtual int disasmOne  (IBus *bus, uint32_t addr, char *buf, int bufsz) = 0;
    virtual int disasmEntry(IBus *bus, uint32_t addr, DisasmEntry &entry) = 0;
};
```

### 5.2 `IAssembler`

```cpp
// src/libtoolchain/main/iassembler.h

struct AssemblerResult {
    bool        success;
    std::string errorMessage;
    std::string outputPath;
    std::string symPath;
    std::string listPath;
    int         errorCount;
    int         warningCount;
};

class IAssembler {
public:
    virtual const char    *name() const = 0;
    virtual bool           isaSupported(const std::string& isa) const = 0;
    virtual AssemblerResult assemble(const std::string& sourcePath, 
                                     const std::string& outputPath) = 0;
    virtual int assembleLine(const std::string& line, uint8_t* buf, int bufsz, 
                             uint32_t currentAddr = 0);
};
```

---

## 6. Debug Infrastructure

Execution management, breakpoints, and snapshots are handled by `DebugContext`.

```cpp
// src/libdebug/main/debug_context.h

class DebugContext : public ExecutionObserver {
public:
    DebugContext(ICore* cpu, IBus* bus);

    void onStep(ICore* cpu, IBus* bus, const DisasmEntry& entry) override;
    void onMemoryWrite(IBus* bus, uint32_t addr, uint8_t before, uint8_t after) override;

    BreakpointList& breakpoints();
    TraceBuffer&    trace();

    int  saveSnapshot(const std::string& label);
    bool restoreSnapshot(int index);
    std::vector<uint32_t> diffSnapshots(int idxA, int idxB);
};
```

---

## 7. Frankenstein Machine Support

A Frankenstein machine is any combination of cores, buses, and devices assembled at runtime — pick any `ICore`, any `IBus`, attach any `IOHandler` devices at any addresses.

---

## 8. GUI Register Pane

`pane_registers.cpp` is driven entirely from `ICore` descriptor tables — no hardcoded CPU knowledge.

---

## 9. Implementation Phases

### Phase 1 — IBus and Memory6502Bus (Done)
**Deliverable:** `IBus` interface + `Memory6502Bus` wrapper + `FlatMemoryBus`.

### Phase 2 — ICore on CPU Hierarchy (Done)
**Deliverable:** `ICore` abstract class. `MOS6502` class implements `ICore`.

### Phase 3 — MachineDescriptor Replaces machine_init_hardware (In Progress)
**Deliverable:** `MachineDescriptor` structure and `MachineRegistry`.

### Phase 4 — Plugin Loader (In Progress)
**Deliverable:** `PluginLoader` implementation. `mmemu_plugin_api.h` established.

### Phase 5 — CLI Target Implementation (In Progress)
**Deliverable:** REPL with support for multiple machines and cores.

---

## 10. File Summary

```
src/include/
  mmemu_plugin_api.h                Public ABI contract for all plugins

src/libmem/main/
  ibus.h                            IBus abstract interface
  memory_bus.h / memory_bus.cpp     FlatMemoryBus, PagedMemoryBus

src/libcore/main/
  icore.h                           ICore abstract CPU interface
  machine_desc.h                    MachineDescriptor, CpuSlot, BusSlot
  machines/
    machine_registry.h / .cpp       MachineRegistry lookup

src/libtoolchain/main/
  idisasm.h                         IDisassembler interface
  iassembler.h                      IAssembler interface

src/libdebug/main/
  debug_context.h / .cpp            DebugContext session management
  breakpoint_list.h / .cpp
  trace_buffer.h / .cpp

src/plugin_loader/main/
  plugin_loader.h / .cpp            Dynamic plugin management
```

---

## 11. Open vs. Commercial Boundary

**Open source (this repo):**
- All modular libraries, CLI, GUI, MCP.
- 6502/65C02/65CE02/45GS02 CPU cores and opcode tables.
- VIC-II, SID, CIA, MEGA65 math/DMA device handlers.
- Machine presets: `raw6502`, `c64`, `c128`, `mega65`, `x16`, `pet`.
- Plugin loader infrastructure and ABI headers.

**Commercial plugins (private repos):**

| Plugin                | CPU Cores              | Machine Presets                               |
|-----------------------|------------------------|-----------------------------------------------|
| `mmemu-plugin-z80`    | Z80, Z80B, CMOS-Z80    | ZX Spectrum 48K/128K, ZX81, generic CP/M, MSX |
| `mmemu-plugin-6809`   | M6809, HD6309          | CoCo 3, Dragon 32                             |
| `mmemu-plugin-68000`  | 68000/10/20            | Amiga 500/1200, Atari ST, Sega Genesis        |
| `mmemu-plugin-arm`    | Cortex-M0/M3/M4        | STM32F1, RP2040                               |
| `mmemu-plugin-avr`    | ATmega328p, ATmega2560 | Arduino Uno, Arduino Mega                     |
| `mmemu-plugin-8051`   | 8051, AT89C51          | Generic 8051 dev board                        |
| `mmemu-plugin-pic`    | PIC16F, PIC18F         | Generic PIC dev board                         |
| `mmemu-plugin-tms320` | TMS320C28x             | Generic DSP eval board                        |

---

## 12. Device Reference

Minimum device sets for running real software on each target machine preset.

### 12.0 65xx Family (Open Source)

#### VIC-20

| Chip                | Role                                                             | Address     |
|---------------------|------------------------------------------------------------------|-------------|
| MOS 6560 / 6561 VIC | 22×23 char video, 3.5-channel sound, light pen, raster           | $9000–$900F |
| MOS 6522 VIA #1     | Serial bus, joystick port 1, cassette, keyboard column scan      | $9110–$911F |
| MOS 6522 VIA #2     | Serial bus ATN/SRQ, user port, cassette sense, keyboard row scan | $9120–$912F |

#### Commodore PET / CBM

| Chip | Role | Address |
|---|---|---|
| MOS 6820 / 6821 PIA #1 | Keyboard matrix scan | $E810–$E813 |
| MOS 6820 / 6821 PIA #2 | IEEE-488 bus control, cassette #1 | $E820–$E823 |
| MOS 6522 VIA | Cassette #2, 60 Hz IRQ | $E840–$E84F |
| Motorola MC6845 CRTC | Programmable video timing (80-col boards) | $E880–$E881 |

#### Commodore Plus/4 and C16

| Chip | Role | Address |
|---|---|---|
| MOS 7360 / 8360 TED | Video, 2-channel sound, keyboard scan, timers, IRQ/NMI | $FF00–$FF3F |
| MOS 6551 ACIA (Plus/4) | RS-232 serial | $FD00–$FD0F |

#### Atari 400 / 800 / XL / XE

| Chip | Role | Address |
|---|---|---|
| ANTIC | Display-list DMA, NMI (VBI/DLI) | $D400–$D40F |
| CTIA / GTIA | Player-missile sprites, collision, color palette | $D000–$D01F |
| POKEY | 4-channel sound, keyboard scan, serial I/O, ADC, IRQ | $D200–$D20F |
| PIA (MOS 6520) | Joystick direction bits, cassette motor, bank-select (XL/XE) | $D300–$D30F |

#### Atari 2600 (VCS)

| Chip | Role | Address |
|---|---|---|
| TIA | Scanline video, 2-channel sound, joystick/paddle read, collision | Write $00–$2C; Read $00–$0D |
| RIOT (MOS 6532) | 128-byte RAM, two 8-bit I/O ports, interval timer | $280–$29F |

#### Atari 5200 SuperSystem

| Chip | Role | Address |
|---|---|---|
| GTIA | Sprites, color, priority | $C000–$C0FF |
| POKEY (×2 on some boards) | 4-channel sound, analog controller ADC | $E800–$E8FF |
| ANTIC | Display list, DMA | $D400–$D4FF |
| PIA (6821) | Controller port routing | $E900–$E9FF |

#### Apple II / II+ / IIe / IIc

| Chip / Mechanism | Role | Address |
|---|---|---|
| Soft switches (IOU region) | Video mode, page flip, 80-col, alt-charset | $C000–$C0FF |
| Keyboard latch | Last key pressed + strobe | $C000 (read), $C010 (strobe clear) |
| Speaker toggle | 1-bit audio | $C030 |
| Game port / paddles | 4 analog paddle, 4 pushbutton | $C061–$C070 |
| Language Card (16 KB) | RAM banking $D000–$FFFF over ROM | $C080–$C08F |
| IIe MMU / IOU | Auxiliary 64 KB slot, main/aux bank switching | $C002–$C00F |
| Disk II interface | Stepper motor phases, drive select, R/W shift register | $C0E0–$C0EF (slot 6) |

#### Apple IIgs

All Apple IIe devices, plus:

| Chip | Role | Address |
|---|---|---|
| Ensoniq ES5503 DOC | 32-voice wavetable oscillators | $C03C–$C03F + $E10000–$E1003F |
| VGC | Super Hi-Res 320×200 / 640×200, per-scanline palettes | $C029, $C034, $C035 |
| Mega II | ADB keyboard/mouse, IWM floppy, IIe soft-switch compatibility | $C000–$C0FF |
| IWM | 3.5" / 5.25" floppy disk controller | $C0E0–$C0EF |

#### BBC Micro / Acorn Electron

| Chip | Role | Address |
|---|---|---|
| Motorola MC6845 CRTC | Programmable video timing (modes 0–7) | $FE00–$FE01 |
| MOS 6522 System VIA | Keyboard matrix, sound latch (SN76489), joystick, timers | $FE40–$FE5F |
| MOS 6522 User VIA | User port, joystick analogue latch, printer | $FE60–$FE7F |
| SN76489 PSG | 3-channel square + noise | via VIA latch |
| MOS 6850 ACIA | RS-423 serial (cassette and Econet) | $FE08–$FE0F |
| Paged ROM / RAM latch | 4-bit select for up to 16 ROM/RAM banks | $FE30 |
| Tube interface | Second-processor bus (6502, Z80, ARM co-pro) | $FEE0–$FEE7 |

#### NES / Famicom

| Chip | Role | Address (CPU bus) |
|---|---|---|
| PPU (Ricoh 2C02 / 2C07) | Tile BG, 64 sprites, 4 palettes, vblank NMI | $2000–$2007 (regs), $4014 (OAM DMA) |
| APU (inside Ricoh 2A03 / 2A07) | 2 pulse, 1 triangle, 1 noise, 1 DMC; controller read; DMA trigger | $4000–$4017 |
| Mapper hardware | CHR/PRG bank switching | Varies per mapper |
| Standard controller | Shift-register serial read (8 buttons) | $4016 (port 1), $4017 (port 2) |

#### SNES / Super Famicom (65816)

| Chip | Role | Address |
|---|---|---|
| PPU1 + PPU2 (Ricoh 5C77 + 5C78) | 8 BG modes, 128 sprites, HDMA, color math, window masking | $2100–$213F |
| SPC700 + Sony DSP (APU) | 8-channel ADPCM sound, echo, pitch mod | $2140–$2143 |
| DMA / HDMA | 8-channel general DMA + H-blank DMA | $4300–$437F |
| WRAM registers | 128 KB work RAM sequential-access port | $2180–$2183 |
| Joypads | Auto-read 16-bit serial latch | $4016–$4017, $4218–$421F |
| Math unit | 16×16→32 multiplier, 32÷16 divider | $4202–$4217 |

---

## 13. Debugger & Interactive UI Components

These components form the interactive layer between the user, the simulator session, and the assembled source. Each pane is driven by existing interfaces (`ICore`, `IBus`, `IDisassembler`, `source_map_t`, `symbol_table_t`, `breakpoint_list_t`) and introduces no new coupling to machine-specific logic.

---

### 13.1 CPU Registers Pane (`pane_registers`)

Already described in §8. Summary of additional behaviour:

- Flag bits in the STATUS register are decoded into individual named bits (N/V/B/D/I/Z/C for 6502; S/Z/H/P/N/C for Z80) displayed as a horizontal bit row beneath the register value.
- Changed registers since the last step are highlighted.
- Inline editing calls `regWriteByName(s, name, val)`.
- For multi-CPU machines (`machine->cpus.size() > 1`) a **CPU slot selector** dropdown appears at the top of the pane; selecting a slot switches the `ICore*` context for all register reads/writes without affecting execution.

---

### 13.2 Disassembly / PC View (`pane_disasm`)

A scrollable listing of decoded instructions centred on the current PC.

Features:
- **Symbol overlay** — raw addresses replaced with `label+offset` where the symbol table resolves them.
- **Branch arrows** — for instructions where `DisasmEntry::isBranch` is true and `targetAddr` is within the visible window, a small ASCII/drawn arrow connects source and destination rows.
- **Click to set breakpoint** — clicking the address gutter toggles an execution breakpoint at that address.
- **Follow PC** — auto-scrolls to keep the current PC row centred; a toggle disables this for manual inspection.
- **Go to address / symbol** — a text field accepts a hex address or symbol name and jumps the view.

---

### 13.3 Source View (`pane_source`)

Displays the assembler source file(s) with the line currently executing highlighted.

Features:
- **Bidirectional sync** with the disassembly pane: navigating one scrolls the other.
- **Inline error display** — after an assembly run, `AssemblerResult::errorMessage` is parsed and errors are shown as gutter annotations on the relevant line.
- **Editable** — changes trigger a re-assemble via `IAssembler::assemble()`; on success the new binary is hot-loaded into the session memory without a full reset (unless the load address changed).
- **Source-level breakpoints** — right-clicking a line inserts a breakpoint at the address `source_map->sourceToAddr(file, line)`.

---

### 13.4 Memory / Hex Editor (`pane_memory`)

A hex+ASCII dump of any bus region.

Features:
- **Bus selector** — for Harvard architectures (`ISA_CAP_HARVARD`), a dropdown selects the code bus, data bus, or I/O port bus.
- **Write highlight** — bytes that changed on the last step are rendered with a coloured background; the before-value is shown in a tooltip.
- **Editable** — writes go through `IBus::write8()`; undo restores via `write8(addr, beforeValue)` from the write log.
- **Symbol jump** — address bar accepts a symbol name in addition to a hex address.
- **Search** — scan the bus for a byte sequence, word value, or ASCII string; results are listed and clickable.

---

### 13.5 Memory Map Viewer (`pane_memmap`)

A visual bar spanning the full address space, subdivided by region type.

Data sources:
- `MachineDescriptor::overlays` — ROM/RAM overlays with `active` flag.
- `IORegistry` — all registered `IOHandler` ranges.
- `IBus` concrete type — `FlatMemoryBus`, `PagedMemoryBus` window list, `SparseMemoryBus` allocated pages.

Clicking a region jumps the hex editor to the base address of that region.

---

### 13.6 Symbol Browser (`pane_symbols`)

A searchable, sortable table of all labels from `SymbolTable`.

Clicking a row jumps both the disassembly pane and the source pane to that address/line.

---

### 13.7 Breakpoints Pane (`pane_breakpoints`)

Features:
- **Types** — execution, memory read-watch, memory write-watch (write-watch backed by `IBus::getWrites` scan after each step).
- **Conditions** — simple expressions over register names and memory values; evaluated after the breakpoint address fires.
- **Hit count** — a breakpoint can be set to only trigger after N hits (useful for loops).
- **Source-linked** — breakpoints set from the source pane show the file/line rather than a raw address.

---

### 13.8 Step Controls (`toolbar_execution`)

| Button | Action | Implementation |
|---|---|---|
| Step Into | Advance one instruction | `ICore::step()` |
| Step Over | Run past a call as one unit | Run until `PC == addr + isCallAt(bus, addr)` |
| Step Out | Return from current subroutine | Run until `ICore::isReturnAt()` is true |
| Run | Free-run until breakpoint or halt | Loop `step()` checking breakpoints each iteration |
| Pause | Halt free-run | Set a stop flag checked by the run loop |
| Reset | Reset machine | `ICore::reset()`, `IBus::reset()`, `IORegistry::resetAll()` |
| Run to Cursor | Run to a specific address | Temporary execution breakpoint at target |
| Run N Cycles | Advance by a fixed cycle count | Loop until `core->cycles() >= target` |

---

### 13.9 Call Stack Pane (`pane_callstack`)

Reconstructs the call chain by walking the hardware stack and matching return addresses against known call sites.

- Reconstruction is heuristic; the pane displays a **confidence indicator** when the stack contains bytes that do not resolve to known call sites (e.g., stack used as data storage).
- Clicking a frame jumps the disassembly pane to that address.
- For non-6502 CPUs the same logic applies using the ISA-appropriate stack width and `isCallAt()`.

---

### 13.10 Instruction Trace Log (`pane_trace`)

A fixed-size circular buffer of the last N executed instructions, captured after each `ICore::step()`.

- **Scrollable backwards** — lets the user examine the path taken before a crash or unexpected state.
- **Filtered view** — filter by address range, by symbol name, or by mnemonic prefix (e.g., show only `JSR`/`RTS` entries to trace call flow).
- **Diff highlight** — registers that changed relative to the previous entry are highlighted in each row.
- Buffer size is configurable (default 1 000 entries).

---

### 13.11 Memory Write History (`pane_write_history`)

A time-ordered log of bus writes, sourced from `IBus::getWrites`.

Filterable by address range. Clicking a row jumps the hex editor to the written address.

---

### 13.12 Watch Expressions (`pane_watches`)

User-defined expressions evaluated each step using side-effect-free bus reads and register reads.

- Backed by `IBus::peek8()` (memory) or `regReadByName()` (register).
- Labels are user-supplied strings.
- Format is selectable per entry.
- Entries whose value change since the last step are highlighted.

---

### 13.13 Device / MMIO Inspector (`pane_devices`)

A tree of all registered `IOHandler` instances, with their decoded internal state.

- Driven by `IDeviceUI::ChipRegDescriptor` (per §Cross-Cutting GUI Standards in todo.md).
- Clicking a register row jumps the hex editor to that register's bus address.
- Bit-field annotations are supplied by each device's `decodeReg()` implementation.

---

### 13.14 Stack View (`pane_stack`)

A dedicated listing of the hardware stack contents.

- For 6502: `$0100`–`$01FF`, with `SP` indicated by an arrow; shows bytes as hex and, where they look like valid addresses, as `→ symbol`.
- For Z80 / 65C816 / ARM: the relevant memory region around `SP`, auto-sizing to show approximately 16 entries.
- Depth indicator shows how much stack has been consumed relative to the configured stack region size.

---

### 13.15 Interrupt / Event Log (`pane_irq_log`)

Records hardware events timestamped by `ICore::cycles()`.

- Populated when `triggerIrq()`, `triggerNmi()`, or `triggerReset()` are called.
- Useful for diagnosing raster-based interrupt timing where the exact cycle of the interrupt matters.

---

### 13.16 State Snapshot Manager (`pane_snapshots`)

Save and restore complete machine state using `ICore::saveState()` and `IBus::saveState()`.

Features:
- **Save** — appends a named snapshot to the list.
- **Restore** — calls `ICore::loadState()` and `IBus::loadState()`, then refreshes all panes.
- **Diff** — compares two snapshots: changed registers are listed; changed memory bytes are shown in the hex editor with before/after colouring.
- Stored in a session-local file alongside the project so snapshots survive across restarts.

---

### 13.17 Assemble & Load Panel (`pane_assembler`)

Surfaces the toolchain pipeline directly in the UI.

- Stdout/stderr from the assembler backend (`KickAssemblerBackend`) is displayed verbatim in a scrollable log.
- On success, the binary is written into bus memory without a full session reset unless the entry point address changed.
- The `.lst` path is also opened in a **Listing View** tab — assembler output interleaved with source, with the current PC line highlighted.

---

### 13.18 Summary of Pane–Interface Dependencies

| Pane | Primary interfaces used |
|---|---|
| Registers | `ICore::regDescriptor`, `regRead/regWrite` |
| Disassembly | `IDisassembler::disasmEntry`, `SymbolTable`, `BreakpointList` |
| Source | `SourceMap`, `IAssembler` |
| Hex Editor | `IBus::peek8`, `IBus::write8`, `IBus::getWrites` |
| Memory Map | `MachineDescriptor::overlays`, `IORegistry`, `IBus` concrete type |
| Symbol Browser | `SymbolTable`, `SourceMap` |
| Breakpoints | `BreakpointList`, `IBus::getWrites` |
| Step Controls | `ICore::step`, `isCallAt`, `isReturnAt`, `reset` |
| Call Stack | `ICore::sp`, `isCallAt`, `IBus::peek8`, `SymbolTable` |
| Trace Log | `IDisassembler::disasmOne`, `ICore::regRead`, `ICore::cycles` |
| Write History | `IBus::writeCount`, `getWrites`, `clearWriteLog` |
| Watches | `IBus::peek8`, `regReadByName` |
| Device Inspector | `IORegistry`, `IDeviceUI::ChipRegDescriptor` |
| Stack View | `ICore::sp`, `IBus::peek8`, `SymbolTable` |
| IRQ Log | `ICore::cycles`, `triggerIrq/Nmi/Reset` callbacks |
| Snapshots | `ICore::save/loadState`, `IBus::save/loadState` |
| Assemble & Load | `IAssembler`, `simLoadBinary`, `simLoadSymbols` |

---

## 14. Interface Parity (CLI & MCP)

To ensure a uniform experience across both human-driven and programmatic workflows, `mmemu-cli` and `mmemu-mcp` must maintain strict functional parity. Any capability introduced to one must have an equivalent representation in the other:

- **Execution & Debugging:** Stepping, breakpoints, watchpoints, and call stack introspection must be available symmetrically.
- **State Management:** Snapshot operations (save, diff, list, restore) and trace buffer controls must be accessible from both interfaces.
- **Memory & Registers:** Both interfaces must fully support reading, writing, filling, copying, swapping, and pattern searching across memory and CPU registers.
- **Toolchain:** On-the-fly assembly and disassembly features must be exposed equally.
- **Hardware Control:** Mounting disks/tapes, cartridge management, device info querying, and keyboard injection must be supported in both.

Discrepancies (e.g., CLI-only `screenshot` or MCP-only `set_map_state`) must be actively resolved to maintain a consistent capability matrix.
