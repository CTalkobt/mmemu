#include "cbm_image_dos.h"
#include "plugins/cbm-loader/main/d64_parser.h"
#include "plugins/cbm-loader/main/d71_parser.h"
#include "plugins/cbm-loader/main/d80_parser.h"
#include "plugins/cbm-loader/main/d81_parser.h"
#include "plugins/cbm-loader/main/d82_parser.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

CbmImageDos::CbmImageDos() { setError(0, " OK"); }
CbmImageDos::~CbmImageDos() = default;

bool CbmImageDos::open(const std::string& path) {
    std::string ext = path.size() > 4 ? path.substr(path.size() - 3) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::unique_ptr<CbmSectorDisk> d;
    if      (ext == "d64") d.reset(new D64Parser());
    else if (ext == "d71") d.reset(new D71Parser());
    else if (ext == "d80") d.reset(new D80Parser());
    else if (ext == "d81") d.reset(new D81Parser());
    else if (ext == "d82") d.reset(new D82Parser());
    else return false;
    if (!d->open(path)) return false;
    m_disk = std::move(d);
    m_path = path;
    reset();
    return true;
}

void CbmImageDos::reset() {
    for (auto& c : m_ch) c = Channel{};
    m_listening = m_talking = m_opening = m_active = false;
    m_name.clear();
    m_cmd.clear();
    setError(0, " OK");
}

void CbmImageDos::setError(int code, const char* text, int track, int sector) {
    // As the drives send them: "62,FILE NOT FOUND,00,00" ("00, OK,00,00")
    char buf[64];
    std::snprintf(buf, sizeof buf, "%02d,%s,%02d,%02d\r", code, text, track, sector);
    m_error = buf;
    m_errorPos = 0;
}

bool CbmImageDos::matches(const std::string& pattern, const std::string& name) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '*') return true;
        if (i >= name.size()) return false;
        if (pattern[i] != '?' && pattern[i] != name[i]) return false;
    }
    return pattern.size() == name.size();
}

// ---------------------------------------------------------------------------
// The directory
// ---------------------------------------------------------------------------

