#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SerialEndpoint;

/**
 * A DriveWire 4 server (#356): the host side of the DriveWire protocol, the
 * CoCo's (or Dragon's) virtual disks and clock over a byte stream. It knows
 * nothing of the transport: the guest's bytes go in through receive() and
 * its answers come out of send() / hasOutput(). The Becker port (BeckerCart,
 * coco_storage.h) feeds it a byte per register write; a bit-banger serial
 * transport can feed it the same stream.
 *
 * After the DriveWire 4 specification (Boisy Pitre / Aaron Wolfe, the
 * DriveWire wiki), checked against the clients it must satisfy: NitrOS-9's
 * rbdw, dwio, clock2_dw and boot_dw (nitros9project/nitros9, level1/modules)
 * and HDB-DOS (nitros9project/toolshed, hdbdos/), and pyDriveWire's server.
 *
 * Transactions (guest bytes -> host bytes):
 *
 * | Op | Name | Guest sends | Host answers |
 * |----|------|-------------|--------------|
 * | $00 | OP_NOP | op | - |
 * | $49 'I' | OP_INIT | op | - |
 * | $54 'T' | OP_TERM | op | - |
 * | $F8 $FE $FF | OP_RESET3/1/2 | op | - |
 * | $5A 'Z' | OP_DWINIT | op, driver version | server version (4: DW4) |
 * | $23 '#' | OP_TIME | op | year-1900, month 1-12, day, hour, minute, second |
 * | $24 '$' | OP_SETTIME | op + those 6 | - (the host clock is not set) |
 * | $52 'R' $72 'r' | OP_READ / OP_REREAD | op, drive, LSN (24 bits, MSB first) | 0, checksum (2, MSB first), 256 data; or one error byte |
 * | $D2 $F2 | OP_READEX / OP_REREADEX | op, drive, LSN; then checksum (2) after the data | 256 data (zeros on error); then 0, $F3 (E$CRC: checksum mismatch) or the error |
 * | $57 'W' $77 'w' | OP_WRITE / OP_REWRITE | op, drive, LSN, 256 data, checksum (2) | 0, $F3, or the error |
 * | $47 'G' $53 'S' | OP_GETSTAT / OP_SETSTAT | op, drive, code | - (logged) |
 * | $43 'C' | OP_SERREAD | op | 2 bytes: what the virtual channels have (below) |
 * | $63 'c' | OP_SERREADM | op, channel, count | `count` of the channel's queued bytes |
 * | $C3 | OP_SERWRITE | op, channel, byte | - (to the channel) |
 * | $64 | OP_SERWRITEM | op, channel, count, bytes | - (to the channel) |
 * | $80-$9F | OP_FASTWRITE | op ($80 + channel), byte | - (to the channel) |
 * | $44 'D' | OP_SERGETSTAT | op, channel, code | - ; SS.KySns $27: 0 (Level 1's scdwv reads a byte) |
 * | $C4 | OP_SERSETSTAT | op, channel, code (+26 for SS.ComSt $28) | - (SS.Open $29, SS.Close $2A noted) |
 * | $45 'E' $C5 | OP_SERINIT / OP_SERTERM | op, channel | - (the guest's driver opens / closes the channel) |
 * | $50 'P' | OP_PRINT | op, byte | - (kept: printBuffer) |
 * | $46 'F' | OP_PRINTFLUSH | op | - |
 * | $01 $02 | OP_NAMEOBJ_MOUNT / CREATE | op, length, name | 0 (failed: no named objects) |
 *
 * A new command drops answer bytes the guest left unread (the SS.KySns
 * byte Level 2's scdwv doesn't read). Other opcodes are taken as one byte and ignored (unknownOps counts them).
 * The checksum is the 16-bit sum of the 256 data bytes.
 *
 * Errors: $F2 E$WP (write protected), $F3 E$CRC, $F4 E$Read, $F5 E$Write,
 * $F6 E$NotRdy (no image in the drive).
 *
 * Drives 0-255, each a raw image of 256-byte sectors (a NitrOS-9 or RS-DOS
 * .dsk): LSN n at byte n * 256. A read past the image's end is zeros; a
 * write there extends it. Writes go through to the file at once, or (Mode::
 * CopyOnWrite) are kept in memory and read back, the file untouched;
 * Mode::ReadOnly refuses them (E$WP), as does a file that can't be written.
 *
 * A command whose bytes stop arriving is dropped after 250 ms of guest
 * time (the spec's host timeout), so the stream resynchronises; advance()
 * gives the time.
 *
 * Virtual channels (NitrOS-9's scdwv: /Term and /N1-/N13, /MIDI = 0-14;
 * the windows /Z1-/Z15 = $11-$1F, the headless builds' /Term on 0). A
 * channel is a port number (the descriptor's, V.PORT+1): the guest's
 * driver opens it with OP_SERINIT and closes it with OP_SERTERM; between
 * them its writes (SERWRITE, SERWRITEM, FASTWRITE $80 + port) go to the
 * channel's host side, and bytes the host side has for it are offered
 * when dwio polls (OP_SERREAD, from a VIRQ), round robin over the open
 * channels. The answer to OP_SERREAD (dwio's IRQSvc):
 *
 * | Bytes | Meaning |
 * |-------|---------|
 * | 0, 0 | nothing |
 * | 1 + port, byte | one byte for serial port 0-14 (sent while 1 or 2 wait) |
 * | $11 + port, count | `count` (1-255) bytes wait: the guest asks OP_SERREADM port, n <= count |
 * | $10, port | serial port 0-14 closed by the host (CLOSE_DELAY after its last byte was read): the guest hangs up (S$HUP) its paths to it |
 * | $40 + (port & $0F), byte | a key for window port $10-$1F |
 *
 * ($10, $FF would reboot the guest: never sent.) A channel's host side is
 * an endpoint (attachChannel: tcp:PORT, file:PATH, loopback, none) and,
 * always, the in-process API (hostWrite, channelOutput) for tests and the
 * CLI's `drivewire` command. A serial channel 1-14 with no endpoint is the
 * DriveWire 4 server's "virtual modem": the lines the guest writes there
 * are API commands (NitrOS-9's dw, telnet and inetd utilities open /N and
 * write them): `dw disk show`, `dw server version`, `dw port show` answer
 * (a line the client skips, the text, then the channel closes);
 * `tcp connect HOST PORT` connects the channel to a TCP server ("OK
 * connected", then the channel carries the connection until either end
 * closes); anything else answers `FAIL 010 ...` (net.as's error format).
 */
