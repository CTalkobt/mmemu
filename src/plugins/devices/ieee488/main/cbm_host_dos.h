#pragma once

// A host folder served as a Commodore DOS unit at the bus command level
// (Commodore bus L2: LISTEN/TALK/SECOND/TKSA/CIOUT/ACPTR/UNLSN/UNTLK, and L3:
// the DOS). Shared by the Commander X16's HostFS (KERNAL IEEE API traps) and
// the PET's IEEE-488 drive.
//
// Ported from x16-emulator src/ieee.c and src/iso_8859_15.c,
// Copyright (c) 2019, 2022, 2023 Michael Steil, et al. All rights reserved.
// License: 2-clause BSD (see cbm_host_dos.cpp).

#include "cbm_dos.h"
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

/**
 * CBM DOS on a host folder: LOAD, SAVE, OPEN/CLOSE/GET#/PRINT#, `$`
 * directories (with CMDR-DOS's filters `$=C`, `$=T`, `$=L`), the error
 * channel (15) and DOS commands (CD, MD, RD, S, R, P, T, I, UI, U0>, S-8/9).
 *
 * The bus calls return >= 0 a STATUS value, -1 handled without STATUS, -2 not
 * for this unit (another device must answer).
 */
class CbmHostDos : public ICbmDos {
public:
    CbmHostDos() = default;
    virtual ~CbmHostDos();
    CbmHostDos(const CbmHostDos&) = delete;
    CbmHostDos& operator=(const CbmHostDos&) = delete;

    // Settings
    void setRoot(const std::string& path) { m_rootSetting = path; m_startinSetting.clear(); reset(); }
    void setStartIn(const std::string& path) { m_startinSetting = path; reset(); }
    void setUnit(int unit) { m_unit = unit; }
    /** Load address of a `$` listing: $0801 (X16, as x16-emulator), $0401
     *  (the PET's BASIC start, as Commodore's drives send it). */
    void setDirectoryLoadAddress(uint16_t a) { m_dirLoadAddr = a; }
    int unit() const override { return m_unit; }
    const std::string& rootPath() const { return m_rootSetting; }
    /** The current directory (after the folder is first used), else empty. */
    const std::string& currentDir() const { return m_cwd; }
    const std::string& resolvedRoot() const { return m_fsroot; }

    /** Close every channel and return to the start directory (machine reset). */
    void reset() override;
    /** Resolve the folder (done at first use). False if it does not exist. */
    bool ensureInit() override { return m_initialized || init(); }

    // Commodore bus L2 (the KERNAL IEEE API, or the bus commands under ATN)
    int listen(uint8_t a) override;
    int second(uint8_t a) override;
    int ciout(uint8_t a) override;
    int unlsn() override;
    int talk(uint8_t a) override;
    int tksa(uint8_t a) override;
    int acptr(uint8_t* a) override;
    int untlk() override;

    /** Busy (between LISTEN/TALK and UNLSN/UNTLK): the drive's activity LED. */
    bool active() const override { return m_active; }
    /** The error channel holds an error (the LED flashes on a real drive). */
    bool errorPending() const { return m_errorFlag; }
    bool listening() const { return m_listening; }
    bool talking() const { return m_talking; }

protected:
    /** The activity or error state changed (the X16 mirrors it in cbdos_flags). */
    virtual void onActivity(bool /*active*/) {}
    virtual void onErrorFlag(bool /*error*/) {}

    struct Channel {
        std::string name;
        bool read = false;
        bool write = false;
        std::FILE* f = nullptr;
    };

    bool init();
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
    uint16_t m_dirLoadAddr = 0x0801;
    std::string m_rootSetting = ".", m_startinSetting;

    // State (x16-emulator ieee.c globals)
    bool m_initialized = false;
    std::string m_fsroot, m_startin, m_cwd;
    uint8_t m_error[256] = {};
    int m_errorLen = 0, m_errorPos = 0;
    std::string m_cmd;
    int m_channel = 0;
    bool m_listening = false, m_talking = false, m_opening = false;
    bool m_overwrite = false, m_pathExists = false;
    bool m_active = false, m_errorFlag = false;

    std::vector<uint8_t> m_dirlist;
    size_t m_dirlistPos = 0;
    bool m_dirlistEof = true;
    bool m_dirlistTimestamps = false, m_dirlistLong = false;
    void* m_dirlistDirp = nullptr;              // DIR*
    std::string m_dirlistWildcard;
    char m_dirlistTypeFilter = 0;

    Channel m_channels[16];
};

/** "8".."31" as a unit number; throws std::invalid_argument otherwise. */
int parseCbmUnit(const std::string& s);
