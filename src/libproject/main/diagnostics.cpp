#include "diagnostics.h"
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

namespace project {

static Diagnostic::Severity severityOf(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    if (s.find("warning") != std::string::npos) return Diagnostic::Severity::Warning;
    if (s.find("note") != std::string::npos)    return Diagnostic::Severity::Note;
    return Diagnostic::Severity::Error;
}

std::string DiagnosticParser::resolve(const std::string& file) const {
    if (file.empty()) return "";
    fs::path p(file);
    if (p.is_relative()) p = fs::path(m_dir) / p;
    std::error_code ec;
    auto norm = fs::weakly_canonical(p, ec);
    return ec ? p.string() : norm.string();
}

std::optional<Diagnostic> DiagnosticParser::feed(const std::string& line) {
    static const std::regex kEnter(R"(^make(?:\[\d+\])?: Entering directory ['`](.*)'$)");
    static const std::regex kGcc(R"((?:^|\s)([^\s:]*):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note)\s*:\s*(.*)$)",
                                 std::regex::icase);
    static const std::regex kCa65(R"(^(\S+)\((\d+)\):\s*(Error|Warning|Note)\s*:\s*(.*)$)");
    static const std::regex kAcme(R"(^(Serious error|Error|Warning) - File (.+?), line (\d+)[^:]*:\s*(.*)$)");
    static const std::regex kKickMsg(R"(^\s*(Error|Warning)\s*:\s*(.*)$)");
    static const std::regex kKickAt(R"(^\s*at line (\d+), column (\d+) in (.+)$)");

    std::smatch m;
    if (std::regex_search(line, m, kEnter)) { m_dir = m[1].str(); return std::nullopt; }

    if (!m_pendingKickAss.empty() && std::regex_search(line, m, kKickAt)) {
        Diagnostic d;
        std::smatch msg;
        std::regex_search(m_pendingKickAss, msg, kKickMsg);
        d.severity = severityOf(msg[1].str());
        d.message = msg[2].str();
        d.line = std::stoi(m[1].str());
        d.column = std::stoi(m[2].str());
        d.file = resolve(m[3].str());
        m_pendingKickAss.clear();
        return d;
    }
    m_pendingKickAss.clear();

    if (std::regex_search(line, m, kGcc)) {
        Diagnostic d;
        d.file = resolve(m[1].str());
        d.line = std::stoi(m[2].str());
        d.column = m[3].matched ? std::stoi(m[3].str()) : 0;
        d.severity = severityOf(m[4].str());
        d.message = m[5].str();
        return d;
    }
    if (std::regex_search(line, m, kCa65)) {
        Diagnostic d;
        d.file = resolve(m[1].str());
        d.line = std::stoi(m[2].str());
        d.severity = severityOf(m[3].str());
        d.message = m[4].str();
        return d;
    }
    if (std::regex_search(line, m, kAcme)) {
        Diagnostic d;
        d.severity = severityOf(m[1].str());
        d.file = resolve(m[2].str());
        d.line = std::stoi(m[3].str());
        d.message = m[4].str();
        return d;
    }
    if (std::regex_search(line, m, kKickMsg)) m_pendingKickAss = line;   // location follows
    return std::nullopt;
}

}  // namespace project
