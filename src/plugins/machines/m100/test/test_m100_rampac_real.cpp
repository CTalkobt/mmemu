// The RAMPAC with real software: Paul Globman's RAMDSK (the current RAM100.CO
// from bkw777's NODE_DATAPAC, which knows up to 4 banks) on a Model 100 and
// a Tandy 200 with their real ROMs, when m100.rom / t200.rom and ram100.do /
// ram200.do (NODE_DATAPAC's software/RAMDSK/RAMxxx/RAMxxx.DO) are in
// roms/m100/ (else it says so and passes). The loader is installed as its README says (`dl -b`): BASIC's
// LOAD "COM:98N1E" takes it over RS-232, and running it pokes the program in
// and saves it. RAMDSK then formats the card, saves a file to it, finds and
// formats the 512 KB card's second bank, and starts again from the menu.

#include "test_harness.h"
#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"
#include "plugins/machines/m100/main/machine_m100.h"
#include "plugins/machines/m100/main/m100_system.h"
#include "plugins/machines/m100/test/m100_test_rom.h"
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace m100test;
using m100::Model;

namespace {

bool have(const std::string& file) {
    std::ifstream f(m100::firmwareDir() + "/" + file);
    if (!f) std::printf("  (no %s in %s: skipped)\n", file.c_str(), m100::firmwareDir().c_str());
    return (bool)f;
}

// The ROM's text screen: the 100's 8 x 40 at $FE00, the 200's 16 x 40 at $FA30
bool g_t200 = false;

std::string lcdText(MachineDescriptor* m) {
    std::string s;
    const uint32_t base = g_t200 ? 0xFA30 : 0xFE00;
    for (int i = 0; i < (g_t200 ? 640 : 320); ++i) {
        const uint8_t c = m->buses[0].bus->peek8(base + (uint32_t)i);
        s += (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
        if (i % 40 == 39) s += '\n';
    }
    return s;
}

bool waitLcd(MachineDescriptor* m, const std::string& text, double seconds) {
    for (double t = 0; t < seconds; t += 0.25) {
        if (lcdText(m).find(text) != std::string::npos) return true;
        runFor(m, 0.25);
    }
    if (lcdText(m).find(text) != std::string::npos) return true;
    std::printf("  waiting for \"%s\":\n%s", text.c_str(), lcdText(m).c_str());
    return false;
}

// The far end of the RS-232 cable sending a file, as `dl -b` does, minding XON/XOFF
class FileSender : public SerialEndpoint {
public:
    explicit FileSender(std::deque<uint8_t> data) : m_data(std::move(data)) {}
    void send(uint8_t b) override {
        if (b == 0x13) m_paused = true;
        else if (b == 0x11) m_paused = false;
    }
    bool receive(uint8_t& b) override {
        if (m_paused || m_data.empty()) return false;
        b = m_data.front();
        m_data.pop_front();
        return true;
    }
    std::string describe() const override { return "file sender"; }
    bool done() const { return m_data.empty(); }

private:
    std::deque<uint8_t> m_data;
    bool m_paused = false;
};

}  // namespace

// RAMDSK on `model`: `rom` and the loader `loader` (RAMxxx.DO) for program `name`, made at `start`
void ramdsk(Model model, const char* rom, const char* loader, const std::string& name, int start) {
    if (!have(rom) || !have(loader)) return;
    g_t200 = model == Model::M200;
    std::string err;
    MachineDescriptor* m = m100::create(model, &err);
    ASSERT(m != nullptr);
    auto* sys = m100::systemOf(m);
    ASSERT(m->expansionSlot("sysbus")->install("rampac512", "", err));
    sys->powerOn();
    ASSERT(waitLcd(m, "Bytes free", 10));
    runFor(m, 2);
    auto key = [&](const char* k) {
        sys->keyboard.pressKeyByName(k, true);
        runFor(m, 0.1);
        sys->keyboard.pressKeyByName(k, false);
        runFor(m, 0.5);
    };

    // The loader over RS-232 (9600 baud, 8 bits, no parity, 1 stop, XON/XOFF), ended by ^Z
    std::ifstream f(m100::firmwareDir() + "/" + loader, std::ios::binary);
    std::deque<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!data.empty() && data.back() == 0x1A) data.pop_back();
    data.push_back(0x1A);
    auto sender = std::make_shared<FileSender>(std::move(data));
    sys->serialMux->rs232 = sender;
    if (g_t200) sys->usart.setEndpoint(sys->serialMux);
    else sys->uart.setEndpoint(sys->serialMux);
    sys->keyboard.enqueueText("\\n");                  // BASIC
    ASSERT(waitLcd(m, "Ok", 5));
    // (the 200's COM: settings take two more letters than the 100's)
    sys->keyboard.enqueueText(g_t200 ? "LOAD \"COM:98N1ENN\"\\n" : "LOAD \"COM:98N1E\"\\n");
    for (int i = 0; i < 120 && !sender->done(); ++i) runFor(m, 0.5);
    ASSERT(sender->done());
    runFor(m, 2);
    sys->keyboard.enqueueText("RUN\\n");
    ASSERT(waitLcd(m, "Please type: NEW", 60));
    sys->keyboard.enqueueText("NEW\\n");
    runFor(m, 1);
    sys->keyboard.enqueueText("CLEAR 0," + std::to_string(start) + "\\n");   // HIMEM under it, as for any .CO
    runFor(m, 1);
    sys->keyboard.enqueueText("RUNM \"" + name + "\"\\n");
    ASSERT(waitLcd(m, "Format?", 10));
    sys->keyboard.enqueueText("Y");
    ASSERT(waitLcd(m, "Sure?", 5));
    sys->keyboard.enqueueText("Y");
    ASSERT(waitLcd(m, "K free", 30));
    auto* r = sys->rampac();
    ASSERT_EQ(r->ram()[0], 0x40);                      // the format stamp
    ASSERT_EQ(r->ram()[1], 0x04);
    const std::string before = lcdText(m);
    ASSERT(before.find("K free") != std::string::npos);

