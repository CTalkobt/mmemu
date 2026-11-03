#include "sid_file.h"
#include "libmem/main/ibus.h"
#include "libcore/main/icore.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace psid {

namespace {

uint16_t be16(const std::vector<uint8_t>& d, size_t off) {
    return (uint16_t)((d[off] << 8) | d[off + 1]);
}

std::string str32(const std::vector<uint8_t>& d, size_t off) {
    std::string s;
    for (size_t i = 0; i < 32 && d[off + i]; ++i) s += (char)d[off + i];
    return s;
}

std::string hex4(uint16_t v) {
    std::ostringstream o;
    o << '$' << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << v;
    return o.str();
}

constexpr int DRIVER_SIZE = 64;

bool overlaps(uint32_t a, uint32_t aLen, uint32_t b, uint32_t bLen) {
    return a < b + bLen && b < a + aLen;
}

}  // namespace

uint8_t bankFor(uint16_t addr) {
    if (addr < 0xA000) return 0x37;   // BASIC, KERNAL and I/O
    if (addr < 0xD000) return 0x36;   // KERNAL and I/O
    if (addr >= 0xE000) return 0x35;  // I/O only
    return 0x34;                      // RAM only
}

bool parse(const std::vector<uint8_t>& file, Info& info, std::string& err) {
    if (file.size() < 0x76) { err = "File too small for a PSID header."; return false; }
    info.magic.assign(reinterpret_cast<const char*>(file.data()), 4);
    if (info.magic != "PSID" && info.magic != "RSID") { err = "Not a PSID/RSID file."; return false; }
    info.version = be16(file, 0x04);
    uint16_t dataOffset = be16(file, 0x06);
    info.loadAddr  = be16(file, 0x08);
    info.initAddr  = be16(file, 0x0A);
    info.playAddr  = be16(file, 0x0C);
    info.songs     = std::max(1, (int)be16(file, 0x0E));
    info.startSong = std::max(1, (int)be16(file, 0x10));
    uint32_t speed = ((uint32_t)be16(file, 0x12) << 16) | be16(file, 0x14);
    info.title    = str32(file, 0x16);
    info.author   = str32(file, 0x36);
    info.released = str32(file, 0x56);
    if (info.version >= 2 && file.size() >= 0x78) {
        uint16_t flags = be16(file, 0x76);
        info.clock    = (flags >> 2) & 3;
        info.sidModel = (flags >> 4) & 3;
    }
    if (dataOffset >= file.size()) { err = "No data after the header."; return false; }
    size_t data = dataOffset, len = file.size() - dataOffset;
    if (info.loadAddr == 0) {        // load address in the first two data bytes
        if (len < 2) { err = "No load address."; return false; }
        info.loadAddr = (uint16_t)(file[data] | (file[data + 1] << 8));
        len -= 2;
    }
    if ((uint32_t)info.loadAddr + len > 0x10000) { err = "Data runs past $FFFF."; return false; }
    info.dataLen = (uint32_t)len;
    if (info.initAddr == 0) info.initAddr = info.loadAddr;
    // Speed bit per song (songs past 32 use bit 31); RSID tunes time themselves
    info.ciaSpeed = info.magic == "RSID" || ((speed >> std::min(info.startSong - 1, 31)) & 1);
    return true;
}

