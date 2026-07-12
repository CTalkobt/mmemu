#include "frame_analyzer.h"
#include "debug_context.h"
#include "libmem/main/ibus.h"
#include "libtoolchain/main/variable_symbol.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

static std::string variableTypeToString(VariableType type) {
    switch (type) {
        case VariableType::UNKNOWN: return "?";
        case VariableType::INT8: return "int8";
        case VariableType::INT16: return "int16";
        case VariableType::INT32: return "int32";
        case VariableType::UINT8: return "uint8";
        case VariableType::UINT16: return "uint16";
        case VariableType::UINT32: return "uint32";
        case VariableType::CHAR: return "char";
        case VariableType::POINTER: return "ptr";
        case VariableType::STRUCT: return "struct";
        case VariableType::ARRAY: return "array";
        default: return "?";
    }
}

std::vector<FrameLayoutEntry> FrameLayoutAnalyzer::analyzeCurrentFrame(
    DebugContext* dbg, IBus* bus, uint32_t framePointer, uint32_t frameSize) {

    if (!dbg || !bus) {
        return {};
    }

    std::vector<FrameLayoutEntry> entries;

    // Get all global variables (which represent the current frame)
    auto globalVars = dbg->variables().getGlobalVariables();

    for (const auto* var : globalVars) {
        if (!var) continue;

        FrameLayoutEntry entry;
        entry.type = FrameLayoutEntry::Type::VARIABLE;
        entry.name = var->name;
        entry.displayName = var->displayName.empty() ? var->name : var->displayName;
        entry.typeStr = variableTypeToString(var->type);
        entry.offset = var->address;
        entry.size = var->size;
        entry.level = 0;

        // Read actual value from memory
        uint32_t maxAddr = (1u << bus->config().addrBits);
        if (var->address < maxAddr) {
            for (uint32_t i = 0; i < var->size && var->address + i < maxAddr; i++) {
                entry.value.push_back(bus->peek8(var->address + i));
            }
        }

        entries.push_back(entry);
    }

    // Sort by offset
    std::sort(entries.begin(), entries.end(),
              [](const FrameLayoutEntry& a, const FrameLayoutEntry& b) {
                  return a.offset < b.offset;
              });

    // Detect gaps
    auto gaps = FrameLayoutAnalyzer::detectGaps(entries);
    entries.insert(entries.end(), gaps.begin(), gaps.end());

    // Re-sort with gaps
    std::sort(entries.begin(), entries.end(),
              [](const FrameLayoutEntry& a, const FrameLayoutEntry& b) {
                  return a.offset < b.offset;
              });

    // Check initialization status
    FrameLayoutAnalyzer::checkInitializationStatus(entries, bus);

    return entries;
}

std::vector<FrameLayoutEntry> FrameLayoutAnalyzer::analyzeFrameForFunction(
    DebugContext* dbg, IBus* bus, const std::string& functionName,
    uint32_t framePointer, uint32_t frameSize) {

    // For now, delegate to current frame analysis
    // Future: could filter to only variables for this function
    return analyzeCurrentFrame(dbg, bus, framePointer, frameSize);
}

std::vector<FrameLayoutEntry> FrameLayoutAnalyzer::detectGaps(
    const std::vector<FrameLayoutEntry>& layout) {

    std::vector<FrameLayoutEntry> gaps;

    if (layout.empty()) {
        return gaps;
    }

    // Find gaps between variables
    for (size_t i = 0; i < layout.size() - 1; i++) {
        uint32_t endOfCurrent = layout[i].offset + layout[i].size;
        uint32_t startOfNext = layout[i + 1].offset;

        if (endOfCurrent < startOfNext) {
            FrameLayoutEntry gap;
            gap.type = FrameLayoutEntry::Type::GAP;
            gap.name = "[UNALLOCATED]";
            gap.displayName = "[gap]";
            gap.typeStr = "-";
            gap.offset = endOfCurrent;
            gap.size = startOfNext - endOfCurrent;
            gap.status = FrameLayoutEntry::Status::UNKNOWN;

            gaps.push_back(gap);
        }
    }

    return gaps;
}

