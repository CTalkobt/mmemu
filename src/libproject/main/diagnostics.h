#pragma once

#include <optional>
#include <string>

namespace project {

struct Diagnostic {
    enum class Severity { Error, Warning, Note };
    Severity    severity = Severity::Error;
    std::string file;      // absolute path, or "" if the tool did not name one
    int         line = 0;
    int         column = 0;
    std::string message;
};

/**
 * Turns build output lines into diagnostics. Recognises:
 *   gcc / ca45 / 64tass / cc45:  file:line[:col]: error|warning|note: message
 *   ca65:                         file(line): Error|Warning: message
 *   acme:                         Error - File file, line N (...): message
 *   KickAssembler (two lines):    Error: message / at line N, column C in file
 * Relative file names are resolved against the directory make is in (it
 * follows "make: Entering directory '...'" lines).
 */
class DiagnosticParser {
public:
    explicit DiagnosticParser(std::string baseDir) : m_dir(std::move(baseDir)) {}
    std::optional<Diagnostic> feed(const std::string& line);

private:
    std::string resolve(const std::string& file) const;
    std::string m_dir;
    std::string m_pendingKickAss;   // "Error: ..." line waiting for its location line
};

}  // namespace project
