// The DriveWire 4 server (#356): see drivewire_server.h.

#include "drivewire_server.h"
#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

struct DriveWireServer::Drive {
    std::string path;
    Mode mode = Mode::WriteThrough;
    std::unique_ptr<std::fstream> file;
    uint64_t size = 0;                                      // bytes in the file
    std::map<uint32_t, std::array<uint8_t, 256>> overlay;   // copy-on-write sectors
};

// A virtual channel: the guest's side (its driver has it open; a path has it
// open; a hang-up to tell it), the bytes waiting for it, what it wrote, and
// the host side (an endpoint, the virtual modem, or neither)
struct DriveWireServer::Channel {
    bool open = false, pathOpen = false;
    bool closing = false;                   // tell the guest ($10, port) once `in` is read
    double sinceRead = 1e9;                 // seconds since the guest last took a byte
    std::deque<uint8_t> in;                 // host -> guest
    std::string out;                        // guest -> host (the last MAX_OUTPUT bytes)
    std::vector<uint8_t> comSt;             // the last SS.ComSt's PD.OPT
    std::unique_ptr<SerialEndpoint> endpoint;
    std::string spec = "none";
    bool cooked = true, connected = false;
    bool lastCr = false;                    // cooked input: LF after CR dropped
    int iac = 0;                            // cooked input: telnet IAC parser state
    // The virtual modem (serial ports 1-14 with no endpoint)
    bool modem = false;
    bool online = false;                    // `tcp connect`: `endpoint` is the connection
    std::string line;
};

namespace {

// `tcp connect HOST PORT`'s connection: a client socket as a serial endpoint
class TcpClientEndpoint : public SerialEndpoint {
public:
    ~TcpClientEndpoint() override {
        if (m_fd >= 0) ::close(m_fd);
    }
    bool connect(const std::string& host, int port, std::string& err) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        if (rc != 0) { err = gai_strerror(rc); return false; }
        err = "connection refused";
        for (addrinfo* a = res; a; a = a->ai_next) {
            const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            ::fcntl(fd, F_SETFL, O_NONBLOCK);
            int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
            if (r < 0 && errno == EINPROGRESS) {
                pollfd p{fd, POLLOUT, 0};
                if (::poll(&p, 1, 3000) == 1) {             // (the emulation waits: at most 3 s)
                    int e = 0;
                    socklen_t l = sizeof e;
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
                    r = e == 0 ? 0 : -1;
                    if (e) err = std::strerror(e);
                } else {
                    err = "timed out";
                }
            } else if (r < 0) {
                err = std::strerror(errno);
            }
            if (r == 0) {
                m_fd = fd;
                m_name = host + ":" + std::to_string(port);
                break;
            }
            ::close(fd);
        }
        ::freeaddrinfo(res);
        return m_fd >= 0;
    }
    void send(uint8_t b) override {
        if (m_fd >= 0 && ::send(m_fd, &b, 1, MSG_NOSIGNAL) < 0 && errno != EAGAIN) hangUp();
    }
    bool receive(uint8_t& b) override {
        if (m_rx.empty()) return false;
        b = m_rx.front();
        m_rx.pop_front();
        return true;
    }
    void poll() override {
        if (m_fd < 0 || m_rx.size() > 256) return;
        uint8_t buf[256];
        const ssize_t n = ::recv(m_fd, buf, sizeof buf, 0);
        if (n > 0) m_rx.insert(m_rx.end(), buf, buf + n);
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) hangUp();
    }
    std::string describe() const override { return "connected to " + m_name + (m_fd >= 0 ? "" : " (closed)"); }
    bool dcd() const override { return m_fd >= 0 || !m_rx.empty(); }

private:
    void hangUp() {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
    }
    int m_fd = -1;
    std::string m_name;
    std::deque<uint8_t> m_rx;
};

bool serialPort(int port) { return port >= 0 && port < DriveWireServer::SERIAL_PORTS; }
bool windowPort(int port) { return port >= DriveWireServer::WINDOW_BASE && port < DriveWireServer::CHANNELS; }

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace

DriveWireServer::DriveWireServer() {
    m_clock = [] {
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_r(&now, &t);
        return t;
    };
}

DriveWireServer::~DriveWireServer() = default;

uint16_t DriveWireServer::checksum(const uint8_t* data, size_t n) {
    uint16_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum = (uint16_t)(sum + data[i]);
    return sum;
}

void DriveWireServer::reset() {
    m_cmd.clear();
    m_need = 0;
    m_quiet = 0;
    m_out.clear();
    resetChannels();
}