class DriveWireServer {
public:
    enum class Mode { WriteThrough, CopyOnWrite, ReadOnly };

    static constexpr uint8_t OP_NOP = 0x00, OP_NAMEOBJ_MOUNT = 0x01, OP_NAMEOBJ_CREATE = 0x02,
        OP_TIME = 0x23, OP_SETTIME = 0x24, OP_SERREAD = 0x43, OP_SERGETSTAT = 0x44, OP_SERINIT = 0x45,
        OP_PRINTFLUSH = 0x46, OP_GETSTAT = 0x47, OP_INIT = 0x49, OP_PRINT = 0x50, OP_READ = 0x52,
        OP_SETSTAT = 0x53, OP_TERM = 0x54, OP_WRITE = 0x57, OP_DWINIT = 0x5A, OP_SERREADM = 0x63,
        OP_SERWRITEM = 0x64, OP_REREAD = 0x72, OP_REWRITE = 0x77, OP_FASTWRITE = 0x80,
        OP_SERWRITE = 0xC3, OP_SERSETSTAT = 0xC4, OP_SERTERM = 0xC5, OP_READEX = 0xD2,
        OP_REREADEX = 0xF2, OP_RESET3 = 0xF8, OP_RESET1 = 0xFE, OP_RESET2 = 0xFF;
    static constexpr uint8_t E_OK = 0x00, E_WP = 0xF2, E_CRC = 0xF3, E_READ = 0xF4, E_WRITE = 0xF5, E_NOTRDY = 0xF6;
    static constexpr uint8_t SS_KYSNS = 0x27, SS_COMST = 0x28, SS_OPEN = 0x29, SS_CLOSE = 0x2A;
    /** Channels by port: serial 0-14, windows $10-$1F (16 is the windows' /Term). */
    static constexpr int CHANNELS = 32, SERIAL_PORTS = 15, WINDOW_BASE = 0x10;
    /** SS.ComSt's payload: SCF's PD.OPT, OPTCNT bytes. */
    static constexpr int COMST_BYTES = 26;
    /** OP_DWINIT's answer: a DriveWire 4 server (NitrOS-9's dwio then polls OP_SERREAD). */
    static constexpr uint8_t SERVER_VERSION = 4;
    static constexpr int DRIVES = 256;
    static constexpr double COMMAND_TIMEOUT = 0.25;
    /** A hang-up waits this long (guest time) after the guest last took a byte
     *  of the channel: its reader must run first, or S$HUP cuts the read short. */
    static constexpr double CLOSE_DELAY = 0.5;

    struct Stats {
        uint64_t reads = 0, writes = 0, rereads = 0, crcErrors = 0, unknownOps = 0, serialPolls = 0;
        uint64_t serialIn = 0, serialOut = 0;   // bytes to / from the guest's channels
        int lastOp = -1, lastDrive = -1;
        uint32_t lastLsn = 0;
    };

    DriveWireServer();
    ~DriveWireServer();

    /** The guest reset: a part-received command and unread answers dropped. */
    void reset();
    /** A byte from the guest. */
    void receive(uint8_t b);
    /** Whether an answer byte is waiting for the guest. */
    bool hasOutput() const { return !m_out.empty(); }
    size_t outputSize() const { return m_out.size(); }
    /** The next answer byte (0 if none). */
    uint8_t send();
    uint8_t peekOutput() const { return m_out.empty() ? 0 : m_out.front(); }
    /** Guest time passing (for the command timeout). */
    void advance(double seconds);
    /** No command part-way through. */
    bool idle() const { return m_cmd.empty(); }

