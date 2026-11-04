#pragma once

#include "libcore/main/intercept.h"
#include <string>
#include <cstdint>
#include <functional>

class MOS45GS02;
class IBus;

/**
 * HDOS (Hypervisor DOS) trap handler.
 *
 * Intercepts HYPPO DOS traps and services them from the host filesystem,
 * bypassing the need for full SD card + FAT32 emulation. Modeled after
 * xemu's hdos.c trap_for_xemu() approach.
 *
 * Intercept type "mega65.hdos" (libcore/main/intercept.h): machines/mega65.json
 * lists it with the DOS functions it serves ("trap": "$00", "functions":
 * [...]) and the host folders to use ("paths", the first that exists). It
 * reads the name a program passes to setname ($2E) and the transfer area
 * ($3A) from user memory and still lets HYPPO handle those two calls, so
 * keep them in "functions". Settings: `path` (host folder), `filename`
 * (overrides the setname name for findfile/openfile/loadfile; empty = off).
 *
 * Function numbers (already masked & 0x7E by caller):
 *   $00 = get version          $02 = get default drive
 *   $04 = get current drive    $06 = select drive
 *   $0C = chdir                $12 = opendir
 *   $14 = readdir              $16 = closedir
 *   $18 = openfile             $1A = readfile
 *   $20 = closefile            $22 = closeall
 *   $2E = setname              $34 = findfile
 *   $36 = loadfile             $3A = set transfer area
 *   $3C = cdrootdir
 *
 * Carry convention: SET = success, CLEAR = error.
 */
class HdosHandler : public ICallIntercept {
public:
    static constexpr int MAX_DESCRIPTORS = 4;

    // HDOS error codes (from xemu hdos.h)
    static constexpr uint8_t HDOSERR_FILE_NOT_FOUND  = 0x05;
    static constexpr uint8_t HDOSERR_INVALID_DESC    = 0x07;
    static constexpr uint8_t HDOSERR_TOO_MANY_OPEN   = 0x08;
    static constexpr uint8_t HDOSERR_READ_ERROR      = 0x09;
    static constexpr uint8_t HDOSERR_IS_DIRECTORY     = 0x0A;
    static constexpr uint8_t HDOSERR_NO_SUCH_DISK     = 0x80;
    static constexpr uint8_t HDOSERR_TOO_LONG         = 0x0B;
    static constexpr uint8_t HDOSERR_CANNOT_OPEN_DIR  = 0x0C;

    HdosHandler();
    /** From an intercept entry: "paths" (or "path") for the host folder. */
    explicit HdosHandler(const nlohmann::json& settings);
    ~HdosHandler() override;

    bool onCall(MachineDescriptor& m, ICore& cpu, uint32_t trap, uint32_t function) override;
    bool setOption(const std::string& key, const std::string& value) override;
    std::string describe() const override;
    const std::string& rootDir() const { return m_rootDir; }

    void setRootDir(const std::string& path);
    void setPhysBus(IBus* bus) { m_physBus = bus; }

    /// Called by HypervisorRegs when DOS trap fires in user mode.
    /// Returns true if the function was virtualized (caller should exit hypervisor).
    bool handleTrap(uint8_t func, MOS45GS02* cpu);

    /// setname ($2E): remember the name at user page Y (as HYPPO reads it).
    void captureSetname(MOS45GS02* cpu);

    void closeAll();

private:
    enum DescStatus { DESC_CLOSED, DESC_FILE, DESC_DIR };

    struct Descriptor {
        DescStatus status = DESC_CLOSED;
        int fd = -1;           // file descriptor (when DESC_FILE)
        void* dirp = nullptr;  // DIR* (when DESC_DIR)
        std::string basePath;
    };

    int allocateDescriptor();
    void closeDescriptor(int idx);

    // Virtualized HDOS functions
    void virtGetDrive(MOS45GS02* cpu);
    void virtSelectDrive(MOS45GS02* cpu);
    void virtFindFile(MOS45GS02* cpu);
    void virtOpenFile(MOS45GS02* cpu);
    void virtOpenDir(MOS45GS02* cpu);
    void virtReadFile(MOS45GS02* cpu);
    void virtReadDir(MOS45GS02* cpu);
    void virtCloseFileOrDir(MOS45GS02* cpu);
    void virtCloseAll(MOS45GS02* cpu);
    void virtLoadFile(MOS45GS02* cpu, uint32_t addrBase);
    void virtCd(MOS45GS02* cpu);
    void virtCdRoot(MOS45GS02* cpu);

    // Set result registers in hypervisor state
    void setResult(MOS45GS02* cpu, uint8_t a, bool carry);
    void setResultRegs(MOS45GS02* cpu, uint8_t a, uint8_t x, uint8_t y, uint8_t z, bool carry);

    std::string m_rootDir;
    std::string m_cwd;           // Current working directory (full host path, always ends with /)
    std::string m_setnameFn;     // Last filename set by setname
    std::string m_overrideName;  // `filename` setting: replaces m_setnameFn when set
    const std::string& requestedName() const { return m_overrideName.empty() ? m_setnameFn : m_overrideName; }
    std::string m_fileFound;     // Last file found by findfile
    IBus* m_physBus = nullptr;
    int m_currentDesc = -1;      // Last opened file descriptor
    int m_transferArea = 0;      // Transfer area address (set by $3A)
    uint8_t m_lastError = 0;

    Descriptor m_descriptors[MAX_DESCRIPTORS];
};

/** Register the "mega65.hdos" intercept type (idempotent). */
void registerHdosIntercept();

/** The settings machines/mega65.json had before intercepts were configurable. */
nlohmann::json defaultHdosInterceptSpec();
