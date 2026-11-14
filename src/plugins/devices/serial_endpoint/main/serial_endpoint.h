#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * The far end of a serial port, at the byte level: what a UART sends goes
 * to send(); what the far end has for the UART comes out of receive().
 *
 * mmsim's existing serial devices (the TRS-80's TR1602, the 6551, the
 * CoCo's bit banger) each carry their own `loopback` / `file:` / `tcp:`
 * code; this is the same set of links as one reusable object, so a UART
 * can be wired to any of them, or to an emulated peripheral that speaks
 * a protocol (the TPDD) by implementing this interface.
 *
 * Specs (makeSerialEndpoint, the CLI `serial` argument):
 *  - `none`        nothing connected (sends are dropped, nothing arrives)
 *  - `loopback`    what is sent comes back
 *  - `file:PATH`   sends go to a host file; nothing arrives
 *  - `tcp:PORT`    a listening socket on localhost; one client at a time
 *  - any scheme registered with registerSerialScheme(), e.g. `tpdd:DIR`
 *
 * Modem lines: the far end's CTS, DSR and DCD (true = asserted); DTR and
 * RTS from the UART side. A link that has no such lines asserts them all.
 */
class SerialEndpoint {
public:
    virtual ~SerialEndpoint() = default;

    /** A byte from the UART to the far end. */
    virtual void send(uint8_t b) = 0;
    /** The next byte from the far end, if any (false: nothing waiting). */
    virtual bool receive(uint8_t& b) = 0;
    /** Housekeeping (accept a TCP client, read the socket); call often. */
    virtual void poll() {}
    /** The spec this endpoint was made from, plus its state. */
    virtual std::string describe() const = 0;

    virtual bool cts() const { return true; }
    virtual bool dsr() const { return true; }
    virtual bool dcd() const { return true; }
    virtual void setDtr(bool) {}
    virtual void setRts(bool) {}
};

/** `none`: drops what is sent, never has anything. */
class NullSerialEndpoint : public SerialEndpoint {
public:
    void send(uint8_t) override {}
    bool receive(uint8_t&) override { return false; }
    std::string describe() const override { return "none"; }
    bool cts() const override { return false; }
    bool dsr() const override { return false; }
    bool dcd() const override { return false; }
};

/** `loopback`: every byte sent comes back (also a test double: inject()). */
class LoopbackSerialEndpoint : public SerialEndpoint {
public:
    void send(uint8_t b) override { m_queue.push_back(b); }
    bool receive(uint8_t& b) override;
    std::string describe() const override { return "loopback"; }
    /** Queue bytes as if the far end sent them. */
    void inject(const std::vector<uint8_t>& bytes) { m_queue.insert(m_queue.end(), bytes.begin(), bytes.end()); }

private:
    std::deque<uint8_t> m_queue;
};

/** `file:PATH`: a capture of what is sent. */
class FileSerialEndpoint : public SerialEndpoint {
public:
    ~FileSerialEndpoint() override;
    /** False if the file can't be opened. */
    bool open(const std::string& path);
    void send(uint8_t b) override;
    bool receive(uint8_t&) override { return false; }
    std::string describe() const override { return "file:" + m_path; }

private:
    FILE* m_file = nullptr;
    std::string m_path;
};

/** `tcp:PORT`: a listening socket on 127.0.0.1; one client is the far end. */
class TcpSerialEndpoint : public SerialEndpoint {
public:
    ~TcpSerialEndpoint() override;
    /** False if the port can't be bound. */
    bool listen(int port);
    void send(uint8_t b) override;
    bool receive(uint8_t& b) override;
    void poll() override;
    std::string describe() const override;
    bool dcd() const override { return m_clientFd >= 0; }
    bool dsr() const override { return m_clientFd >= 0; }
    bool cts() const override { return m_clientFd >= 0; }

private:
    int m_listenFd = -1, m_clientFd = -1, m_port = 0;
    std::deque<uint8_t> m_rx;
};

/** Makes the endpoint for a scheme's argument (the text after "scheme:"); null on failure. */
using SerialSchemeFactory = std::function<std::unique_ptr<SerialEndpoint>(const std::string& arg)>;

/** Adds (or replaces) a scheme for makeSerialEndpoint, e.g. "tpdd". */
void registerSerialScheme(const std::string& scheme, SerialSchemeFactory factory);

/** The endpoint for a spec (see SerialEndpoint); null if it is unknown or fails. */
std::unique_ptr<SerialEndpoint> makeSerialEndpoint(const std::string& spec);
