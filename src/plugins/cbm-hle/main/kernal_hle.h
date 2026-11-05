#pragma once
#include <functional>

#include "libcore/main/intercept.h"
#include <cstdint>
#include <string>

class ICore;
class IBus;

/**
 * CBM KERNAL LOAD/SAVE served from a host folder (intercept type
 * "cbm.kernal.fileio"). A machine's configuration installs it, normally on
 * its KERNAL ROM entry, so it fires only while that ROM is mapped:
 *
 *   { "type": "cbm.kernal.fileio", "load": "$FFD5", "save": "$FFD8",
 *     "path": ".",
 *     "zeroPage": { "status": "$90", "fnlen": "$B7", "sa": "$B9",
 *                   "fa": "$BA", "fnadr": "$BB" } }
 *
 * The addresses default to the C64 KERNAL's. A unit that an emulated drive
 * answers (an IOHandler's getDiskStatus) is left to the KERNAL. Settings:
 * `path` (host folder). SAVE writes the file there (start address first);
 * an existing file is replaced only with an "@:" prefix, as on CBM DOS.
 */
class KernalHLE : public IPcIntercept {
public:
    struct ZeroPage {
        uint16_t status = 0x90, fnlen = 0xB7, sa = 0xB9, fa = 0xBA, fnadr = 0xBB;
    };

    KernalHLE() = default;
    /** From an intercept entry's settings (throws on a bad address). */
    explicit KernalHLE(const nlohmann::json& settings);

    std::vector<uint32_t> addresses() const override { return { m_load, m_save }; }
    bool onExecute(MachineDescriptor& m, ICore& cpu, IBus& bus, uint32_t pc) override;
    bool setOption(const std::string& key, const std::string& value) override;
    std::string describe() const override;

    void setHostPath(const std::string& path) { m_hostPath = path; }
    const std::string& getHostPath() const { return m_hostPath; }

    /** Serve LOAD/SAVE without a machine (tests); returns true if handled. */
    bool handle(ICore& cpu, IBus& bus, uint32_t pc, MachineDescriptor* m = nullptr);

private:
    uint32_t    m_load = 0xFFD5, m_save = 0xFFD8;
    ZeroPage    m_zp;
    std::string m_hostPath = ".";

    void handleLoad(ICore* cpu, IBus* bus);
    void handleSave(ICore* cpu, IBus* bus);
    uint8_t getDevice(IBus* bus);
    uint8_t getSecondaryAddress(IBus* bus);
    std::string getFilename(IBus* bus);
    /** A byte the KERNAL would fetch from RAM (MachineDescriptor::readRam, else the bus). */
    uint8_t ramByte(IBus* bus, uint16_t addr) const;
    std::function<uint8_t(uint16_t)> m_readRam;     // set from the machine at each call
    void setStatus(IBus* bus, uint8_t status);
    void setCarry(ICore* cpu, bool set);
};

/** Register the "cbm.kernal.fileio" intercept type. */
void registerKernalHleIntercept();