// The guest starts again: no channel is open (the host sides stay as they are)
void DriveWireServer::resetChannels() {
    for (auto& c : m_channels) {
        if (!c) continue;
        c->open = c->pathOpen = c->closing = false;
        c->line.clear();
        if (c->online) {
            c->endpoint.reset();
            c->online = c->connected = false;
        }
    }
}

uint8_t DriveWireServer::send() {
    if (m_out.empty()) return 0;
    const uint8_t b = m_out.front();
    m_out.pop_front();
    return b;
}

void DriveWireServer::advance(double seconds) {
    m_sincePoll += seconds;
    m_sinceWrite += seconds;
    for (auto& c : m_channels)
        if (c) c->sinceRead += seconds;
    if (m_sincePoll >= 0.005) {             // the endpoints, every 5 ms of guest time
        m_sincePoll = 0;
        pollChannels();
    }
    if (m_cmd.empty()) return;
    m_quiet += seconds;
    if (m_quiet >= COMMAND_TIMEOUT) done();     // the rest never came: resynchronise
}

// The bytes a command needs before the server acts (more may follow: dispatch)
size_t DriveWireServer::lengthOf(uint8_t op) const {
    switch (op) {
        case OP_TIME: case OP_SERREAD: case OP_INIT: case OP_TERM: case OP_PRINTFLUSH: case OP_NOP:
        case OP_RESET1: case OP_RESET2: case OP_RESET3:
            return 1;
        case OP_DWINIT: case OP_SERINIT: case OP_SERTERM: case OP_PRINT:
        case OP_NAMEOBJ_MOUNT: case OP_NAMEOBJ_CREATE:   // then the name
            return 2;
        case OP_GETSTAT: case OP_SETSTAT: case OP_SERGETSTAT: case OP_SERSETSTAT: case OP_SERWRITE:
        case OP_SERREADM: case OP_SERWRITEM:              // then the bytes
            return 3;
        case OP_READ: case OP_REREAD: case OP_READEX: case OP_REREADEX:
            return 5;
        case OP_SETTIME:
            return 7;
        case OP_WRITE: case OP_REWRITE:
            return 5 + 256 + 2;
        default:
            if (op >= OP_FASTWRITE && op < OP_FASTWRITE + CHANNELS) return 2;   // $80 + port, byte
            return 1;
    }
}

void DriveWireServer::done() {
    m_cmd.clear();
    m_need = 0;
    m_quiet = 0;
}

void DriveWireServer::receive(uint8_t b) {
    // A new command: an answer the guest didn't read is stale (SS.KySns's
    // byte, which Level 2's scdwv doesn't read)
    if (m_cmd.empty()) m_out.clear();
    m_cmd.push_back(b);
    m_quiet = 0;
    if (m_cmd.size() == 1) m_need = lengthOf(b);
    if (m_cmd.size() >= m_need) dispatch();
}

