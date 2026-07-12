#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

class DebugContext;
class IBus;

/**
 * Represents a single entry in a stack frame layout.
 * Can be a variable, a gap, or a struct field.
 */
struct FrameLayoutEntry {
    enum class Type {
        VARIABLE,      // Named variable
        STRUCT_FIELD,  // Field within a struct variable
        GAP,           // Unallocated space between variables
    };

    enum class Status {
        UNKNOWN,
        UNINITIALIZED, // Has zero/garbage bytes, should have data
        WRONG_VALUE,   // Has data but wrong value
        INITIALIZED,   // Has correct/expected value
        NO_SOURCE,     // Can't verify - no source value available
    };

    Type type;
    Status status;
    std::string name;          // Variable or field name
    std::string displayName;   // Human-readable name
    std::string typeStr;       // Type string (int16, struct Point, etc.)
    uint32_t offset;           // Offset within frame
    uint32_t size;             // Size in bytes
    std::vector<uint8_t> value; // Actual bytes read from memory
    std::vector<uint8_t> expectedValue; // What it should be (if known)
    int level;                 // Nesting level (0 = top-level, 1+ = struct field)
    std::string comment;       // Additional notes/validation info

    FrameLayoutEntry()
        : type(Type::VARIABLE), status(Status::UNKNOWN), offset(0), size(0), level(0) {}
};

/**
 * Analyzes stack frame layout and detects initialization issues.
 */
class FrameLayoutAnalyzer {
public:
    /**
     * Analyze the current stack frame.
     * Returns a list of frame layout entries sorted by offset.
     */
    static std::vector<FrameLayoutEntry> analyzeCurrentFrame(
        DebugContext* dbg, IBus* bus, uint32_t framePointer, uint32_t frameSize);

    /**
     * Analyze a function's frame layout (based on symbol table).
     */
    static std::vector<FrameLayoutEntry> analyzeFrameForFunction(
        DebugContext* dbg, IBus* bus, const std::string& functionName,
        uint32_t framePointer, uint32_t frameSize);

    /**
     * Detect gaps (unallocated space) in frame layout.
     * Returns list of gap entries between variables.
     */
    static std::vector<FrameLayoutEntry> detectGaps(
        const std::vector<FrameLayoutEntry>& layout);

    /**
     * Check initialization status of variables.
     * Compares actual values against expected defaults.
     */
    static void checkInitializationStatus(
        std::vector<FrameLayoutEntry>& entries, IBus* bus);

    /**
     * Format frame layout for display.
     */
    static std::string formatFrameLayout(
        const std::vector<FrameLayoutEntry>& entries,
        uint32_t framePointer, uint32_t frameSize);

    /**
     * Format as pseudo-struct definition.
     */
    static std::string formatAsStructDefinition(
        const std::vector<FrameLayoutEntry>& entries);

private:
    static void analyzeVariableBranch(
        DebugContext* dbg, IBus* bus,
        std::vector<FrameLayoutEntry>& entries,
        uint32_t framePointer);

    static std::string getInitStatusString(FrameLayoutEntry::Status status);
    static std::string getInitStatusEmoji(FrameLayoutEntry::Status status);
};

/**
 * Frame context - represents a stack frame for inspection.
 */
struct FrameContext {
    std::string functionName;
    uint32_t framePointer;
    uint32_t stackPointer;
    uint32_t frameSize;
    std::vector<FrameLayoutEntry> layout;
    std::string pc;  // Program counter as hex string

    FrameContext() : framePointer(0), stackPointer(0), frameSize(0) {}
};
