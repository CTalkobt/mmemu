#include "debug_helpers.h"
#include "debug_context.h"
#include "libmem/main/ibus.h"
#include "libtoolchain/main/variable_symbol.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace DebugHelpers {

uint32_t getMaxAddress(IBus* bus) {
    if (!bus) return 0;
    return 1u << bus->config().addrBits;
}

LocalVariablesInfo getLocalVariablesInfo(DebugContext* dbg, IBus* bus) {
    LocalVariablesInfo result;

    if (!dbg || !bus) {
        result.hasVariables = false;
        return result;
    }

    auto globalVars = dbg->variables().getGlobalVariables();
    if (globalVars.empty()) {
        result.hasVariables = false;
        return result;
    }

    result.hasVariables = true;
    uint32_t maxAddr = getMaxAddress(bus);

    for (const auto* var : globalVars) {
        if (!var) continue;

        VariableInfo info;
        info.name = var->name;
        info.displayName = var->displayName.empty() ? var->name : var->displayName;
        info.type = formatVariableType(var->type);
        info.address = var->address;
        info.size = var->size;
        info.sourceLine = var->sourceLine;
        info.isParameter = var->isParameter;
        info.isFrameRelative = var->isFrameRelative;

        // Read variable value from bus
        if (var->address < maxAddr) {
            for (uint32_t i = 0; i < var->size && var->address + i < maxAddr; i++) {
                info.value.push_back(bus->peek8(var->address + i));
            }
        }

        result.variables.push_back(info);
    }

    // Sort by address for display
    std::sort(result.variables.begin(), result.variables.end(),
              [](const VariableInfo& a, const VariableInfo& b) {
                  return a.address < b.address;
              });

    return result;
}

FrameLayoutInfo getFrameLayoutInfo(DebugContext* dbg, IBus* bus) {
    FrameLayoutInfo result;

    if (!dbg || !bus) {
        return result;
    }

    auto globalVars = dbg->variables().getGlobalVariables();
    uint32_t maxAddr = getMaxAddress(bus);

    for (const auto* var : globalVars) {
        if (!var) continue;

        VariableInfo info;
        info.name = var->name;
        info.displayName = var->displayName.empty() ? var->name : var->displayName;
        info.type = formatVariableType(var->type);
        info.address = var->address;
        info.size = var->size;
        info.sourceLine = var->sourceLine;
        info.isParameter = var->isParameter;
        info.isFrameRelative = var->isFrameRelative;

        // Read variable value from bus
        if (var->address < maxAddr) {
            for (uint32_t i = 0; i < var->size && var->address + i < maxAddr; i++) {
                info.value.push_back(bus->peek8(var->address + i));
            }
        }

        result.variables.push_back(info);
        result.totalFrameSize = std::max(result.totalFrameSize, var->address + var->size);
    }

    // Sort by address for layout display
    std::sort(result.variables.begin(), result.variables.end(),
              [](const VariableInfo& a, const VariableInfo& b) {
                  return a.address < b.address;
              });

    return result;
}

VariableInfo getVariableInfo(DebugContext* dbg, IBus* bus,
                            const std::string& varName) {
    VariableInfo result;

    if (!dbg || !bus) {
        return result;
    }

    const VariableSymbol* var = dbg->variables().findGlobalVariable(varName);
    if (!var) {
        return result;  // Return empty/default VariableInfo
    }

    result.name = var->name;
    result.displayName = var->displayName.empty() ? var->name : var->displayName;
    result.type = formatVariableType(var->type);
    result.address = var->address;
    result.size = var->size;
    result.sourceLine = var->sourceLine;
    result.isParameter = var->isParameter;
    result.isFrameRelative = var->isFrameRelative;

    // Read variable value from bus
    uint32_t maxAddr = getMaxAddress(bus);
    if (var->address < maxAddr) {
        for (uint32_t i = 0; i < var->size && var->address + i < maxAddr; i++) {
            result.value.push_back(bus->peek8(var->address + i));
        }
    }

    return result;
}

std::string formatVariableValue(const VariableInfo& var) {
    if (var.value.empty() || var.address == 0 && var.size == 0) {
        return "[no value]";
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');

    // Format based on type and value size
    if (var.size == 1) {
        oss << "0x" << std::setw(2) << (int)var.value[0];
    } else if (var.size == 2 && var.value.size() >= 2) {
        uint16_t val = var.value[0] | (var.value[1] << 8);
        oss << "0x" << std::setw(4) << val;
    } else if (var.size == 4 && var.value.size() >= 4) {
        uint32_t val = var.value[0] |
                      (var.value[1] << 8) |
                      (var.value[2] << 16) |
                      (var.value[3] << 24);
        oss << "0x" << std::setw(8) << val;
    } else {
        // For other sizes, show hex dump
        oss << "[";
        for (size_t i = 0; i < var.value.size(); i++) {
            if (i > 0) oss << " ";
            oss << std::setw(2) << (int)var.value[i];
        }
        oss << "]";
    }

    return oss.str();
}

}  // namespace DebugHelpers