void FrameLayoutAnalyzer::checkInitializationStatus(
    std::vector<FrameLayoutEntry>& entries, IBus* bus) {

    for (auto& entry : entries) {
        if (entry.type == FrameLayoutEntry::Type::GAP) {
            // Gaps don't have a status
            continue;
        }

        if (entry.value.empty()) {
            entry.status = FrameLayoutEntry::Status::UNKNOWN;
            entry.comment = "Could not read value";
            continue;
        }

        // Check if all bytes are zero (uninitialized)
        bool allZero = true;
        for (uint8_t byte : entry.value) {
            if (byte != 0) {
                allZero = false;
                break;
            }
        }

        if (allZero) {
            entry.status = FrameLayoutEntry::Status::UNINITIALIZED;
            entry.comment = "All bytes zero - likely uninitialized";
        } else {
            // For now, mark as initialized if non-zero
            entry.status = FrameLayoutEntry::Status::INITIALIZED;
            entry.comment = "Initialized";
        }
    }
}

std::string FrameLayoutAnalyzer::formatFrameLayout(
    const std::vector<FrameLayoutEntry>& entries,
    uint32_t framePointer, uint32_t frameSize) {

    std::ostringstream oss;
    oss << "Stack Frame at FP=$" << std::hex << std::uppercase
        << std::setfill('0') << std::setw(2) << framePointer
        << " (Size: " << std::dec << frameSize << " bytes):\n\n";

    oss << std::left << std::setw(10) << "Offset"
        << std::setw(8) << "Size"
        << std::setw(20) << "Name"
        << std::setw(12) << "Type"
        << std::setw(12) << "Value"
        << std::setw(20) << "Status\n";

    oss << std::string(72, '-') << "\n";

    for (const auto& entry : entries) {
        // Offset
        oss << std::hex << std::uppercase << std::setfill('0')
            << std::setw(2) << (framePointer + entry.offset);
        oss << std::dec << std::setfill(' ');

        // Size
        oss << std::setw(8) << entry.size;

        // Name
        oss << std::setw(20) << entry.displayName.substr(0, 19);

        // Type
        oss << std::setw(12) << entry.typeStr.substr(0, 11);

        // Value (hex)
        oss << std::setw(12);
        if (entry.value.empty()) {
            oss << "??";
        } else if (entry.value.size() == 1) {
            oss << std::hex << std::setw(2) << (int)entry.value[0];
        } else {
            oss << std::hex;
            for (size_t i = 0; i < std::min(entry.value.size(), size_t(4)); i++) {
                oss << std::setfill('0') << std::setw(2) << (int)entry.value[i];
                if (i < 3) oss << " ";
            }
        }
        oss << std::dec << std::setfill(' ');

        // Status
        oss << std::setw(20) << getInitStatusString(entry.status);

        oss << "\n";
    }

    return oss.str();
}

std::string FrameLayoutAnalyzer::formatAsStructDefinition(
    const std::vector<FrameLayoutEntry>& entries) {

    std::ostringstream oss;
    oss << "struct _frame {\n";

    for (const auto& entry : entries) {
        if (entry.type == FrameLayoutEntry::Type::GAP) {
            oss << "  // +" << std::hex << entry.offset << " [gap " << std::dec
                << entry.size << " bytes]\n";
        } else {
            oss << "  " << std::setw(12) << entry.typeStr << " "
                << entry.displayName << " @ +" << std::hex << entry.offset
                << std::dec << ";  // ";

            // Show value in hex
            if (!entry.value.empty()) {
                oss << std::hex;
                for (uint8_t b : entry.value) {
                    oss << std::setfill('0') << std::setw(2) << (int)b << " ";
                }
                oss << std::dec;
            }

            // Show status
            oss << "[" << getInitStatusString(entry.status) << "]";
            oss << "\n";
        }
    }

    oss << "}\n";
    return oss.str();
}

std::string FrameLayoutAnalyzer::getInitStatusString(FrameLayoutEntry::Status status) {
    switch (status) {
        case FrameLayoutEntry::Status::UNKNOWN:
            return "UNKNOWN";
        case FrameLayoutEntry::Status::UNINITIALIZED:
            return "UNINITIALIZED";
        case FrameLayoutEntry::Status::WRONG_VALUE:
            return "WRONG";
        case FrameLayoutEntry::Status::INITIALIZED:
            return "OK";
        case FrameLayoutEntry::Status::NO_SOURCE:
            return "NO_SOURCE";
        default:
            return "?";
    }
}

std::string FrameLayoutAnalyzer::getInitStatusEmoji(FrameLayoutEntry::Status status) {
    switch (status) {
        case FrameLayoutEntry::Status::INITIALIZED:
            return "✓";
        case FrameLayoutEntry::Status::UNINITIALIZED:
            return "⚠";
        case FrameLayoutEntry::Status::WRONG_VALUE:
            return "✗";
        case FrameLayoutEntry::Status::NO_SOURCE:
            return "?";
        default:
            return " ";
    }
}
