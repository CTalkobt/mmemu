// The Game Boy (DMG) machine: gb::Gameboy with its screen, sound, joypad,
// cartridge slot and battery saves, and the SM83 for the debugger.

#include "machine_gameboy.h"
#include "gameboy.h"
#include "sm83_core.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ivideo_output.h"
#include "util/path_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

namespace gb {
namespace {

std::string firmwareDir() {
    const char* v = std::getenv("MMEMU_GB_FIRMWARE");
    return v ? v : PathUtil::findResource("roms/gameboy");
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    return f && f.write((const char*)data.data(), (std::streamsize)data.size());
}

std::string withExt(const std::string& path, const char* ext) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    const std::string stem = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? path.substr(0, dot) : path;
    return stem + ext;
}

/** The LCD: its four shades in the DMG's greens (BGB's palette). */
class GbVideo : public IOHandler, public IVideoOutput {
public:
    static constexpr int WIDTH = 160, HEIGHT = 144;
    explicit GbVideo(Gameboy* gb) : m_gb(gb) {}

    const char* name() const override { return "lcd"; }
    uint32_t baseAddr() const override { return 0xFF40; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}

    VideoDimensions getDimensions() const override { return {WIDTH, HEIGHT, WIDTH, HEIGHT}; }
    void renderFrame(uint32_t* buffer) override {
        static const uint32_t SHADES[4] = {0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820};
        const uint8_t* s = m_gb->screen();
        for (int i = 0; i < WIDTH * HEIGHT; ++i) buffer[i] = SHADES[s[i] & 3];
    }
private:
    Gameboy* m_gb;
};

/** The sound: the APU's stereo samples mixed to mono, through a DC blocker, for the host. */
class GbAudio : public IOHandler, public IAudioOutput {
public:
    static constexpr int RATE = 44100;
    explicit GbAudio(Gameboy* gb) : m_gb(gb) { m_gb->apu().setSampleRate(RATE, Gameboy::CLOCK_HZ); }

    const char* name() const override { return "apu"; }
    uint32_t baseAddr() const override { return 0xFF10; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_count = m_read = m_write = 0;
        m_dcIn = m_dcOut = 0;
    }
    void tick(uint64_t) override {}

    /** Take the APU's samples so far (the emulation's thread). */
    void drain() {
        m_gb->apu().run();
        auto& q = m_gb->apu().samples();
        std::lock_guard<std::mutex> lock(m_mutex);
        while (q.size() >= 2) {
            const double l = q.front(); q.pop_front();
            const double r = q.front(); q.pop_front();
            const double in = (l + r) * 0.5;
            const double out = in - m_dcIn + 0.999 * m_dcOut;
            m_dcIn = in;
            m_dcOut = out;
            float f = (float)(out / 12000.0);
            f = std::max(-1.0f, std::min(1.0f, f));
            m_buf[m_write] = f;
            m_write = (m_write + 1) % BUF;
            if (m_count < BUF) ++m_count;
            else m_read = (m_read + 1) % BUF;           // nobody taking them: the oldest go
        }
    }

    int nativeSampleRate() const override { return RATE; }
    int pullSamples(float* buffer, int maxSamples) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        int n = 0;
        while (n < maxSamples && m_count > 0) {
            buffer[n++] = m_buf[m_read];
            m_read = (m_read + 1) % BUF;
            --m_count;
        }
        return n;
    }
private:
    static constexpr int BUF = 16384;
    Gameboy* m_gb;
    std::mutex m_mutex;
    float m_buf[BUF] = {};
    int m_read = 0, m_write = 0, m_count = 0;
    double m_dcIn = 0, m_dcOut = 0;
};

/** Everything the machine is: the Game Boy and what surrounds it. */
struct GbMachine {
    Gameboy gb;
    std::string label, savePath;
    std::vector<uint8_t> saved, savedRtc;
    uint64_t nextHousekeeping = 0;
    int ticks = 0;
    bool hasCart = false;
    uint8_t keys = 0, joy[2] = {}, pads[2] = {};
    GbAudio* audio = nullptr;

