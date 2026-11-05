#pragma once

// Commander X16 HostFS: a host folder served as an IEEE/CBM DOS unit by
// intercepting the KERNAL's IEEE API (LISTEN, SECOND, CIOUT, MCIOUT, UNLSN,
// TALK, TKSA, ACPTR, MACPTR, UNTLK). The DOS itself is CbmHostDos.
//
// Ported from x16-emulator src/ieee.c and the intercept in src/main.c,
// Copyright (c) 2022, 2023 Michael Steil, et al. All rights reserved.
// License: 2-clause BSD.

#include "libcore/main/intercept.h"
#include "plugins/devices/ieee488/main/cbm_host_dos.h"
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ICore;
class IBus;
struct MachineDescriptor;

/**
 * Intercept type "x16.hostfs": a host folder as CBM DOS unit `unit`
 * (default 8, range 8-31), the way x16-emulator's HostFS does it: LOAD,
 * SAVE, OPEN/CLOSE/GET#/PRINT#, `$` directories (with CMDR-DOS's filters,
 * `$=C`, `$=T`, `$=L`), the error channel and DOS commands (CD, MD, RD,
 * S, R, P, T, I, UI, U0>, S-8/9).
 *
 *   { "type": "x16.hostfs", "unit": 8, "path": "." }
 *
 * Settings: `path` (the folder; also the root nothing can resolve outside
 * of), `startin` (the starting directory inside it; default: the root),
 * `unit`. It fires only while the KERNAL ROM bank is mapped (the "MIST"
 * signature at $FFF6). If a probe says an SD card is attached and the unit
 * is 8, it steps aside so the SD card (CMDR-DOS) serves unit 8.
 */
class X16HostFs : public CbmHostDos, public IPcIntercept {
public:
    X16HostFs() = default;
    /** From an intercept entry's settings (throws on a bad unit). */
    explicit X16HostFs(const nlohmann::json& settings);
    ~X16HostFs() override = default;

    std::vector<uint32_t> addresses() const override;
    bool onExecute(MachineDescriptor& m, ICore& cpu, IBus& bus, uint32_t pc) override;
    bool setOption(const std::string& key, const std::string& value) override;
    std::string describe() const override;

    /** Machine reset (x16-emulator ieee_init): close channels, back to the start directory. */
    void reset() override;
    /** Called at each KERNAL call: true when an SD card is attached. */
    void setSdCardProbe(std::function<bool()> probe) { m_sdCardProbe = std::move(probe); }

    /** Serve one KERNAL call at `pc` (no machine needed); true if handled. */
    bool handle(ICore& cpu, IBus& bus, uint32_t pc);

private:
    // The KERNAL's block calls (x16-emulator MACPTR / MCIOUT)
    int macptr(uint16_t addr, uint16_t* count, bool streamMode);
    int mciout(uint16_t addr, uint16_t* count, bool streamMode);

    // The DOS's activity and error state, mirrored in the KERNAL's cbdos_flags
    void onActivity(bool active) override;
    void onErrorFlag(bool error) override;

    void locateKernalVariables();
    void setKernalStatus(uint8_t s);
    void setCbdosFlags(uint8_t flags);
    uint8_t getCbdosFlags();
    uint8_t read6502(uint16_t addr);
    void write6502(uint16_t addr, uint8_t v);

    std::function<bool()> m_sdCardProbe;
    bool m_kernalLocated = false;
    uint16_t m_cbdosFlags = 0;                  // KERNAL variable addresses (0: not found)
    uint16_t m_statusAddr = 0;
    IBus* m_bus = nullptr;                      // during a call
};

/** Register the "x16.hostfs" intercept type (idempotent). */
void registerX16HostFsIntercept();
