#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class ICore;
class IBus;
struct MachineDescriptor;

/**
 * Intercepts: host-side handlers that stand in for firmware routines, such as
 * the CBM KERNAL's LOAD served from a host folder, or MEGA65 HYPPO DOS calls.
 *
 * A machine's configuration installs them: an "intercepts" array on the
 * machine, or on one of its ROMs (so the intercept only fires while that ROM
 * is visible at the address). Each entry names a type that a plugin
 * registered with InterceptRegistry, plus that type's settings:
 *
 *   "intercepts": [
 *     { "type": "cbm.kernal.fileio", "name": "kernal-fileio",
 *       "load": "$FFD5", "save": "$FFD8", "path": "." }
 *   ]
 *
 * Common keys: "type" (required), "name" (default: the type), "enabled"
 * (default true), and for call intercepts "trap" and "functions" (which
 * firmware calls it handles; absent = all).
 */
class IIntercept {
public:
    virtual ~IIntercept() = default;

    const std::string& type() const { return m_type; }
    const std::string& name() const { return m_name; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }

    /** Change a setting at run time (CLI `intercept set`); false if unknown. */
    virtual bool setOption(const std::string& key, const std::string& value) {
        (void)key; (void)value; return false;
    }
    /** One line describing what it serves and its current settings. */
    virtual std::string describe() const { return ""; }

    /** Set by installIntercepts(). */
    void setIdentity(const std::string& type, const std::string& name) { m_type = type; m_name = name; }

private:
    std::string m_type, m_name;
    bool m_enabled = true;
};

/**
 * Runs instead of the instruction at one of its addresses: the CPU calls it
 * before fetching an opcode there.
 */
class IPcIntercept : public IIntercept {
public:
    /** 16-bit CPU addresses this intercept handles. */
    virtual std::vector<uint32_t> addresses() const = 0;
    /**
     * Handle the routine at `pc`. Return true when handled (registers and PC
     * set as the routine would leave them; the instruction is not executed),
     * false to let the CPU run it.
     */
    virtual bool onExecute(MachineDescriptor& m, ICore& cpu, IBus& bus, uint32_t pc) = 0;

    /**
     * Installed from a ROM entry: fire only while that ROM is visible at the
     * address (the CPU sees the ROM's byte there), not when it is banked out.
     */
    void setRom(const uint8_t* data, uint32_t cpuBase, uint32_t size) {
        m_romData = data; m_romBase = cpuBase; m_romSize = size;
    }
    bool romVisible(IBus& bus, uint32_t pc) const;

private:
    const uint8_t* m_romData = nullptr;
    uint32_t m_romBase = 0, m_romSize = 0;
};

/**
 * Runs before the firmware services a call (e.g. a MEGA65 hypervisor trap);
 * the machine decides what a call is and asks its call intercepts first.
 */
class ICallIntercept : public IIntercept {
public:
    /** Return true when handled: the firmware does not see the call. */
    virtual bool onCall(MachineDescriptor& m, ICore& cpu, uint32_t trap, uint32_t function) = 0;

    /** The "trap" and "functions" selection from the configuration. */
    bool selects(uint32_t trap, uint32_t function) const;
    void setSelection(int trap, std::vector<uint32_t> functions) {
        m_trap = trap; m_functions = std::move(functions);
    }

private:
    int m_trap = -1;                       // -1: any trap
    std::vector<uint32_t> m_functions;     // empty: any function
};

/** Intercept types, registered by plugins by name. */
class InterceptRegistry {
public:
    using Factory = std::function<IIntercept*(const nlohmann::json& settings)>;

    static InterceptRegistry& instance();
    void registerType(const std::string& type, Factory factory);
    bool hasType(const std::string& type) const { return m_factories.count(type) != 0; }
    /** A new intercept, or nullptr if the type is unknown or the settings are invalid. */
    IIntercept* create(const std::string& type, const nlohmann::json& settings) const;
    std::vector<std::string> types() const;
    /** Forget all types (before plugins are unloaded: the factories live in them). */
    void clear() { m_factories.clear(); }

private:
    std::map<std::string, Factory> m_factories;
};

/**
 * The CPU-side table of PC intercepts for one machine: a bitmap of armed
 * addresses (cheap to test before every instruction) and the intercepts at
 * each address.
 */
class PcInterceptHook {
public:
    explicit PcInterceptHook(MachineDescriptor* machine) : m_machine(machine) {}
    void add(IPcIntercept* intercept);
    bool armed(uint32_t pc) const { pc &= 0xFFFF; return (m_map[pc >> 3] >> (pc & 7)) & 1; }
    /** Run the enabled intercepts at `pc`; true if one handled it. */
    bool fire(ICore* cpu, uint32_t pc);

private:
    MachineDescriptor* m_machine;
    uint8_t m_map[8192] = {};
    std::multimap<uint32_t, IPcIntercept*> m_at;
};

/**
 * Install the intercepts a machine specification lists: spec["intercepts"]
 * and spec["roms"][i]["intercepts"] (a ROM entry with intercepts needs
 * "cpuAddr", the CPU address of its first byte, or an "overlayAddr"). `romData` returns a ROM's
 * loaded image by label. Unknown types and bad settings are reported in
 * m.warnings. PC intercepts are attached to the machine's first CPU.
 */
void installIntercepts(MachineDescriptor& m, const nlohmann::json& spec,
                       const std::function<const uint8_t*(const std::string& label)>& romData = {});

/** Run the machine's call intercepts for (trap, function); true if one handled it. */
bool dispatchCallIntercepts(MachineDescriptor& m, ICore& cpu, uint32_t trap, uint32_t function);

/** Find an installed intercept by name (or type). */
IIntercept* findIntercept(MachineDescriptor& m, const std::string& name);

/** Parse "$FFD5", "0xFFD5" or a number. */
uint32_t interceptAddr(const nlohmann::json& v);