    std::string rtcPath() const { return savePath.empty() ? "" : withExt(savePath, ".rtc"); }
    /** No cartridge: the bus floats high, so the CPU reads $FF (RST $38) everywhere it would be. */
    void insertEmpty() {
        std::vector<uint8_t> rom(0x8000, 0xFF);
        rom[0x147] = rom[0x148] = rom[0x149] = 0;          // "ROM only", 32 KB, no RAM
        std::string err;
        gb.load(rom, err);
        hasCart = false;
    }
    std::vector<uint8_t>* batteryRam() {
        return hasCart && gb.hasBattery() && !gb.cartRam().empty() ? &gb.cartRam() : nullptr;
    }
    void loadSave(const std::string& path) {
        savePath = path;
        saved.clear();
        savedRtc.clear();
        if (path.empty()) return;
        if (auto* rtc = gb.rtc()) {
            std::vector<uint8_t> data;
            if (readFile(rtcPath(), data)) rtc->setState(data);     // the time since it was saved passes
            savedRtc = rtc->state();
            savedRtc.resize(5);
        }
        auto* ram = batteryRam();
        if (!ram) return;
        std::vector<uint8_t> data;
        if (readFile(path, data) || readFile(withExt(path, ".srm"), data))
            if (!data.empty())
                for (size_t i = 0; i < ram->size(); ++i) (*ram)[i] = data[i % data.size()];
        saved = *ram;
    }
    void flushSave() {
        if (savePath.empty()) return;
        if (auto* rtc = gb.rtc()) {
            const auto st = rtc->state();
            if (savedRtc.size() != 5 || !std::equal(savedRtc.begin(), savedRtc.end(), st.begin()))
                if (writeFile(rtcPath(), st)) savedRtc.assign(st.begin(), st.begin() + 5);
        }
        auto* ram = batteryRam();
        if (ram && *ram != saved && writeFile(savePath, *ram)) saved = *ram;
    }
    void applyButtons() { gb.setButtons((uint8_t)(keys | joy[0] | joy[1] | pads[0])); }
    void powerOn() {
        std::vector<uint8_t> boot;
        if (readFile(firmwareDir() + "/dmg_boot.bin", boot) && boot.size() == 256) gb.setBootRom(boot);
        else gb.setBootRom({});
        gb.powerOn();
        if (audio) audio->reset();
        nextHousekeeping = gb.cycles() + Gameboy::CLOCK_HZ / 60;
    }
};

/** The debugger's bus, which also finds the machine from its descriptor. */
class MachineBus : public GbBus {
public:
    explicit MachineBus(GbMachine* m) : GbBus(&m->gb), machine(m) {}
    GbMachine* machine;
};

GbMachine* machineOf(MachineDescriptor* m) {
    if (!m) return nullptr;
    for (auto& b : m->buses)
        if (auto* p = dynamic_cast<MachineBus*>(b.bus)) return p->machine;
    return nullptr;
}

uint8_t padBits(uint32_t b) {
    using M = MachineDescriptor;
    uint8_t p = 0;
    if (b & (M::PAD_A | M::PAD_X)) p |= BTN_A;
    if (b & (M::PAD_B | M::PAD_Y)) p |= BTN_B;
    if (b & M::PAD_SELECT) p |= BTN_SELECT;
    if (b & M::PAD_START) p |= BTN_START;
    if (b & M::PAD_UP) p |= BTN_UP;
    if (b & M::PAD_DOWN) p |= BTN_DOWN;
    if (b & M::PAD_LEFT) p |= BTN_LEFT;
    if (b & M::PAD_RIGHT) p |= BTN_RIGHT;
    return p;
}

}  // namespace

bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label,
                     std::string& err, const std::string& savePath) {
    auto* gm = machineOf(m);
    if (!gm) { err = "Not a Game Boy."; return false; }
    gm->flushSave();
    if (!gm->gb.load(image, err)) return false;
    gm->hasCart = true;
    gm->label = label;
    gm->loadSave(savePath);
    gm->powerOn();
    return true;
}

void removeCartridge(MachineDescriptor* m) {
    auto* gm = machineOf(m);
    if (!gm) return;
    gm->flushSave();
    gm->savePath.clear();
    gm->label.clear();
    gm->insertEmpty();
    gm->powerOn();
}

