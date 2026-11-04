#include "intercept.h"
#include "machine_desc.h"
#include "icore.h"
#include "libmem/main/ibus.h"
#include "util/logging.h"
#include <algorithm>

using json = nlohmann::json;

uint32_t interceptAddr(const json& v) {
    if (v.is_number_unsigned() || v.is_number_integer()) return v.get<uint32_t>();
    if (!v.is_string()) throw std::invalid_argument("address must be a number or a string");
    std::string s = v.get<std::string>();
    int base = 10;
    if (!s.empty() && s[0] == '$') { s = s.substr(1); base = 16; }
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s = s.substr(2); base = 16; }
    size_t used = 0;
    unsigned long n = std::stoul(s, &used, base);
    if (used != s.size()) throw std::invalid_argument("bad address \"" + v.get<std::string>() + "\"");
    return (uint32_t)n;
}

bool IPcIntercept::romVisible(IBus& bus, uint32_t pc) const {
    if (!m_romData) return true;
    if (pc < m_romBase || pc >= m_romBase + m_romSize) return false;
    // The ROM is visible if the CPU reads its bytes at this address
    for (uint32_t a = pc; a < pc + 3 && a < m_romBase + m_romSize; a++)
        if (bus.peek8(a) != m_romData[a - m_romBase]) return false;
    return true;
}

bool ICallIntercept::selects(uint32_t trap, uint32_t function) const {
    if (m_trap >= 0 && (uint32_t)m_trap != trap) return false;
    if (m_functions.empty()) return true;
    return std::find(m_functions.begin(), m_functions.end(), function) != m_functions.end();
}

InterceptRegistry& InterceptRegistry::instance() {
    static InterceptRegistry reg;
    return reg;
}

void InterceptRegistry::registerType(const std::string& type, Factory factory) {
    m_factories[type] = std::move(factory);
}

IIntercept* InterceptRegistry::create(const std::string& type, const json& settings) const {
    auto it = m_factories.find(type);
    return it == m_factories.end() ? nullptr : it->second(settings);
}

std::vector<std::string> InterceptRegistry::types() const {
    std::vector<std::string> out;
    for (const auto& [t, f] : m_factories) out.push_back(t);
    return out;
}

void PcInterceptHook::add(IPcIntercept* intercept) {
    for (uint32_t a : intercept->addresses()) {
        a &= 0xFFFF;
        m_map[a >> 3] |= (uint8_t)(1u << (a & 7));
        m_at.insert({a, intercept});
    }
}

bool PcInterceptHook::fire(ICore* cpu, uint32_t pc) {
    pc &= 0xFFFF;
    IBus* bus = cpu->getDataBus();
    if (!bus) return false;
    auto range = m_at.equal_range(pc);
    for (auto it = range.first; it != range.second; ++it) {
        IPcIntercept* i = it->second;
        if (!i->enabled() || !i->romVisible(*bus, pc)) continue;
        if (i->onExecute(*m_machine, *cpu, *bus, pc)) return true;
    }
    return false;
}

namespace {

void warn(MachineDescriptor& m, const std::string& msg) {
    LogRegistry::instance().getLogger("intercept")->warn("{}", msg);
    m.warnings.push_back(msg);
}

IIntercept* makeOne(MachineDescriptor& m, const json& e, const std::string& where) {
    if (!e.is_object() || !e.contains("type") || !e["type"].is_string()) {
        warn(m, "Intercept in " + where + " has no \"type\"; ignored.");
        return nullptr;
    }
    std::string type = e["type"].get<std::string>();
    auto& reg = InterceptRegistry::instance();
    if (!reg.hasType(type)) {
        warn(m, "Intercept type \"" + type + "\" (" + where + ") is not provided by any loaded plugin; ignored.");
        return nullptr;
    }
    IIntercept* i = nullptr;
    try {
        i = reg.create(type, e);
    } catch (const std::exception& ex) {
        warn(m, "Intercept \"" + type + "\" (" + where + "): " + ex.what() + "; ignored.");
        return nullptr;
    }
    if (!i) {
        warn(m, "Intercept \"" + type + "\" (" + where + ") has invalid settings; ignored.");
        return nullptr;
    }
    i->setIdentity(type, e.value("name", type));
    i->setEnabled(e.value("enabled", true));
    if (auto* c = dynamic_cast<ICallIntercept*>(i)) {
        int trap = e.contains("trap") ? (int)interceptAddr(e["trap"]) : -1;
        std::vector<uint32_t> fns;
        if (e.contains("functions"))
            for (const auto& f : e["functions"]) fns.push_back(interceptAddr(f));
        c->setSelection(trap, std::move(fns));
    }
    return i;
}

} // namespace

void installIntercepts(MachineDescriptor& m, const json& spec,
                       const std::function<const uint8_t*(const std::string&)>& romData) {
    std::vector<IIntercept*> made;
    if (spec.contains("intercepts") && spec["intercepts"].is_array())
        for (const auto& e : spec["intercepts"])
            if (IIntercept* i = makeOne(m, e, "machine " + m.machineId)) made.push_back(i);

    if (spec.contains("roms") && spec["roms"].is_array()) {
        for (const auto& r : spec["roms"]) {
            if (!r.contains("intercepts") || !r["intercepts"].is_array()) continue;
            std::string label = r.value("label", "");
            std::string where = "ROM " + label;
            const uint8_t* data = romData ? romData(label) : nullptr;
            // The CPU address of the ROM: "cpuAddr", else its "overlayAddr"
            json addr = r.contains("cpuAddr") ? r["cpuAddr"] : r.value("overlayAddr", json());
            if (!data || addr.is_null()) {
                warn(m, "Intercepts on " + where + " need the ROM loaded and its \"cpuAddr\"; ignored.");
                continue;
            }
            uint32_t base = interceptAddr(addr);
            uint32_t size = r.value("size", 0u);
            for (const auto& e : r["intercepts"]) {
                IIntercept* i = makeOne(m, e, where);
                if (!i) continue;
                if (auto* p = dynamic_cast<IPcIntercept*>(i)) p->setRom(data, base, size);
                made.push_back(i);
            }
        }
    }

    PcInterceptHook* hook = nullptr;
    for (IIntercept* i : made) {
        m.intercepts.push_back(i);
        m.deleters.push_back([i]() { delete i; });
        if (auto* p = dynamic_cast<IPcIntercept*>(i)) {
            if (!hook) hook = new PcInterceptHook(&m);
            hook->add(p);
        }
    }
    if (hook) {
        if (!m.cpus.empty() && m.cpus[0].cpu) m.cpus[0].cpu->setPcInterceptHook(hook);
        m.deleters.push_back([hook]() { delete hook; });
    }
}

bool dispatchCallIntercepts(MachineDescriptor& m, ICore& cpu, uint32_t trap, uint32_t function) {
    for (IIntercept* i : m.intercepts) {
        auto* c = dynamic_cast<ICallIntercept*>(i);
        if (c && c->enabled() && c->selects(trap, function) && c->onCall(m, cpu, trap, function))
            return true;
    }
    return false;
}

IIntercept* findIntercept(MachineDescriptor& m, const std::string& name) {
    for (IIntercept* i : m.intercepts) if (i->name() == name) return i;
    for (IIntercept* i : m.intercepts) if (i->type() == name) return i;
    return nullptr;
}
