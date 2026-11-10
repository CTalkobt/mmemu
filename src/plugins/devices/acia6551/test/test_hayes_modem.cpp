#include "test_harness.h"
#include "plugins/devices/acia6551/main/hayes_modem.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// The Hayes modem: PETSCII commands and results, dialing a TCP server on
// localhost, telnet negotiation, +++ and hanging up
namespace {
struct Rig {
    std::vector<uint8_t> got;
    double t = 0;
    HayesModem modem{[this](uint8_t b) { got.push_back(b); }, []() { return true; }};
    // Type a line as a C64 terminal sends it (PETSCII, unshifted letters)
    void type(const std::string& ascii, double gap = 0.01) {
        for (char c : ascii) { t += gap; modem.fromComputer(HayesModem::asciiToPetscii((uint8_t)tolower((unsigned char)c)), t); }
        t += gap;
        modem.fromComputer('\r', t);
    }
    // What came back, as ASCII
    std::string text() {
        std::string s;
        for (uint8_t b : got) s += (char)HayesModem::petsciiToAscii(b);
        got.clear();
        return s;
    }
    // Poll until `want` shows (or a second of wall time passes)
    bool waitFor(const std::string& want, std::string* all = nullptr) {
        std::string s;
        for (int i = 0; i < 200; ++i) {
            t += 0.005;
            modem.poll(t);
            s += text();
            if (s.find(want) != std::string::npos) { if (all) *all = s; return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (all) *all = s;
        return false;
    }
};

// A TCP server on 127.0.0.1 (an ephemeral port)
struct Server {
    int fd = -1, client = -1, port = 0;
    Server() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, (sockaddr*)&a, sizeof a);
        ::listen(fd, 1);
        socklen_t len = sizeof a;
        ::getsockname(fd, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    ~Server() { if (client >= 0) ::close(client); ::close(fd); }
    bool accept() {
        for (int i = 0; i < 200 && client < 0; ++i) {
            client = ::accept(fd, nullptr, nullptr);
            if (client < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (client >= 0) ::fcntl(client, F_SETFL, O_NONBLOCK);
        return client >= 0;
    }
    void send(const std::vector<uint8_t>& b) { ::send(client, b.data(), b.size(), 0); }
    std::vector<uint8_t> recv() {
        std::vector<uint8_t> out;
        for (int i = 0; i < 40; ++i) {
            uint8_t buf[256];
            ssize_t n = ::recv(client, buf, sizeof buf, 0);
            if (n > 0) out.insert(out.end(), buf, buf + n);
            else if (!out.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return out;
    }
};
}  // namespace

TEST_CASE(hayes_petscii_conversion) {
    EXPECT_EQ(HayesModem::petsciiToAscii(0x41), 'a');           // unshifted: lower case
    EXPECT_EQ(HayesModem::petsciiToAscii(0xC1), 'A');           // shifted
    EXPECT_EQ(HayesModem::petsciiToAscii('0'), '0');
    EXPECT_EQ(HayesModem::petsciiToAscii(0x14), 0x08);          // DEL
    EXPECT_EQ(HayesModem::asciiToPetscii('o'), 0x4F);
    EXPECT_EQ(HayesModem::asciiToPetscii('O'), 0xCF);
    EXPECT_EQ(HayesModem::asciiToPetscii(':'), ':');
}

TEST_CASE(hayes_command_mode) {
    Rig r;
    r.type("at");
    std::string s = r.text();
    EXPECT_TRUE(s.find("at\r") == 0);                            // echoed
    EXPECT_TRUE(s.find("\rOK\r") != std::string::npos);
    // The PETSCII on the wire: "OK" shifted
    r.type("at");
    bool shifted = false;
    for (size_t i = 0; i + 1 < r.got.size(); ++i) shifted = shifted || (r.got[i] == 0xCF && r.got[i + 1] == 0xCB);
    EXPECT_TRUE(shifted);
    r.got.clear();
    r.type("ate0");
    EXPECT_TRUE(r.text().find("OK") != std::string::npos);
    r.type("at");
    EXPECT_EQ(r.text(), std::string("\rOK\r"));                  // no echo now
    r.type("atv0");
    EXPECT_EQ(r.text(), std::string("0\r"));                     // numeric
    r.type("atv1 x4 &c1 &d2 s0=0 m1 l2");                        // a terminal's init string
    EXPECT_EQ(r.text(), std::string("\rOK\r"));
    r.type("ats0?");
    EXPECT_TRUE(r.text().find("0") != std::string::npos);
    r.type("atj");
    EXPECT_EQ(r.text(), std::string("\rERROR\r"));
    r.type("ati");
    EXPECT_TRUE(r.text().find("MMEMU HAYES MODEM") != std::string::npos);
    r.type("atdt nosuch.invalid:23");
    EXPECT_EQ(r.text(), std::string("\rNO CARRIER\r"));
    r.type("atdt 127.0.0.1:notaport");
    EXPECT_EQ(r.text(), std::string("\rNO CARRIER\r"));
    // ASCII results with *P0
    r.type("at*p0");
    r.got.clear();
    for (char c : std::string("AT\r")) { r.t += 0.01; r.modem.fromComputer((uint8_t)c, r.t); }
    EXPECT_EQ(std::string(r.got.begin(), r.got.end()), std::string("\r\nOK\r\n"));
    r.got.clear();
    // Backspace and A/
    r.modem.reset();
    r.type("ate0");
    r.got.clear();
    for (uint8_t b : {0x41, 0x54, 0x49, 0x14, 0x0D}) { r.t += 0.01; r.modem.fromComputer(b, r.t); }   // "ati", DEL: "at"
    EXPECT_EQ(r.text(), std::string("\rOK\r"));
    r.modem.fromComputer(0x41, r.t += 0.01);
    r.modem.fromComputer('/', r.t += 0.01);
    EXPECT_EQ(r.text(), std::string("\rOK\r"));
}

TEST_CASE(hayes_dials_tcp) {
    Server srv;
    Rig r;
    r.type("ate0");
    r.text();
    r.type("atdt 127.0.0.1:" + std::to_string(srv.port));
    EXPECT_TRUE(r.modem.dialing() || r.modem.carrier());
    ASSERT(srv.accept());
    EXPECT_TRUE(r.waitFor("CONNECT"));
    EXPECT_TRUE(r.modem.online());
    // Telnet: DO TTYPE is refused, WILL ECHO accepted, the data comes through;
    // IAC IAC is a data $FF
    srv.send({255, 253, 24, 255, 251, 1, 'H', 'I', 255, 255, '!'});
    std::string all;
    for (int i = 0; i < 50 && r.got.size() < 4; ++i) { r.t += 0.005; r.modem.poll(r.t); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    EXPECT_EQ(r.got.size(), 4u);
    if (r.got.size() == 4) {
        EXPECT_EQ(r.got[0], 'H');
        EXPECT_EQ(r.got[2], 0xFF);
        EXPECT_EQ(r.got[3], '!');
    }
    r.got.clear();
    auto answer = srv.recv();
    EXPECT_TRUE(answer.size() >= 6);
    if (answer.size() >= 6) {
        EXPECT_EQ(answer[1], 252);                                // WONT TTYPE
        EXPECT_EQ(answer[2], 24);
        EXPECT_EQ(answer[4], 253);                                // DO ECHO
    }
    // Online, the computer's bytes go out as they are; $FF doubled
    for (uint8_t b : {0x48, 0xC9, 0xFF}) r.modem.fromComputer(b, r.t += 0.01);
    auto sent = srv.recv();
    EXPECT_TRUE(sent == std::vector<uint8_t>({0x48, 0xC9, 0xFF, 0xFF}));
    // +++ without the guard before it is data
    for (int i = 0; i < 3; ++i) r.modem.fromComputer('+', r.t += 0.01);
    r.t += 2;
    r.modem.poll(r.t);
    EXPECT_TRUE(r.modem.online());
    srv.recv();
    // With a second of silence either side: command mode, still connected
    r.t += 1.5;
    for (int i = 0; i < 3; ++i) r.modem.fromComputer('+', r.t += 0.1);
    r.t += 1.2;
    EXPECT_TRUE(r.waitFor("OK"));
    EXPECT_TRUE(!r.modem.online());
    EXPECT_TRUE(r.modem.carrier());
    r.type("ato");
    EXPECT_TRUE(r.text().find("CONNECT") != std::string::npos);
    EXPECT_TRUE(r.modem.online());
    // Back out and hang up: the server sees the end
    r.t += 1.5;
    for (int i = 0; i < 3; ++i) r.modem.fromComputer('+', r.t += 0.1);
    r.t += 1.2;
    r.waitFor("OK");
    srv.recv();
    r.type("ath");
    EXPECT_TRUE(r.text().find("OK") != std::string::npos);
    EXPECT_TRUE(!r.modem.carrier());
    uint8_t b;
    ssize_t n = -1;
    for (int i = 0; i < 40 && n != 0; ++i) { n = ::recv(srv.client, &b, 1, 0); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    EXPECT_EQ(n, 0);
}

TEST_CASE(hayes_far_end_hangs_up_and_translates) {
    Server srv;
    Rig r;
    r.type("ate0 *t1 *n0");                                      // translated, raw TCP
    r.text();
    r.type("atd127.0.0.1:" + std::to_string(srv.port));
    ASSERT(srv.accept());
    ASSERT(r.waitFor("CONNECT"));
    srv.send({'H', 'i', '\r'});
    for (int i = 0; i < 50 && r.got.size() < 3; ++i) { r.t += 0.005; r.modem.poll(r.t); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    EXPECT_TRUE(r.got.size() == 3 && r.got[0] == 0xC8 && r.got[1] == 0x49);   // ASCII "Hi" as PETSCII
    r.got.clear();
    r.modem.fromComputer(0x41, r.t += 0.01);                      // PETSCII 'a'
    auto sent = srv.recv();
    EXPECT_TRUE(sent == std::vector<uint8_t>({'a'}));
    ::close(srv.client);
    srv.client = -1;
    EXPECT_TRUE(r.waitFor("NO CARRIER"));
    EXPECT_TRUE(!r.modem.carrier());
}

// Through a SwiftLink's 6551: DSR on with the modem there, DCD only with a
// carrier; the command goes out and the result comes back at the line rate
TEST_CASE(hayes_through_swiftlink) {
    Acia6551 a(0xDE00);
    a.setMode(Acia6551::Mode::SwiftLink);
    a.setClockHz(985248);
    ASSERT(a.attachSerial("modem"));
    uint8_t st = 0;
    a.ioPeek(nullptr, 0xDE01, &st);
    EXPECT_EQ(st & 0x60, 0x20);                                   // DSR asserted, no carrier
    EXPECT_TRUE(a.ioWrite(nullptr, 0xDE03, 0x1F));                // 19200 x2 = 38400, 8N1
    EXPECT_EQ((int)a.bitRate(), 38400);
    a.ioWrite(nullptr, 0xDE02, 0x0B);                             // DTR, transmitter on, no IRQs
    std::string got;
    auto pump = [&](int cycles) {
        for (int c = 0; c < cycles; c += 50) {
            a.tick(50);
            uint8_t s = 0;
            a.ioPeek(nullptr, 0xDE01, &s);
            if (s & 0x08) { uint8_t v; a.ioRead(nullptr, 0xDE00, &v); got += (char)HayesModem::petsciiToAscii(v); }
        }
    };
    for (uint8_t b : {0x41, 0x54, 0x0D}) {                        // "at" in PETSCII
        a.ioWrite(nullptr, 0xDE00, b);
        pump(400);
    }
    pump(20000);
    EXPECT_TRUE(got.find("OK") != std::string::npos);
    EXPECT_TRUE(!a.ioWrite(nullptr, 0xDE04, 0));                  // a SwiftLink is 4 registers
}

TEST_CASE(turbo232_registers) {
    Acia6551 a(0xDE00);
    a.setMode(Acia6551::Mode::Turbo232);
    a.setClockHz(985248);
    uint8_t v = 0;
    a.ioRead(nullptr, 0xDE07, &v);
    EXPECT_EQ(v, 0x04);                                           // control rate 0: enhanced on, 230400
    EXPECT_EQ((int)a.bitRate(), 230400);
    a.ioWrite(nullptr, 0xDE07, 0x01);
    EXPECT_EQ((int)a.bitRate(), 115200);
    a.ioRead(nullptr, 0xDE05, &v);
    EXPECT_EQ(v, 0xFF);
    a.ioWrite(nullptr, 0xDE03, 0x1E);                             // 9600 x2
    EXPECT_EQ((int)a.bitRate(), 19200);
    a.ioWrite(nullptr, 0xDE07, 0x02);                             // ignored while the rate isn't 0
    a.ioRead(nullptr, 0xDE07, &v);
    EXPECT_EQ(v, 0x01);
    std::string err;
    EXPECT_TRUE(a.setOption("base", "$D700", err));
    EXPECT_EQ(a.baseAddr(), 0xD700u);
    EXPECT_TRUE(!a.setOption("base", "DE80", err));
}