void DriveWireServer::dispatch() {
    const uint8_t op = m_cmd[0];
    m_stats.lastOp = op;
    switch (op) {
        case OP_TIME: {
            const std::tm t = m_clock();
            push((uint8_t)t.tm_year);
            push((uint8_t)(t.tm_mon + 1));
            push((uint8_t)t.tm_mday);
            push((uint8_t)t.tm_hour);
            push((uint8_t)t.tm_min);
            push((uint8_t)t.tm_sec);
            break;
        }
        case OP_DWINIT:
            resetChannels();                // (dwio's Init: before any channel opens)
            push(SERVER_VERSION);
            ++m_stats.inits;
            break;
        case OP_SERREAD:
            ++m_stats.serialPolls;
            serRead();
            break;
        case OP_SERREADM: {
            // The bytes OP_SERREAD announced (dwio may take fewer: its buffer's room)
            Channel* ch = channel(m_cmd[1]);
            for (int i = 0; ch && i < m_cmd[2] && !ch->in.empty(); ++i) {
                push(ch->in.front());
                ch->in.pop_front();
                ch->sinceRead = 0;
                ++m_stats.serialIn;
            }
            break;
        }
        case OP_SERWRITE:
            guestWrite(m_cmd[1], &m_cmd[2], 1);
            break;
        case OP_SERWRITEM:
            if (m_cmd.size() == 3 && m_cmd[2] > 0) { m_need = 3 + m_cmd[2]; return; }
            guestWrite(m_cmd[1], m_cmd.data() + 3, m_cmd.size() - 3);
            break;
        case OP_SERINIT:
            if (Channel* ch = channel(m_cmd[1])) {
                ch->open = true;
                ch->closing = false;
                ch->line.clear();
            }
            break;
        case OP_SERTERM:
            if (Channel* ch = channel(m_cmd[1])) {
                ch->open = ch->pathOpen = ch->closing = false;
                ch->line.clear();
                if (ch->online) {
                    ch->endpoint.reset();
                    ch->online = ch->connected = false;
                }
                if (ch->modem) ch->in.clear();      // (a command's reply nobody read)
            }
            break;
        case OP_SERSETSTAT:
            if (m_cmd.size() == 3 && m_cmd[2] == SS_COMST) { m_need = 3 + COMST_BYTES; return; }
            if (Channel* ch = channel(m_cmd[1])) {
                if (m_cmd[2] == SS_COMST) ch->comSt.assign(m_cmd.begin() + 3, m_cmd.end());
                else if (m_cmd[2] == SS_OPEN) ch->pathOpen = true;
                else if (m_cmd[2] == SS_CLOSE) {
                    // The last path closed: a `tcp connect` ends with it
                    ch->pathOpen = false;
                    ch->line.clear();
                    if (ch->online) {
                        ch->endpoint.reset();
                        ch->online = ch->connected = false;
                    }
                }
            }
            break;
        case OP_NAMEOBJ_MOUNT: case OP_NAMEOBJ_CREATE:
            if (m_cmd.size() == 2 && m_cmd[1] > 0) { m_need = 2 + m_cmd[1]; return; }
            push(0);                                // no named objects: failed
            break;
        case OP_PRINT:
            m_print.push_back(m_cmd[1]);
            break;
        case OP_READ: case OP_REREAD: {
            const int drive = m_cmd[1];
            const uint32_t lsn = lsnOf();
            m_stats.lastDrive = drive;
            m_stats.lastLsn = lsn;
            ++(op == OP_READ ? m_stats.reads : m_stats.rereads);
            uint8_t data[256];
            const uint8_t err = readSector(drive, lsn, data);
            if (err) { push(err); break; }
            const uint16_t sum = checksum(data, 256);
            push(E_OK);
            push((uint8_t)(sum >> 8));
            push((uint8_t)sum);
            for (uint8_t d : data) push(d);
            break;
        }
        case OP_READEX: case OP_REREADEX:
            if (m_cmd.size() == 5) {
                // The sector (zeros if it can't be read), then the guest's checksum
                const int drive = m_cmd[1];
                const uint32_t lsn = lsnOf();
                m_stats.lastDrive = drive;
                m_stats.lastLsn = lsn;
                ++(op == OP_READEX ? m_stats.reads : m_stats.rereads);
                m_readErr = readSector(drive, lsn, m_sector.data());
                if (m_readErr) m_sector.fill(0);
                for (uint8_t d : m_sector) push(d);
                m_need = 7;
                return;
            }
            if (m_readErr) {
                push(m_readErr);
            } else if (((uint16_t)m_cmd[5] << 8 | m_cmd[6]) != checksum(m_sector.data(), 256)) {
                ++m_stats.crcErrors;
                push(E_CRC);
            } else {
                push(E_OK);
            }
            break;
        case OP_WRITE: case OP_REWRITE: {
            const int drive = m_cmd[1];
            const uint32_t lsn = lsnOf();
            m_stats.lastDrive = drive;
            m_stats.lastLsn = lsn;
            ++m_stats.writes;
            const uint8_t* data = &m_cmd[5];
            if (((uint16_t)m_cmd[261] << 8 | m_cmd[262]) != checksum(data, 256)) {
                ++m_stats.crcErrors;
                push(E_CRC);
            } else {
                push(writeSector(drive, lsn, data));
            }
            break;
        }
        case OP_INIT:
            // The guest's driver started (DriveWire 3's init; 4's is OP_DWINIT)
            ++m_stats.inits;
            break;
        case OP_NOP: case OP_TERM: case OP_RESET1: case OP_RESET2: case OP_RESET3:
            break;
        case OP_SERGETSTAT:
            // The driver advertises its GetStats; Level 1's scdwv then reads
            // SS.KySns's key-sense byte back (no keys down)
            if (m_cmd.size() > 2 && m_cmd[2] == SS_KYSNS) push(0);
            break;
        case OP_GETSTAT: case OP_SETSTAT: case OP_PRINTFLUSH: case OP_SETTIME:
            break;
        default:
            if (op >= OP_FASTWRITE && op < OP_FASTWRITE + CHANNELS) guestWrite(op - OP_FASTWRITE, &m_cmd[1], 1);
            else ++m_stats.unknownOps;
            break;
    }
    done();
}