std::vector<uint8_t> CbmImageDos::directory(const std::string& pattern) {
    std::vector<uint8_t> out;
    uint16_t addr = 0x0401;                     // the PET's BASIC start, as the drives send it
    out.push_back((uint8_t)addr);
    out.push_back((uint8_t)(addr >> 8));
    // One BASIC line: link to the next, line number, text, 0
    auto line = [&](uint16_t number, const std::string& text) {
        uint16_t next = (uint16_t)(addr + 2 + 2 + text.size() + 1);
        out.push_back((uint8_t)next);
        out.push_back((uint8_t)(next >> 8));
        out.push_back((uint8_t)number);
        out.push_back((uint8_t)(number >> 8));
        out.insert(out.end(), text.begin(), text.end());
        out.push_back(0);
        addr = next;
    };
    std::string name = m_disk->getDiskName();
    name.resize(16, ' ');
    line(0, std::string("\x12\"") + name + "\" " + m_disk->getDiskId() + " " + m_disk->getDosType());

    static const char* types[] = {"DEL", "SEQ", "PRG", "USR", "REL"};
    for (const auto& e : m_disk->getDirectory()) {
        if (!pattern.empty() && !matches(pattern, e.filename)) continue;
        std::string text;
        if (e.sizeBlocks < 10) text += "   ";
        else if (e.sizeBlocks < 100) text += "  ";
        else if (e.sizeBlocks < 1000) text += " ";
        text += "\"" + e.filename + "\"";
        text.append(16 - std::min<size_t>(16, e.filename.size()), ' ');
        text += (e.type & 0x80) ? ' ' : '*';                     // not closed: a splat file
        int t = e.type & 0x07;
        text += t < 5 ? types[t] : "???";
        if (e.type & 0x40) text += '<';                          // locked
        line(e.sizeBlocks, text);
    }
    line(m_disk->getFreeBlocks(), "BLOCKS FREE.             ");
    out.push_back(0);
    out.push_back(0);
    return out;
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

void CbmImageDos::openChannel(int ch, const std::string& spec) {
    Channel& c = m_ch[ch];
    c = Channel{};
    if (ch == 15) {                             // OPEN 15,8,15,"command"
        if (!spec.empty()) command(spec);
        return;
    }
    // "0:NAME,S,R" -> drive prefix off, then the name and its options
    std::string s = spec;
    if (s.size() >= 2 && s[1] == ':' && std::isdigit((unsigned char)s[0])) s = s.substr(2);
    else if (!s.empty() && s[0] == ':') s = s.substr(1);
    if (!s.empty() && s[0] == '$') {            // "$", "$0", "$0:PAT*"
        std::string pat;
        size_t colon = s.find(':');
        if (colon != std::string::npos) pat = s.substr(colon + 1);
        c.data = directory(pat);
        c.open = true;
        setError(0, " OK");
        return;
    }
    std::string name = s, opts;
    size_t comma = s.find(',');
    if (comma != std::string::npos) { name = s.substr(0, comma); opts = s.substr(comma); }
    bool write = ch == 1 || opts.find(",W") != std::string::npos || opts.find(",A") != std::string::npos ||
                 (!name.empty() && name[0] == '@');
    if (write) {                                // SAVE (channel 1) or a write: read-only
        setError(26, "WRITE PROTECT ON");
        return;
    }
    for (const auto& e : m_disk->getDirectory()) {
        if (!matches(name, e.filename) || (e.type & 0x07) == 0) continue;
        if (!m_disk->readFile(e.filename, c.data)) break;
        c.open = true;
        setError(0, " OK");
        return;
    }
    setError(62, "FILE NOT FOUND");
}

void CbmImageDos::command(const std::string& raw) {
    std::string cmd = raw;
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();
    if (cmd.empty()) return;
    char c = cmd[0];
    if (c == 'I' || cmd.rfind("UJ", 0) == 0 || cmd.rfind("UI", 0) == 0 || cmd.rfind("U9", 0) == 0) {
        setError(0, " OK");                     // initialise / reset
    } else if (c == 'S' || c == 'R' || c == 'N' || c == 'C' || c == 'V' || cmd.rfind("B-W", 0) == 0 ||
               cmd.rfind("M-W", 0) == 0 || cmd.rfind("U2", 0) == 0) {
        setError(26, "WRITE PROTECT ON");       // would change the disk
    } else {
        setError(31, "SYNTAX ERROR");
    }
}

// ---------------------------------------------------------------------------
// The bus calls
// ---------------------------------------------------------------------------

int CbmImageDos::listen(uint8_t a) {
    if ((a & 0x1F) != m_unit) return -2;
    m_listening = true;
    m_active = true;
    return -1;
}

int CbmImageDos::talk(uint8_t a) {
    if ((a & 0x1F) != m_unit) return -2;
    m_talking = true;
    m_active = true;
    return -1;
}

int CbmImageDos::second(uint8_t a) {
    if (!m_listening) return -2;
    m_channel = a & 0x0F;
    m_opening = false;
    switch (a & 0xF0) {
        case 0xE0: m_ch[m_channel] = Channel{}; break;          // CLOSE
        case 0xF0: m_opening = true; m_name.clear(); break;     // OPEN: the name follows
        default: break;                                         // $6x: data
    }
    return -1;
}

int CbmImageDos::tksa(uint8_t a) {
    if (!m_talking) return -2;
    m_channel = a & 0x0F;
    return -1;
}

int CbmImageDos::ciout(uint8_t a) {
    if (!m_listening) return -2;
    if (m_opening) {
        if (m_name.size() < 80) m_name += (char)a;
    } else if (m_channel == 15) {
        if (a == '\r') { command(m_cmd); m_cmd.clear(); }
        else if (m_cmd.size() < 80) m_cmd += (char)a;
    } else {
        setError(26, "WRITE PROTECT ON");       // data written to a file
    }
    return 0;
}

int CbmImageDos::unlsn() {
    if (!m_listening) return -2;
    m_listening = false;
    m_active = false;
    if (m_opening) {
        m_opening = false;
        openChannel(m_channel, m_name);
    } else if (m_channel == 15 && !m_cmd.empty()) {
        command(m_cmd);
        m_cmd.clear();
    }
    return -1;
}

int CbmImageDos::acptr(uint8_t* a) {
    if (!m_talking) return -2;
    if (m_channel == 15) {
        *a = (uint8_t)m_error[m_errorPos++];
        if (m_errorPos >= m_error.size()) {
            setError(0, " OK");                 // read once, then OK
            return 0x40;
        }
        return 0;
    }
    Channel& c = m_ch[m_channel];
    if (!c.open || c.pos >= c.data.size()) { *a = 0; return 0x42; }
    *a = c.data[c.pos++];
    if (c.pos == c.data.size()) return 0x40;    // EOI with the last byte
    return 0;
}

int CbmImageDos::untlk() {
    if (!m_talking) return -2;
    m_talking = false;
    m_active = false;
    return -1;
}