    // F3 Save: the program itself, onto the card
    key("f3");
    ASSERT(waitLcd(m, "Save:", 5));
    sys->keyboard.enqueueText(name + ".CO\\n");
    ASSERT(waitLcd(m, "  as:", 5));
    runFor(m, 0.5);
    sys->keyboard.enqueueText("\\n");                // the same name
    ASSERT(waitLcd(m, "253K free", 10));               // ~1.4 KB + headers: 2 blocks
    runFor(m, 1);
    // The file's first block: its name in the RAMDSK header, then the .CO header (its start)
    bool found = false;
    for (int blk = 1; blk < 256 && !found; ++blk) {
        const uint8_t* b = &r->ram()[(size_t)blk * 1024];
        if (std::string(b, b + 8) == name + "CO") {
            found = true;
            ASSERT_EQ(b[10] | b[11] << 8, start);
        }
    }
    ASSERT(found);

    // F1 Bank: the 512 KB card's second bank ($85) is blank and asks to be formatted
    key("f1");
    ASSERT(waitLcd(m, "Bank1", 5));
    ASSERT(waitLcd(m, "Format?", 5));
    sys->keyboard.enqueueText("Y");
    ASSERT(waitLcd(m, "Sure?", 5));
    sys->keyboard.enqueueText("Y");
    ASSERT(waitLcd(m, "255K free", 30));
    ASSERT_EQ(r->ram()[Rampac::BANK_SIZE], 0x40);      // bank 1's own stamp
    ASSERT_EQ(r->ram()[Rampac::BANK_SIZE + 1], 0x04);
    ASSERT(lcdText(m).find(name + ".CO") == std::string::npos);
    // ... and F1 again: back to bank 0, its file still there
    key("f1");
    ASSERT(waitLcd(m, "Bank0", 5));
    ASSERT(waitLcd(m, name + ".CO  1", 5));
    ASSERT(waitLcd(m, "253K free", 5));
    key("f8");                                          // Menu
    ASSERT(waitLcd(m, "Bytes free", 5));
    runFor(m, 1);
    // From the menu, the card already formatted: straight to its listing. The 100's menu
    // takes the full name (it doesn't find a .CO by its bare name); the 200's has no
    // name prompt, so the cursor goes to it (after the 200's MSPLAN)
    ASSERT(waitLcd(m, "TELCOM", 5));
    if (g_t200) {
        for (int i = 0; i < 6; ++i) key("right");
        key("enter");
    } else {
        sys->keyboard.enqueueText(name + ".CO\\n");
    }
    ASSERT(waitLcd(m, name + ".CO  1", 10));
    ASSERT(waitLcd(m, "253K free", 5));
    delete m;
}

TEST_CASE(m100_rampac_ramdsk) { ramdsk(Model::M100, "m100.rom", "ram100.do", "RAM100", 61558); }

TEST_CASE(m100_t200_rampac_ramdsk) { ramdsk(Model::M200, "t200.rom", "ram200.do", "RAM200", 59715); }
