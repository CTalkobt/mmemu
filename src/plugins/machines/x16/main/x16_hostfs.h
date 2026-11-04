#pragma once

// Commander X16 HostFS: a host folder served as an IEEE/CBM DOS unit by
// intercepting the KERNAL's IEEE API (LISTEN, SECOND, CIOUT, MCIOUT, UNLSN,
// TALK, TKSA, ACPTR, MACPTR, UNTLK).
//
// Ported from x16-emulator src/ieee.c and the intercept in src/main.c,
// Copyright (c) 2022, 2023 Michael Steil, et al. All rights reserved.
// License: 2-clause BSD.

#include "libcore/main/intercept.h"
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
class X16HostFs : public IPcIntercept {
public:
    X16HostFs() = default;
    /** From an intercept entry's settings (throws on a bad unit). */
    explicit X16HostFs(const nlohmann::json& settings);
    ~X16HostFs() override;

    std::vector<uint32_t> addresses() const override;
    bool onExecute(MachineDescriptor& m, ICore& cpu, IBus& bus, uint32_t pc) override;
    bool setOption(const std::string& key, const std::string& value) override;
    std::string describe() const override;

    /** Machine reset (x16-emulator ieee_init): close channels, back to the start directory. */
    void reset();
    /** Called at each KERNAL call: true when an SD card is attached. */
    void setSdCardProbe(std::function<bool()> probe) { m_sdCardProbe = std::move(probe); }

    int unit() const { return m_unit; }
    const std::string& rootPath() const { return m_rootSetting; }

    /** Serve one KERNAL call at `pc` (no machine needed); true if handled. */
    bool handle(ICore& cpu, IBus& bus, uint32_t pc);

private:
    struct Channel {
        std::string name;
        bool read = false;
        bool write = false;
        std::FILE* f = nullptr;
    };

    // The KERNAL IEEE API: >= 0 a STATUS value, -1 handled without STATUS,
    // -2 not handled (another unit: the KERNAL runs it), -3 unsupported (SEC)
    int second(uint8_t a);
    int tksa(uint8_t a);
    int acptr(uint8_t* a);
    int ciout(uint8_t a);
    int untlk();
    int unlsn();
    int listen(uint8_t a);
    int talk(uint8_t a);
    int macptr(uint16_t addr, uint16_t* count, bool streamMode);
    int mciout(uint16_t addr, uint16_t* count, bool streamMode);

    bool init();
    void locateKernalVariables();
    void setKernalStatus(uint8_t s);
    void setCbdosFlags(uint8_t flags);
    uint8_t getCbdosFlags();
    uint8_t read6502(uint16_t addr);
    void write6502(uint16_t addr, uint8_t v);

    void clearError();
    void setError(int e, int t, int s);
    void setErrorText(int e, const char* text, int t, int s);
    void setActivity(bool active);

    void command(const std::string& cmd);
    void cchdir(const std::string& dir);
    void cmkdir(const std::string& dir);
    void crmdir(const std::string& dir);
    void cunlink(const std::string& f);
    void crename(const std::string& f);
    void cseek(int channel, uint32_t pos);
    void ctell(int channel);
    int  copen(int channel);
    void cclose(int channel);

    std::optional<std::string> parseDosFilename(const std::string& name, bool dirHandling);
    std::optional<std::string> resolvePathUtf8(const std::string& name, bool mustExist, int wildcardType);
    std::optional<std::string> resolvePathIso(const std::string& name, bool mustExist, int wildcardType);
    void cgetcwd(uint8_t* buf, size_t len);
    std::vector<uint8_t> createDirectoryListing(const std::string& dirstring);
    std::vector<uint8_t> continueDirectoryListing();
    std::vector<uint8_t> createCwdListing();
    void closeDirectory();

    // Settings
    int m_unit = 8;
    std::string m_rootSetting = ".", m_startinSetting;
    std::function<bool()> m_sdCardProbe;

    // State (x16-emulator ieee.c globals)
    bool m_initialized = false;
    bool m_kernalLocated = false;
    std::string m_fsroot, m_startin, m_cwd;
    uint8_t m_error[256] = {};
    int m_errorLen = 0, m_errorPos = 0;
    std::string m_cmd;
    int m_channel = 0;
    bool m_listening = false, m_talking = false, m_opening = false;
    bool m_overwrite = false, m_pathExists = false;

    std::vector<uint8_t> m_dirlist;
    size_t m_dirlistPos = 0;
    bool m_dirlistEof = true;
    bool m_dirlistTimestamps = false, m_dirlistLong = false;
    void* m_dirlistDirp = nullptr;              // DIR*
    std::string m_dirlistWildcard;
    char m_dirlistTypeFilter = 0;

    uint16_t m_cbdosFlags = 0;                  // KERNAL variable addresses (0: not found)
    uint16_t m_statusAddr = 0;
    Channel m_channels[16];

    IBus* m_bus = nullptr;                      // during a call
};

/** Register the "x16.hostfs" intercept type (idempotent). */
void registerX16HostFsIntercept();