bool DriveWireServer::mount(int drive, const std::string& path, Mode mode) {
    if (drive < 0 || drive >= DRIVES || path.empty()) return false;
    auto d = std::make_unique<Drive>();
    d->path = path;
    d->mode = mode;
    if (mode == Mode::WriteThrough)
        d->file = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!d->file || !*d->file) {
        // Not writable: read only (writes answer E$WP)
        d->file = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in);
        if (!*d->file) return false;
        if (mode == Mode::WriteThrough) d->mode = Mode::ReadOnly;
    }
    d->file->seekg(0, std::ios::end);
    d->size = (uint64_t)d->file->tellg();
    m_drives[drive] = std::move(d);
    return true;
}

bool DriveWireServer::eject(int drive) {
    if (!mounted(drive)) return false;
    m_drives[drive].reset();
    return true;
}

std::string DriveWireServer::imagePath(int drive) const { return mounted(drive) ? m_drives[drive]->path : ""; }

DriveWireServer::Mode DriveWireServer::mode(int drive) const {
    return mounted(drive) ? m_drives[drive]->mode : Mode::ReadOnly;
}

uint32_t DriveWireServer::sectors(int drive) const {
    if (!mounted(drive)) return 0;
    const Drive& d = *m_drives[drive];
    uint32_t n = (uint32_t)((d.size + 255) / 256);
    if (!d.overlay.empty()) n = std::max(n, d.overlay.rbegin()->first + 1);
    return n;
}

uint8_t DriveWireServer::readSector(int drive, uint32_t lsn, uint8_t* out) {
    std::memset(out, 0, 256);
    if (!mounted(drive)) return E_NOTRDY;
    Drive& d = *m_drives[drive];
    const auto o = d.overlay.find(lsn);
    if (o != d.overlay.end()) {
        std::memcpy(out, o->second.data(), 256);
        return E_OK;
    }
    const uint64_t at = (uint64_t)lsn * 256;
    if (at >= d.size) return E_OK;                          // past the end: zeros
    d.file->clear();
    d.file->seekg((std::streamoff)at);
    d.file->read((char*)out, 256);
    if (d.file->bad()) return E_READ;
    d.file->clear();
    return E_OK;
}

uint8_t DriveWireServer::writeSector(int drive, uint32_t lsn, const uint8_t* data) {
    if (!mounted(drive)) return E_NOTRDY;
    Drive& d = *m_drives[drive];
    if (d.mode == Mode::ReadOnly) return E_WP;
    if (d.mode == Mode::CopyOnWrite) {
        std::memcpy(d.overlay[lsn].data(), data, 256);
        return E_OK;
    }
    const uint64_t at = (uint64_t)lsn * 256;
    d.file->clear();
    if (at > d.size) {
        // Past the end: fill the gap with zeros first
        d.file->seekp((std::streamoff)d.size);
        const std::vector<uint8_t> zeros((size_t)(at - d.size), 0);
        d.file->write((const char*)zeros.data(), (std::streamsize)zeros.size());
    }
    d.file->seekp((std::streamoff)at);
    d.file->write((const char*)data, 256);
    d.file->flush();
    if (!*d.file) {
        d.file->clear();
        return E_WRITE;
    }
    d.size = std::max<uint64_t>(d.size, at + 256);
    return E_OK;
}

std::string DriveWireServer::describe() const {
    std::string s;
    for (int i = 0; i < DRIVES; ++i) {
        if (!m_drives[i]) continue;
        const std::string& p = m_drives[i]->path;
        const auto slash = p.find_last_of('/');
        if (!s.empty()) s += "; ";
        s += "drive " + std::to_string(i) + ": " + (slash == std::string::npos ? p : p.substr(slash + 1));
        if (m_drives[i]->mode == Mode::CopyOnWrite) s += " (copy on write)";
        else if (m_drives[i]->mode == Mode::ReadOnly) s += " (read only)";
    }
    return s;
}

// ---------------------------------------------------------------------------
// Virtual channels
// ---------------------------------------------------------------------------
DriveWireServer::Channel* DriveWireServer::channel(int port) {
    if (port < 0 || port >= CHANNELS) return nullptr;
    if (!m_channels[port]) {
        m_channels[port] = std::make_unique<Channel>();
        m_channels[port]->modem = port >= 1 && port < SERIAL_PORTS;     // /N1-/N13, /MIDI
        if (m_channels[port]->modem) m_channels[port]->spec = "modem";
    }
    return m_channels[port].get();
}

const DriveWireServer::Channel* DriveWireServer::channel(int port) const {
    return port >= 0 && port < CHANNELS ? m_channels[port].get() : nullptr;
}