    /** An image in drive 0-255. */
    bool mount(int drive, const std::string& path, Mode mode = Mode::WriteThrough);
    bool eject(int drive);
    std::string imagePath(int drive) const;
    bool mounted(int drive) const { return drive >= 0 && drive < DRIVES && m_drives[drive] != nullptr; }
    Mode mode(int drive) const;
    /** The image's size in sectors (with copy-on-write sectors past its end). */
    uint32_t sectors(int drive) const;
    /** Read a sector as the guest would (for tests and the debugger): the error code. */
    uint8_t readSector(int drive, uint32_t lsn, uint8_t* out);

    /** OP_TIME's source (default: the host's local time). */
    void setClock(std::function<std::tm()> clock) { m_clock = std::move(clock); }
    const Stats& stats() const { return m_stats; }
    /** Bytes from OP_PRINT (the virtual printer). */
    const std::vector<uint8_t>& printBuffer() const { return m_print; }
    void clearPrintBuffer() { m_print.clear(); }
    /** "drive 0: nitros9.dsk; drive 1: ..." */
    std::string describe() const;

    // --- Virtual channels: the host side -----------------------------------
    /** Whether the guest's driver has the channel open (OP_SERINIT, not yet OP_SERTERM). */
    bool channelOpen(int port) const;
    /** Whether a guest path has it open (SS.Open seen, not yet SS.Close). */
    bool channelPathOpen(int port) const;
    /** Bytes for the guest on `port` (as if typed at the far end). */
    void hostWrite(int port, const std::vector<uint8_t>& bytes);
    void hostWrite(int port, const std::string& text) { hostWrite(port, std::vector<uint8_t>(text.begin(), text.end())); }
    /** Bytes waiting for the guest on `port`. */
    size_t hostPending(int port) const;
    /** What the guest has written to `port` (the last MAX_OUTPUT bytes). */
    const std::string& channelOutput(int port) const;
    void clearChannelOutput(int port);
    /** The PD.OPT bytes of the last SS.ComSt on `port` (empty if none). */
    std::vector<uint8_t> channelComSt(int port) const;
    /** The host hangs up `port` (a serial port 0-14): the guest is told it closed. */
    void hostClose(int port);
    /** The channel's far end: "tcp:PORT" (a listening socket; telnet-style text:
     *  LF / CR LF in become CR, telnet IAC sequences are dropped), the same
     *  ",raw" (bytes as they come), "file:PATH" (what the guest writes),
     *  "loopback", "none" (detached: the in-process API only). False with
     *  `err` if the spec fails. */
    bool attachChannel(int port, const std::string& spec, std::string& err);
    std::string channelEndpoint(int port) const;
    /** Endpoint housekeeping (accept, read, notice a hang-up); advance() does it
     *  every few milliseconds of guest time. */
    void pollChannels();
    /** "0 (/Term): open, tcp:6809 (client connected); ..." */
    std::string describeChannels() const;
    /** A port's name as NitrOS-9's descriptors have it: Term, N1-N13, MIDI, Z1-Z15. */
    static std::string portName(int port);
    /** A port from a name: 0-31, $1F, term, n3, midi, z2 (-1 if none). */
    static int parsePort(const std::string& name);
    /** The CLI's `drivewire` command (the arguments after it); its reply. */
    std::string hostCommand(const std::vector<std::string>& args);
    static constexpr size_t MAX_OUTPUT = 1 << 20;

    static uint16_t checksum(const uint8_t* data, size_t n);

private:
    struct Drive;
    struct Channel;
    void dispatch();
    Channel* channel(int port);
    const Channel* channel(int port) const;
    void serRead();
    void guestWrite(int port, const uint8_t* data, size_t n);
    void modemLine(Channel& ch, const std::string& line);
    void modemReply(Channel& ch, const std::string& text, bool close);
    void resetChannels();
    std::string apiCommand(const std::vector<std::string>& words, bool& ok);
    size_t lengthOf(uint8_t op) const;
    void done();
    uint8_t writeSector(int drive, uint32_t lsn, const uint8_t* data);
    uint32_t lsnOf() const { return (uint32_t)m_cmd[2] << 16 | (uint32_t)m_cmd[3] << 8 | m_cmd[4]; }
    void push(uint8_t b) { m_out.push_back(b); }

    std::unique_ptr<Drive> m_drives[DRIVES];
    std::vector<uint8_t> m_cmd;
    size_t m_need = 0;
    double m_quiet = 0;                     // seconds since the last byte of a part command
    std::array<uint8_t, 256> m_sector{};    // OP_READEX's data, for its checksum
    uint8_t m_readErr = 0;
    std::deque<uint8_t> m_out;
    std::vector<uint8_t> m_print;
    std::function<std::tm()> m_clock;
    Stats m_stats;
    std::unique_ptr<Channel> m_channels[CHANNELS];
    int m_nextPoll = 0;                     // round robin over the channels
    double m_sincePoll = 0;
};
