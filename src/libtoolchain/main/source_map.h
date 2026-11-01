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

    /**
     * Load a ca45 `--source-map` file. Mapping lines look like
     *   00002000  Line 3  start:  lda #$01
     * and belong to `sourcePath` (ca45 does not name the file). A
     * `<file>:<line>` column instead of `Line N` is also accepted (file
     * relative to the map's directory). Symbols from the map's "Symbol Table"
     * section are appended to `symbols` (label, address) if given.
     * Paths are stored as absolute, normalised paths. Returns the number of
     * address mappings loaded (0 if the file could not be read or has none).
     */
    int loadCa45SourceMap(const std::string& mapPath, const std::string& sourcePath,
                          std::vector<std::pair<std::string, uint32_t>>* symbols = nullptr);

    /** Add one mapping (file is stored as given). */
    void addMapping(uint32_t addr, const std::string& file, int line);
    void clear();
    bool empty() const { return m_addrToSource.empty(); }

    /** Absolute, normalised form of a path, as stored by loadCa45SourceMap(). */
    static std::string normalizePath(const std::string& path);

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