bool DriveWireServer::guestQuiet() const {
    if (m_sinceWrite < CLOSE_QUIET) return false;
    for (const auto& c : m_channels)
        if (c && c->open && !c->in.empty()) return false;
    return true;
}

// OP_SERREAD: the next open channel (round robin) with something for the guest
void DriveWireServer::serRead() {
    for (int i = 0; i < CHANNELS; ++i) {
        const int port = (m_nextPoll + i) % CHANNELS;
        Channel* ch = m_channels[port].get();
        if (!ch || !ch->open) continue;
        if (serialPort(port)) {
            if (ch->in.empty()) {
                if (!ch->closing || ch->sinceRead < CLOSE_DELAY) continue;   // (its reader may not have run yet)
                if (!guestQuiet()) continue;    // (NitrOS-9 Level 1's dwio: see CLOSE_QUIET)
                ch->closing = false;
                push(0x10);                     // the channel closed: the guest hangs up
                push((uint8_t)port);
            } else if (ch->in.size() < 3) {
                push((uint8_t)(1 + port));      // one byte
                push(ch->in.front());
                ch->in.pop_front();
                ch->sinceRead = 0;
                ++m_stats.serialIn;
            } else {
                push((uint8_t)(0x11 + port));   // bytes waiting: the guest asks OP_SERREADM
                push((uint8_t)std::min<size_t>(ch->in.size(), 255));
            }
        } else if (windowPort(port)) {
            if (ch->in.empty()) continue;
            push((uint8_t)(0x40 | (port & 0x0F)));  // a key for the window
            push(ch->in.front());
            ch->in.pop_front();
            ch->sinceRead = 0;
            ++m_stats.serialIn;
        } else {
            continue;
        }
        m_nextPoll = (port + 1) % CHANNELS;
        return;
    }
    push(0);
    push(0);
}

// Bytes the guest wrote to a channel: to its endpoint, or the virtual modem
void DriveWireServer::guestWrite(int port, const uint8_t* data, size_t n) {
    Channel* ch = channel(port);
    if (!ch) return;
    m_sinceWrite = 0;
    m_stats.serialOut += n;
    ch->out.append((const char*)data, n);
    if (ch->out.size() > MAX_OUTPUT) ch->out.erase(0, ch->out.size() - MAX_OUTPUT);
    if (ch->endpoint) {
        for (size_t i = 0; i < n; ++i) ch->endpoint->send(data[i]);
        return;
    }
    if (!ch->modem) return;
    for (size_t i = 0; i < n; ++i) {
        const char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            const std::string line = ch->line;
            ch->line.clear();
            if (line.find_first_not_of(" \t") != std::string::npos) modemLine(*ch, line);
            if (ch->endpoint) {                 // connected: the rest goes to the connection
                for (size_t j = i + 1; j < n; ++j) ch->endpoint->send(data[j]);
                return;
            }
        } else if (ch->line.size() < 256) {
            ch->line += c;
        }
    }
}

void DriveWireServer::modemReply(Channel& ch, const std::string& text, bool close) {
    ch.in.insert(ch.in.end(), text.begin(), text.end());
    if (close) ch.closing = true;
}

// A line to the virtual modem: an API command (pyDriveWire's dwcommand.py;
// NitrOS-9's dw.as and net.as read the answers)
void DriveWireServer::modemLine(Channel& ch, const std::string& line) {
    std::vector<std::string> words;
    std::istringstream ss(line);
    for (std::string w; ss >> w;) words.push_back(w);
    const std::string verb = lower(words[0]);
    if (verb == "dw") {
        // A line dw skips, then the text; then the channel closes (dw's S$HUP)
        bool ok = true;
        const std::string text = apiCommand(std::vector<std::string>(words.begin() + 1, words.end()), ok);
        modemReply(ch, ok ? "OK\r" + text : "FAIL 208 " + text + "\r", true);
        return;
    }
    if (verb == "tcp" && words.size() >= 2 && lower(words[1]) == "connect") {
        std::string host, port;
        if (words.size() == 3) {                // tcp connect HOST:PORT
            const auto colon = words[2].rfind(':');
            if (colon != std::string::npos) {
                host = words[2].substr(0, colon);
                port = words[2].substr(colon + 1);
            }
        } else if (words.size() >= 4) {
            host = words[2];
            port = words[3];
        }
        const int p = std::atoi(port.c_str());
        if (!m_network) {
            modemReply(ch, "FAIL 205 network access is off (on the host: drivewire network on)\r", false);
            return;
        }
        if (host.empty() || p <= 0 || p > 65535) {
            modemReply(ch, "FAIL 203 usage: tcp connect HOST PORT\r", false);
            return;
        }
        auto conn = std::make_unique<TcpClientEndpoint>();
        std::string err;
        if (!conn->connect(host, p, err)) {
            modemReply(ch, "FAIL 246 " + host + ":" + port + ": " + err + "\r", false);
            return;
        }
        ch.endpoint = std::move(conn);
        ch.online = ch.connected = true;
        ch.cooked = false;
        modemReply(ch, "OK connected to " + host + ":" + port + "\r", false);
        return;
    }
    modemReply(ch, "FAIL 208 not supported: " + line + "\r", false);
}