std::string cartridgeInfo(MachineDescriptor* m) {
    auto* gm = machineOf(m);
    if (!gm || !gm->hasCart) return "";
    std::string d = gm->gb.mapperName();
    if (gm->gb.hasBattery()) d += ", battery";
    if (!gm->gb.header().title.empty()) d += ", \"" + gm->gb.header().title + "\"";
    return gm->label + " (" + d + ")";
}

void flushSave(MachineDescriptor* m) {
    if (auto* gm = machineOf(m)) gm->flushSave();
}

std::string savePathFor(const std::string& romPath) { return withExt(romPath, ".sav"); }

Gameboy* gameboyOf(MachineDescriptor* m) {
    auto* gm = machineOf(m);
    return gm ? &gm->gb : nullptr;
}

void setButtons(MachineDescriptor* m, uint8_t buttons) {
    if (auto* gm = machineOf(m)) { gm->keys = buttons; gm->applyButtons(); }
}

MachineDescriptor* createMachine(std::string* /*err*/) {
    auto* desc = new MachineDescriptor();
    desc->machineId = "gameboy";
    desc->displayName = "Nintendo Game Boy";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"cartridge"};
    desc->clockHz = Gameboy::CLOCK_HZ;
    desc->symbolFiles.push_back(PathUtil::findResource("roms/gameboy/hardware.sym"));   // $FF00-$FFFF's names

    auto* gm = new GbMachine();
    auto* bus = new MachineBus(gm);
    desc->buses.push_back({"system", bus});
    auto* cpu = new Sm83Core(&gm->gb, bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* video = new GbVideo(&gm->gb);
    auto* audio = new GbAudio(&gm->gb);
    gm->audio = audio;
    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(video);
    io->registerHandler(audio);
    desc->deleters.push_back([gm, video, audio]() {
        gm->flushSave();
        delete video;
        delete audio;
        delete gm;
    });

    // The joypad: the keyboard, two CBM-style joysticks and a gamepad, each with its own bits
    desc->onGamepad = [gm](int port, uint32_t b) {
        if (port != 0) return false;
        gm->pads[0] = padBits(b);
        gm->applyButtons();
        return true;
    };
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [gm](int port, uint8_t bits) {
        if (port < 0 || port > 1) return;
        uint8_t p = 0;
        if (!(bits & 0x01)) p |= BTN_UP;
        if (!(bits & 0x02)) p |= BTN_DOWN;
        if (!(bits & 0x04)) p |= BTN_LEFT;
        if (!(bits & 0x08)) p |= BTN_RIGHT;
        if (!(bits & 0x10)) p |= BTN_A;
        gm->joy[port] = p;
        gm->applyButtons();
    };
    desc->onKey = [gm](const std::string& name, bool down) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        uint8_t bit = 0;
        if (n == "up") bit = BTN_UP;
        else if (n == "down") bit = BTN_DOWN;
        else if (n == "left") bit = BTN_LEFT;
        else if (n == "right") bit = BTN_RIGHT;
        else if (n == "x") bit = BTN_A;
        else if (n == "z") bit = BTN_B;
        else if (n == "return" || n == "start") bit = BTN_START;
        else if (n == "tab" || n == "left_shift" || n == "right_shift" || n == "select") bit = BTN_SELECT;
        else return false;
        gm->keys = down ? (uint8_t)(gm->keys | bit) : (uint8_t)(gm->keys & ~bit);
        gm->applyButtons();
        return true;
    };
    desc->screenButtons = {
        {"START", {"start"}, "The START button (Return)."},
        {"SELECT", {"select"}, "The SELECT button (Tab)."}};

    // No reset button on a Game Boy: power it off and on (the cartridge's RAM stays)
    desc->onReset = [gm](MachineDescriptor&) { gm->powerOn(); };
    desc->schedulerStep = [gm, cpu](MachineDescriptor&) -> int {
        const int c = cpu->step();
        if (gm->gb.cycles() >= gm->nextHousekeeping) {
            // Sixty times a second: the sound to the host; once a second, the battery RAM to its file
            gm->nextHousekeeping = gm->gb.cycles() + Gameboy::CLOCK_HZ / 60;
            gm->audio->drain();
            if (++gm->ticks >= 60) { gm->ticks = 0; gm->flushSave(); }
        }
        return c;
    };
    gm->insertEmpty();
    gm->powerOn();
    return desc;
}

}  // namespace gb
