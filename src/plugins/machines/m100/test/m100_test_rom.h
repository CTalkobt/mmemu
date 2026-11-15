#pragma once

// 8085 stand-ins for the Model 100/102/200 ROMs (the real ones are
// copyright and not in the repository), assembled with the 8085 plugin's
// line assembler: just enough to drive the hardware as the ROMs do.

#include "plugins/machines/m100/main/machine_m100.h"
#include "plugins/machines/m100/main/m100_system.h"
#include "plugins/8085/main/asm8085.h"
#include "libcore/main/machine_desc.h"
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace m100test {

/**
 * Assemble `src` into `image` (which starts at address `base`): one
 * instruction a line, `label:` before it, `ORG addr` (hex) to move. Labels
 * (two or more letters, not SP or PSW) are substituted as $XXXX in the operands (two passes; an 8085 instruction's
 * length does not depend on its operand's value).
 */
inline std::map<std::string, uint16_t> assemble(std::vector<uint8_t>& image, uint32_t base, const std::string& src) {
    Assembler8085 as;
    std::map<std::string, uint16_t> labels;
    auto subst = [&](const std::string& ops, bool final) {
        std::string out, word;
        auto flush = [&]() {
            if (word.empty()) return;
            std::string lw = word;
            for (auto& c : lw) c = (char)std::tolower((unsigned char)c);
            auto it = labels.find(lw);
            if (it != labels.end() && !std::isdigit((unsigned char)word[0])) {
                char b[8];
                std::snprintf(b, sizeof b, "$%04X", it->second);
                out += b;
            } else if (!final && lw.size() > 1 && std::isalpha((unsigned char)lw[0]) && lw != "sp" && lw != "psw") {
                out += "$0000";                 // a forward label (labels are longer than a register name)
            } else {
                out += word;
            }
            word.clear();
        };
        for (char c : ops) {
            if (std::isalnum((unsigned char)c) || c == '_' || c == '$') word += c;
            else { flush(); out += c; }
        }
        flush();
        return out;
    };
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t pc = base;
        std::istringstream in(src);
        std::string line;
        while (std::getline(in, line)) {
            const auto semi = line.find(';');
            if (semi != std::string::npos) line = line.substr(0, semi);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string l = line.substr(0, colon);
                l.erase(0, l.find_first_not_of(" \t"));
                for (auto& c : l) c = (char)std::tolower((unsigned char)c);
                labels[l] = (uint16_t)pc;
                line = line.substr(colon + 1);
            }
            const auto s = line.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            line = line.substr(s);
            if (line.size() > 4 && (line.compare(0, 4, "ORG ") == 0 || line.compare(0, 4, "org ") == 0)) {
                pc = (uint32_t)std::stoul(line.substr(4 + line.substr(4).find_first_not_of(" $")), nullptr, 16);
                continue;
            }
            const auto sp = line.find_first_of(" \t");
            std::string text = sp == std::string::npos ? line : line.substr(0, sp) + " " + subst(line.substr(sp), pass == 1);
            uint8_t buf[16];
            const int n = as.assembleLine(text, buf, sizeof buf, pc);
            if (n < 0) throw std::runtime_error("8085: can't assemble '" + text + "'");
            for (int i = 0; i < n; ++i) {
                const uint32_t at = pc + (uint32_t)i - base;
                if (pass == 1 && at < image.size()) image[at] = buf[i];
            }
            pc += (uint32_t)n;
        }
    }
    return labels;
}

/** A ROM image of `size` bytes ($FF) with `src` assembled at 0. */
inline std::vector<uint8_t> romFrom(const std::string& src, size_t size = 0x8000) {
    std::vector<uint8_t> r(size, 0xFF);
    assemble(r, 0, src);
    return r;
}

/** A stand-in that does nothing: DI and HLT. */
inline std::vector<uint8_t> idleRom(m100::Model model) {
    return romFrom("DI\nHLT\n", model == m100::Model::M200 ? 0x12000 : 0x8000);
}

inline MachineDescriptor* make(m100::Model model, const std::vector<uint8_t>& rom, int ramKb = 0,
                               const std::vector<uint8_t>& opt = {}) {
    m100::Roms roms;
    roms.rom = rom;
    roms.optRom = opt;
    std::string err;
    return m100::create(model, &err, &roms, ramKb);
}

/** Run for `cycles` CPU clocks. */
inline uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}

inline void runFor(MachineDescriptor* m, double seconds) { run(m, (uint64_t)(seconds * m->clockHz)); }

/** Assemble `src` into RAM through the bus and start the CPU there. */
inline void runInRam(MachineDescriptor* m, uint16_t org, const std::string& src) {
    std::vector<uint8_t> img(0x10000 - org);
    for (size_t i = 0; i < img.size(); ++i) img[i] = m->buses[0].bus->peek8((uint32_t)(org + i));
    assemble(img, org, src);
    for (size_t i = 0; i < img.size(); ++i) m->buses[0].bus->write8((uint32_t)(org + i), img[i]);
    m->cpus[0].cpu->setPc(org);
}

}  // namespace m100test