// `dw ...`: a few of the DriveWire 4 server's commands
std::string DriveWireServer::apiCommand(const std::vector<std::string>& w, bool& ok) {
    const std::string a = w.size() > 0 ? lower(w[0]) : "", b = w.size() > 1 ? lower(w[1]) : "";
    if (a == "disk" && (b == "show" || b.empty())) {
        std::string s = "Drive  Image\r\n";
        bool any = false;
        for (int i = 0; i < DRIVES; ++i) {
            if (!mounted(i)) continue;
            std::string num = std::to_string(i);
            num.resize(7, ' ');
            s += num + imagePath(i);
            if (mode(i) == Mode::CopyOnWrite) s += " (copy on write)";
            else if (mode(i) == Mode::ReadOnly) s += " (read only)";
            s += "\r\n";
            any = true;
        }
        if (!any) s += "(none)\r\n";
        return s;
    }
    if (a == "disk" && b == "eject" && w.size() > 2) {
        if (!eject(std::atoi(w[2].c_str()))) { ok = false; return "no disk in drive " + w[2]; }
        return "Drive " + w[2] + " ejected\r\n";
    }
    if (a == "server" && (b == "version" || b.empty())) return "mmsim's DriveWire 4 server (protocol version 4)\r\n";
    if (a == "port" && (b == "show" || b.empty())) {
        std::string s = "Port   Status\r\n";
        for (int i = 0; i < CHANNELS; ++i) {
            const Channel* ch = channel(i);
            if (!ch || !ch->open) continue;
            std::string name = portName(i);
            name.resize(7, ' ');
            s += name + (ch->pathOpen ? "in use, " : "") + channelEndpoint(i) + "\r\n";
        }
        return s;
    }
    ok = false;
    std::string cmd = "dw";
    for (const auto& x : w) cmd += " " + x;
    return "not supported: " + cmd;
}

bool DriveWireServer::channelOpen(int port) const {
    const Channel* ch = channel(port);
    return ch && ch->open;
}

bool DriveWireServer::channelPathOpen(int port) const {
    const Channel* ch = channel(port);
    return ch && ch->pathOpen;
}

void DriveWireServer::hostWrite(int port, const std::vector<uint8_t>& bytes) {
    if (Channel* ch = channel(port)) ch->in.insert(ch->in.end(), bytes.begin(), bytes.end());
}

size_t DriveWireServer::hostPending(int port) const {
    const Channel* ch = channel(port);
    return ch ? ch->in.size() : 0;
}

const std::string& DriveWireServer::channelOutput(int port) const {
    static const std::string none;
    const Channel* ch = channel(port);
    return ch ? ch->out : none;
}

void DriveWireServer::clearChannelOutput(int port) {
    if (Channel* ch = channel(port)) ch->out.clear();
}

std::vector<uint8_t> DriveWireServer::channelComSt(int port) const {
    const Channel* ch = channel(port);
    return ch ? ch->comSt : std::vector<uint8_t>();
}

void DriveWireServer::hostClose(int port) {
    Channel* ch = channel(port);
    if (!ch || !serialPort(port)) return;
    if (ch->online) {
        ch->endpoint.reset();
        ch->online = ch->connected = false;
    }
    ch->closing = true;
}

bool DriveWireServer::attachChannel(int port, const std::string& specIn, std::string& err) {
    if (port < 0 || port >= CHANNELS || port == SERIAL_PORTS) {
        err = "no channel " + std::to_string(port) + " (0-14, $10-$1F)";
        return false;
    }
    Channel* ch = channel(port);
    std::string spec = specIn;
    bool cooked = true;
    const auto comma = spec.find(',');
    if (comma != std::string::npos) {
        if (spec.substr(comma + 1) != "raw") {
            err = "unknown option '" + spec.substr(comma + 1) + "' (raw)";
            return false;
        }
        cooked = false;
        spec = spec.substr(0, comma);
    }
    std::unique_ptr<SerialEndpoint> e;
    if (spec == "modem") {
        if (!(port >= 1 && port < SERIAL_PORTS)) {
            err = "the virtual modem is on /N1-/N13 and /MIDI (ports 1-14)";
            return false;
        }
    } else if (spec != "none") {
        e = makeSerialEndpoint(spec);
        if (!e) {
            err = "can't attach '" + spec + "' (tcp:PORT, file:PATH, loopback, modem, none)";
            return false;
        }
    }
    ch->endpoint = std::move(e);
    ch->modem = spec == "modem";
    ch->spec = specIn;
    ch->online = ch->connected = false;
    ch->cooked = cooked;
    ch->lastCr = false;
    ch->iac = 0;
    ch->line.clear();
    return true;
}

