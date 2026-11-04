#include "disassembler_6502.h"
#include <cstdio>
#include <iomanip>
#include <sstream>

Disassembler6502::Disassembler6502(SymbolTable* symbols, bool cmos)
    : m_cpu(cmos ? std::unique_ptr<MOS6502>(new MOS65C02()) : std::make_unique<MOS6502>()),
      m_symbols(symbols)
{
}

int Disassembler6502::disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    DisasmEntry entry;
    int bytes = disasmEntry(bus, addr, entry);
    std::snprintf(buf, bufsz, "%s", entry.complete.c_str());
    return bytes;
}

int Disassembler6502::disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) {
    int bytes = m_cpu->disassembleEntry(bus, addr, &entry);
    if (m_symbols) {
        resolveSymbols(entry);
    }
    return bytes;
}

void Disassembler6502::resolveSymbols(DisasmEntry& entry) {
    if (entry.targetAddr == 0 && entry.operands.find('$') == std::string::npos) return;

    // Very basic symbol resolution: replace the first hex address in operands
    // with a label if it exists.
    
    // For 6502, targets are often in operands as $nnnn or $nn.
    size_t dolPos = entry.operands.find('$');
    if (dolPos != std::string::npos) {
        std::string hexStr = entry.operands.substr(dolPos + 1);
        // Take only digits
        size_t end = 0;
        while (end < hexStr.size() && std::isxdigit(hexStr[end])) end++;
        hexStr = hexStr.substr(0, end);

        uint32_t val;
        std::stringstream ss;
        ss << std::hex << hexStr;
        ss >> val;

        std::string label = m_symbols->getLabel(val);
        if (!label.empty()) {
            entry.operands.replace(dolPos, end + 1, label);
            
            // Reconstruct complete string
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%-4s %s", entry.mnemonic.c_str(), entry.operands.c_str());
            entry.complete = buf;
        }
    }
}
