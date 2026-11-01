#include "source_map.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <climits>
#include <algorithm>
#include <filesystem>

bool SourceMap::loadKickAssList(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    // Basic regex to match something like "[1000] 4c 00 10  1  main: jmp main"
    // This is very simplified.
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] != '[') continue;

        size_t endBracket = line.find(']');
        if (endBracket == std::string::npos) continue;

        std::string addrStr = line.substr(1, endBracket - 1);
        uint32_t addr;
        std::stringstream ss;
        ss << std::hex << addrStr;
        ss >> addr;

        // Find the line number. In KickAss list files, it's often after the bytes.
        // For now, let's just store the address.
        // Full parser would be more complex.
        
        m_addrToSource[addr] = {path, 0}; // Line 0 as placeholder
    }

    return true;
}

std::string SourceMap::normalizePath(const std::string& path) {
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
    return ec ? path : p.string();
}

void SourceMap::addMapping(uint32_t addr, const std::string& file, int line) {
    m_addrToSource[addr] = {file, line};
    // Keep the lowest address for a line (its first instruction)
    auto& lines = m_sourceToAddr[file];
    auto it = lines.find(line);
    if (it == lines.end() || addr < it->second) lines[line] = addr;
}

void SourceMap::clear() {
    m_addrToSource.clear();
    m_sourceToAddr.clear();
}

int SourceMap::loadCa45SourceMap(const std::string& mapPath, const std::string& sourcePath,
                                 std::vector<std::pair<std::string, uint32_t>>* symbols) {
    std::ifstream f(mapPath);
    if (!f.is_open()) return 0;
    const std::string source = normalizePath(sourcePath);
    const auto mapDir = std::filesystem::path(mapPath).parent_path();

    // "00002000  Line 3  ..." or "00002000  sub.s:12  ..."
    static const std::regex kLineRe(R"(^\s*([0-9A-Fa-f]{4,8})\s+Line\s+(\d+)\b)");
    static const std::regex kFileRe(R"(^\s*([0-9A-Fa-f]{4,8})\s+(\S+):(\d+)\b)");
    static const std::regex kSymRe(R"(^(\S+)\s+([0-9A-Fa-f]{4,8})\b)");

    int count = 0;
    bool inSymbols = false;
    std::string line;
    std::smatch m;
    while (std::getline(f, line)) {
        if (line.rfind("Symbol Table", 0) == 0) { inSymbols = true; continue; }
        if (inSymbols) {
            if (symbols && line.find_first_not_of("=") != std::string::npos &&
                std::regex_search(line, m, kSymRe))
                symbols->push_back({m[1].str(), (uint32_t)std::stoul(m[2].str(), nullptr, 16)});
            continue;
        }
        if (std::regex_search(line, m, kLineRe)) {
            addMapping((uint32_t)std::stoul(m[1].str(), nullptr, 16), source, std::stoi(m[2].str()));
            ++count;
        } else if (std::regex_search(line, m, kFileRe)) {
            std::filesystem::path file = m[2].str();
            if (file.is_relative()) file = mapDir / file;
            addMapping((uint32_t)std::stoul(m[1].str(), nullptr, 16), normalizePath(file.string()),
                       std::stoi(m[3].str()));
            ++count;
        }
    }
    return count;
}

SourceLocation SourceMap::addrToSource(uint32_t addr) const {
    auto it = m_addrToSource.find(addr);
    if (it != m_addrToSource.end()) return it->second;
    return {"", -1};
}

uint32_t SourceMap::sourceToAddr(const std::string& file, int line) const {
    auto itFile = m_sourceToAddr.find(file);
    if (itFile != m_sourceToAddr.end()) {
        auto itLine = itFile->second.find(line);
        if (itLine != itFile->second.end()) return itLine->second;
    }
    return 0xFFFFFFFF;
}

bool SourceMap::loadAssemblyWithLoc(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    std::string currentFile;
    int currentLine = -1;
    uint32_t currentAddr = 0;
    bool pendingLoc = false;

    while (std::getline(f, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == ';') continue;

        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Parse .loc directive: .loc "filename", line_number
        if (line.find(".loc ") == 0) {
            size_t quoteStart = line.find('"');
            size_t quoteEnd = line.find('"', quoteStart + 1);

            if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
                currentFile = line.substr(quoteStart + 1, quoteEnd - quoteStart - 1);

                // Find line number after comma
                size_t commaPos = line.find(',', quoteEnd);
                if (commaPos != std::string::npos) {
                    std::stringstream ss;
                    ss << line.substr(commaPos + 1);
                    ss >> currentLine;
                    pendingLoc = true;  // Wait for next instruction to assign address
                }
            }
            continue;
        }

        // Skip other directives (labels, constants, etc.)
        if (line[0] == '.') continue;
        if (line[0] == '@') continue;  // Local variable declarations

        // This is an instruction line
        // For now, we estimate addresses by counting instructions
        // A more sophisticated parser would parse actual bytecode lengths

        if (pendingLoc && currentLine >= 0 && !currentFile.empty()) {
            // Associate this address with the pending source location
            m_addrToSource[currentAddr] = {currentFile, currentLine};
            m_sourceToAddr[currentFile][currentLine] = currentAddr;
            pendingLoc = false;
        }

        // Increment address (assuming 1-3 bytes per instruction on 6502/45GS02)
        // This is an approximation; real implementation would parse instruction lengths
        currentAddr++;
    }

    return !m_addrToSource.empty();
}

SourceLocation SourceMap::nearestSource(uint32_t addr) const {
    auto it = m_addrToSource.lower_bound(addr);

    if (it != m_addrToSource.end() && it->first == addr) {
        return it->second;
    }

    if (it != m_addrToSource.begin()) {
        --it;
        return it->second;
    }

    return {"", -1};
}

std::vector<std::string> SourceMap::getSourceFiles() const {
    std::vector<std::string> files;
    for (const auto& pair : m_sourceToAddr) {
        files.push_back(pair.first);
    }
    return files;
}

std::pair<int, int> SourceMap::getLineRange(const std::string& file) const {
    auto it = m_sourceToAddr.find(file);
    if (it == m_sourceToAddr.end()) {
        return {-1, -1};
    }

    int minLine = INT_MAX;
    int maxLine = INT_MIN;

    for (const auto& linePair : it->second) {
        minLine = std::min(minLine, linePair.first);
        maxLine = std::max(maxLine, linePair.first);
    }

    if (minLine == INT_MAX) {
        return {-1, -1};
    }

    return {minLine, maxLine};
}