std::string DriveWireServer::channelEndpoint(int port) const {
    const Channel* ch = channel(port);
    if (!ch) return port >= 1 && port < SERIAL_PORTS ? "modem" : "none";
    if (ch->endpoint) return ch->online ? "modem, " + ch->endpoint->describe() : ch->endpoint->describe() + (ch->cooked ? "" : ", raw");
    return ch->spec;
}

void DriveWireServer::pollChannels() {
    for (int port = 0; port < CHANNELS; ++port) {
        Channel* ch = m_channels[port].get();
        if (!ch || !ch->endpoint) continue;
        SerialEndpoint& e = *ch->endpoint;
        e.poll();
        uint8_t b;
        while (ch->in.size() < 4096 && e.receive(b)) {
            if (!ch->cooked) {
                ch->in.push_back(b);
                continue;
            }
            // Telnet-style text: IAC sequences dropped, LF and CR LF to CR (SCF's end of line)
            switch (ch->iac) {
                case 1:
                    if (b == 0xFF) { ch->iac = 0; break; }                  // IAC IAC: $FF
                    ch->iac = b >= 251 && b <= 254 ? 2 : b == 250 ? 3 : 0;  // WILL/WONT/DO/DONT opt; SB
                    continue;
                case 2: ch->iac = 0; continue;
                case 3: if (b == 0xFF) ch->iac = 4; continue;
                case 4: ch->iac = b == 240 ? 0 : 3; continue;               // IAC SE
                default:
                    if (b == 0xFF) { ch->iac = 1; continue; }
                    break;
            }
            const bool afterCr = ch->lastCr;
            ch->lastCr = b == '\r';
            if ((b == '\n' || b == 0) && afterCr) continue;
            ch->in.push_back(b == '\n' ? '\r' : b);
        }
        // The far end hung up: a `tcp connect` ends; the guest's paths to a
        // serial channel (not the console) hear it
        const bool connected = e.dcd();
        if (ch->connected && !connected) {
            if (ch->online) {
                ch->endpoint.reset();
                ch->online = false;
            }
            if (port >= 1 && port < SERIAL_PORTS && ch->open) ch->closing = true;
        }
        ch->connected = connected;
    }
}

std::string DriveWireServer::portName(int port) {
    if (port == 0 || port == WINDOW_BASE) return "Term";
    if (port == 14) return "MIDI";
    if (port >= 1 && port < 14) return "N" + std::to_string(port);
    if (windowPort(port)) return "Z" + std::to_string(port - WINDOW_BASE);
    return "port " + std::to_string(port);
}

int DriveWireServer::parsePort(const std::string& nameIn) {
    std::string name = lower(nameIn);
    if (!name.empty() && name[0] == '/') name = name.substr(1);
    if (name == "term") return 0;
    if (name == "midi") return 14;
    char kind = 0;
    if (!name.empty() && (name[0] == 'n' || name[0] == 'z')) {
        kind = name[0];
        name = name.substr(1);
    }
    if (name.empty()) return -1;
    char* end = nullptr;
    const long v = name[0] == '$' ? std::strtol(name.c_str() + 1, &end, 16) : std::strtol(name.c_str(), &end, 0);
    if (*end) return -1;
    if (kind == 'n') return v >= 1 && v < SERIAL_PORTS ? (int)v : -1;
    if (kind == 'z') return v >= 1 && v < 16 ? WINDOW_BASE + (int)v : -1;
    return (v >= 0 && v < SERIAL_PORTS) || (v >= WINDOW_BASE && v < CHANNELS) ? (int)v : -1;
}

