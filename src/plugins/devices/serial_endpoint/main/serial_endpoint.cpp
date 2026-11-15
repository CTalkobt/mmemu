// Serial endpoints: the byte-level far end of a UART. The TCP link is the
// one the TRS-80's RS-232 board uses (trs80_serial.cpp), as an object.

#include "serial_endpoint.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

bool LoopbackSerialEndpoint::receive(uint8_t& b) {
    if (m_queue.empty()) return false;
    b = m_queue.front();
    m_queue.pop_front();
    return true;
}

FileSerialEndpoint::~FileSerialEndpoint() {
    if (m_file) std::fclose(m_file);
}

bool FileSerialEndpoint::open(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    if (m_file) std::fclose(m_file);
    m_file = f;
    m_path = path;
    return true;
}

void FileSerialEndpoint::send(uint8_t b) {
    if (!m_file) return;
    std::fputc(b, m_file);
    std::fflush(m_file);
}

TcpSerialEndpoint::~TcpSerialEndpoint() {
    if (m_clientFd >= 0) ::close(m_clientFd);
    if (m_listenFd >= 0) ::close(m_listenFd);
}

bool TcpSerialEndpoint::listen(int port) {
    if (port <= 0 || port > 65535) return false;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, (sockaddr*)&a, sizeof a) < 0 || ::listen(fd, 1) < 0) {
        ::close(fd);
        return false;
    }
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    m_listenFd = fd;
    m_port = port;
    return true;
}

void TcpSerialEndpoint::send(uint8_t b) {
    if (m_clientFd >= 0 && ::send(m_clientFd, &b, 1, MSG_NOSIGNAL) < 0) {
        ::close(m_clientFd);
        m_clientFd = -1;
    }
}

bool TcpSerialEndpoint::receive(uint8_t& b) {
    if (m_rx.empty()) return false;
    b = m_rx.front();
    m_rx.pop_front();
    return true;
}

void TcpSerialEndpoint::poll() {
    if (m_clientFd < 0 && m_listenFd >= 0) {
        const int c = ::accept(m_listenFd, nullptr, nullptr);
        if (c >= 0) {
            ::fcntl(c, F_SETFL, O_NONBLOCK);
            m_clientFd = c;
        }
    }
    if (m_clientFd < 0 || m_rx.size() > 256) return;
    uint8_t buf[64];
    const ssize_t n = ::recv(m_clientFd, buf, sizeof buf, 0);
    if (n > 0) m_rx.insert(m_rx.end(), buf, buf + n);
    else if (n == 0) {
        ::close(m_clientFd);
        m_clientFd = -1;
    }
}

std::string TcpSerialEndpoint::describe() const {
    return "tcp:" + std::to_string(m_port) + (m_clientFd >= 0 ? " (client connected)" : " (waiting)");
}

namespace {
std::map<std::string, SerialSchemeFactory>& schemes() {
    static std::map<std::string, SerialSchemeFactory> s;
    return s;
}
}  // namespace

void registerSerialScheme(const std::string& scheme, SerialSchemeFactory factory) {
    schemes()[scheme] = std::move(factory);
}

std::unique_ptr<SerialEndpoint> makeSerialEndpoint(const std::string& spec) {
    if (spec.empty() || spec == "none") return std::make_unique<NullSerialEndpoint>();
    if (spec == "loopback") return std::make_unique<LoopbackSerialEndpoint>();
    const size_t colon = spec.find(':');
    if (colon == std::string::npos) {
        // A registered scheme may take no argument ("tpdd" alone)
        auto it = schemes().find(spec);
        return it != schemes().end() ? it->second("") : nullptr;
    }
    const std::string scheme = spec.substr(0, colon), arg = spec.substr(colon + 1);
    if (scheme == "file") {
        auto f = std::make_unique<FileSerialEndpoint>();
        return f->open(arg) ? std::move(f) : nullptr;
    }
    if (scheme == "tcp") {
        auto t = std::make_unique<TcpSerialEndpoint>();
        return t->listen(std::atoi(arg.c_str())) ? std::move(t) : nullptr;
    }
    auto it = schemes().find(scheme);
    return it != schemes().end() ? it->second(arg) : nullptr;
}
