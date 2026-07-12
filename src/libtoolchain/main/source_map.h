#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

struct SourceLocation {
    std::string file;
    int line;
};

class SourceMap {
public:
    bool loadKickAssList(const std::string& path);

    /**
     * Load source locations from assembly file with .loc directives.
     * Format: .loc "filename", line_number
     * Associates the next instruction's address with the source location.
     */
    bool loadAssemblyWithLoc(const std::string& path);

    SourceLocation addrToSource(uint32_t addr) const;
    uint32_t sourceToAddr(const std::string& file, int line) const;

    // Find nearest source location at or before the given address
    SourceLocation nearestSource(uint32_t addr) const;

    // Get all files in the map
    std::vector<std::string> getSourceFiles() const;

    // Get line range for a file
    std::pair<int, int> getLineRange(const std::string& file) const;

private:
    std::map<uint32_t, SourceLocation> m_addrToSource;
    std::map<std::string, std::map<int, uint32_t>> m_sourceToAddr;
};