std::string DriveWireServer::describeChannels() const {
    std::string s;
    for (int port = 0; port < CHANNELS; ++port) {
        const Channel* ch = channel(port);
        if (!ch || (!ch->open && !ch->endpoint && ch->in.empty() && ch->out.empty())) continue;
        char num[8];
        std::snprintf(num, sizeof num, "$%02X", port);
        s += "  channel " + std::string(num) + " (/" + portName(port) + "): " + (ch->open ? "open" : "closed");
        if (ch->pathOpen) s += ", in use";
        s += "; " + channelEndpoint(port);
        if (!ch->in.empty()) s += "; " + std::to_string(ch->in.size()) + " bytes waiting";
        if (!ch->out.empty()) s += "; " + std::to_string(ch->out.size()) + " bytes written";
        s += "\n";
    }
    return s;
}

// The CLI's `drivewire` command
bool DriveWireServer::parseOnOff(const std::string& value, bool& on) {
    const std::string v = lower(value);
    if (v == "on" || v == "1" || v == "true" || v == "yes") { on = true; return true; }
    if (v == "off" || v == "0" || v == "false" || v == "no") { on = false; return true; }
    return false;
}

std::string DriveWireServer::hostCommand(const std::vector<std::string>& args) {
    const std::string usage =
        "Usage: drivewire                     the drives and virtual channels\n"
        "       drivewire channel CH SPEC     CH's far end: tcp:PORT[,raw], file:PATH, loopback, modem, none\n"
        "       drivewire type CH TEXT...     TEXT and a carriage return to the guest on CH\n"
        "       drivewire send CH TEXT...     TEXT alone (escapes: \\r \\n \\e \\xHH)\n"
        "       drivewire read CH             what the guest wrote to CH since the last read\n"
        "       drivewire hangup CH           tell the guest CH closed (serial channels)\n"
        "       drivewire network [on|off]    whether the guest's tcp connect may reach the network (default off)\n"
        "  CH: term (0), n1-n13, midi (14), z1-z15 ($11-$1F), or a number\n";
    if (args.empty() || args[0] == "show" || args[0] == "status") {
        std::string s = "DriveWire 4 server: " + (describe().empty() ? std::string("no drives") : describe()) + "\n";
        s += std::string("  network access (tcp connect): ") + (m_network ? "on" : "off") + "\n";
        const std::string ch = describeChannels();
        s += ch.empty() ? "  no virtual channel in use\n" : ch;
        return s;
    }
    const std::string verb = lower(args[0]);
    if (verb == "network" || verb == "net") {
        if (args.size() >= 2) {
            bool on = false;
            if (!parseOnOff(args[1], on)) return "Syntax: drivewire network [on|off]\n";
            m_network = on;
        }
        return std::string("DriveWire network access (tcp connect): ") + (m_network ? "on" : "off") + "\n";
    }
    if (args.size() < 2) return usage;
    const int port = parsePort(args[1]);
    if (port < 0) return "No channel '" + args[1] + "'.\n" + usage;
    std::string text;
    for (size_t i = 2; i < args.size(); ++i) text += (i > 2 ? " " : "") + args[i];
    const std::string label = "/" + portName(port);
    if (verb == "channel" || verb == "attach") {
        if (args.size() < 3) return label + ": " + channelEndpoint(port) + "\n";
        std::string err;
        if (!attachChannel(port, args[2], err)) return "Error: " + err + "\n";
        return label + ": " + channelEndpoint(port) + "\n";
    }
    if (verb == "type" || verb == "send") {
        std::string bytes;
        for (size_t i = 0; i < text.size(); ++i) {
            if (verb == "send" && text[i] == '\\' && i + 1 < text.size()) {
                const char e = text[++i];
                if (e == 'r') bytes += '\r';
                else if (e == 'n') bytes += '\n';
                else if (e == 'e') bytes += '\x1b';
                else if (e == 'x' && i + 2 < text.size()) {
                    bytes += (char)std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16);
                    i += 2;
                } else bytes += e;
            } else {
                bytes += text[i];
            }
        }
        if (verb == "type") bytes += '\r';
        hostWrite(port, bytes);
        return label + ": " + std::to_string(bytes.size()) + " bytes queued" +
               (channelOpen(port) ? "" : " (the guest hasn't opened it yet)") + "\n";
    }
    if (verb == "read") {
        // CR LF and a lone CR as new lines
        const std::string out = channelOutput(port);
        clearChannelOutput(port);
        std::string s;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i] == '\r') {
                s += '\n';
                if (i + 1 < out.size() && out[i + 1] == '\n') ++i;
            } else if (out[i] != '\0') {
                s += out[i];
            }
        }
        if (s.empty()) return label + ": nothing new\n";
        return s.back() == '\n' ? s : s + "\n";
    }
    if (verb == "hangup" || verb == "close") {
        if (!serialPort(port)) return "Only a serial channel (0-14) hangs up.\n";
        hostClose(port);
        return label + ": hangup sent\n";
    }
    return usage;
}