bool load(const std::vector<uint8_t>& file, int subtune, IBus* bus, ICore* cpu,
          Info& info, std::string& err) {
    if (!parse(file, info, err)) return false;
    if (!bus || !cpu) { err = "No machine."; return false; }
    info.subtune = subtune < 1 ? info.startSong : std::min(subtune, info.songs);
    uint32_t speed = ((uint32_t)be16(file, 0x12) << 16) | be16(file, 0x14);
    info.ciaSpeed = info.magic == "RSID" || ((speed >> std::min(info.subtune - 1, 31)) & 1);

    // The data
    size_t data = be16(file, 0x06) + (be16(file, 0x08) == 0 ? 2 : 0);
    for (uint32_t i = 0; i < info.dataLen; ++i)
        bus->write8(info.loadAddr + i, file[data + i]);

    // Free RAM for the driver, always visible whatever $01 is
    uint16_t at = 0;
    for (uint16_t cand : {0x0334, 0x0200})
        if (!overlaps(cand, DRIVER_SIZE, info.loadAddr, info.dataLen)) { at = cand; break; }
    for (uint32_t cand = 0xC000; !at && cand < 0xD000; cand += DRIVER_SIZE)
        if (!overlaps(cand, DRIVER_SIZE, info.loadAddr, info.dataLen)) at = (uint16_t)cand;
    for (uint32_t cand = 0x1000; !at && cand < 0xA000; cand += DRIVER_SIZE)
        if (!overlaps(cand, DRIVER_SIZE, info.loadAddr, info.dataLen)) at = (uint16_t)cand;
    if (!at) { err = "No free memory for the play driver."; return false; }
    info.driverAddr = at;

    std::vector<uint8_t> d;
    auto emit = [&](std::initializer_list<int> bytes) { for (int b : bytes) d.push_back((uint8_t)b); };
    auto setTimer = [&](uint16_t latch) {
        emit({0xA9, latch & 0xFF, 0x8D, 0x04, 0xDC,      // LDA #lo  STA $DC04
              0xA9, latch >> 8,   0x8D, 0x05, 0xDC});    // LDA #hi  STA $DC05
    };
    emit({0x78,                                          // SEI
          0xA9, 0x7F, 0x8D, 0x0D, 0xDC,                  // LDA #$7F STA $DC0D: CIA 1 IRQs off
          0xAD, 0x0D, 0xDC});                            // LDA $DC0D (acknowledge)
    if (info.ciaSpeed) setTimer(CIA_DEFAULT_LATCH);      // init may set its own
    emit({0xA9, bankFor(info.initAddr), 0x85, 0x01,      // LDA #bank STA $01
          0xA9, info.subtune - 1,                        // LDA #song
          0x20, info.initAddr & 0xFF, info.initAddr >> 8});  // JSR init
    if (info.playAddr == 0) {
        // The tune drives itself from its own interrupt handler
        uint16_t self = (uint16_t)(at + d.size() + 1);
        emit({0x58, 0x4C, self & 0xFF, self >> 8});      // CLI; JMP *
    } else {
        if (!info.ciaSpeed) setTimer(PAL_FRAME_LATCH);   // one frame, whatever init did
        emit({0xA9, 0x11, 0x8D, 0x0E, 0xDC});            // LDA #$11 STA $DC0E: run, continuous
        uint16_t loop = (uint16_t)(at + d.size());
        emit({0xAD, 0x0D, 0xDC,                          // loop: LDA $DC0D
              0x29, 0x01,                                //       AND #$01 (timer A underflow)
              0xF0, 0xF9,                                //       BEQ loop
              0xA9, bankFor(info.playAddr), 0x85, 0x01,  //       LDA #bank STA $01
              0x20, info.playAddr & 0xFF, info.playAddr >> 8,   // JSR play
              0x4C, loop & 0xFF, loop >> 8});            //       JMP loop
    }
    for (size_t i = 0; i < d.size(); ++i) bus->write8(at + (uint32_t)i, d[i]);
    cpu->setPc(at);
    return true;
}

std::string describe(const Info& info) {
    std::ostringstream o;
    o << info.magic << " v" << info.version << ": " << info.title << " by " << info.author;
    if (!info.released.empty()) o << " (" << info.released << ")";
    o << "\nSongs: " << info.songs << ", playing subtune " << info.subtune
      << "\nLoad: " << hex4(info.loadAddr) << "-" << hex4((uint16_t)(info.loadAddr + info.dataLen - 1))
      << ", Init: " << hex4(info.initAddr) << ", Play: " << hex4(info.playAddr)
      << "\nTiming: " << (info.playAddr == 0 ? "the tune's own interrupt handler"
                          : info.ciaSpeed ? "CIA 1 timer (the tune's rate, 60 Hz default)"
                                          : "50 Hz (one PAL frame)");
    if (info.sidModel == 1) o << ", SID 6581";
    else if (info.sidModel == 2) o << ", SID 8580";
    o << "\nPlay driver installed at " << hex4(info.driverAddr) << "; run the machine to play.";
    return o.str();
}

}  // namespace psid
