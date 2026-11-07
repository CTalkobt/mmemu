// Apple II / II+ machine, after AppleWin (GPL-2.0-or-later): Memory.cpp
// (the $C0xx soft switches, the floating bus), LanguageCard.cpp,
// Keyboard.cpp, Speaker.cpp, Joystick.cpp.

#include "machine_apple2.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdebug/main/execution_observer.h"
#include "libmem/main/ibus.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/devices/apple2_video/main/apple2_video.h"
#include "disk2.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace apple2 {

namespace {

// The CPU clock: 14.318181 MHz / 14, with a long cycle every 65 (AppleWin
// CLK_6502_NTSC)
constexpr uint32_t CPU_HZ = 1020484;
constexpr int SAMPLE_RATE = 44100;

bool loadFile(const std::string& path, size_t size, std::vector<uint8_t>& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (f) out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (!f || out.size() != size) {
        if (err) *err = "missing or wrong-size ROM " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Keyboard (Keyboard.cpp): a latch of the last key's ASCII code and a strobe
// (bit 7 of $C000), cleared by $C010. The II and II+ keyboards have upper
// case only. Keys come by name (the frontends' US layout, with Shift and
// Control tracked here) or as typed text.
// ---------------------------------------------------------------------------
class Apple2Keyboard : public IOHandler, public IKeyboardMatrix {
public:
    std::function<void()> onReset;              // the RESET key

    const char* name() const override { return "Keyboard"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { m_strobe = false; m_queue.clear(); m_wait = 0; }

    uint8_t data() const { return (uint8_t)(m_code | (m_strobe ? 0x80 : 0)); }
    void clearStrobe() { m_strobe = false; }

    // Typed text: the next character once the program took the last one
    void tick(uint64_t cycles) override {
        if (m_queue.empty() || m_strobe) return;
        if (m_wait > cycles) { m_wait -= cycles; return; }
        latch(m_queue.front());
        m_queue.pop_front();
        m_wait = 2000;
    }

    bool key(std::string n, bool down) {
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (n == "left_shift" || n == "right_shift" || n == "shift") { m_shift = down; return true; }
        if (n == "control" || n == "ctrl") { m_ctrl = down; return true; }
        if (n == "reset") { if (down && onReset) onReset(); return true; }
        if (!down) return true;
        static const std::map<std::string, uint8_t> special = {
            {"return", 0x0D}, {"enter", 0x0D}, {"left", 0x08}, {"delete", 0x08}, {"backspace", 0x08},
            {"right", 0x15}, {"up", 0x0B}, {"down", 0x0A}, {"run_stop", 0x1B}, {"escape", 0x1B}, {"esc", 0x1B},
            {"tab", 0x09}, {"space", 0x20}};
        static const std::map<std::string, char> punct = {
            {"semicolon", ';'}, {"kpeq", '='}, {"equal", '='}, {"equals", '='}, {"minus", '-'}, {"comma", ','},
            {"period", '.'}, {"slash", '/'}, {"at", '@'}, {"bracket_left", '['}, {"bracket_right", ']'},
            {"squote", '\''}, {"backslash", '\\'}, {"grave", '`'}, {"plus", '+'}, {"asterisk", '*'}};
        int c = -1;
        if (auto it = special.find(n); it != special.end()) c = it->second;
        else if (auto p = punct.find(n); p != punct.end()) c = shifted(p->second);
        else if (n.size() == 1 && std::isalnum((unsigned char)n[0])) c = shifted(n[0]);
        if (c < 0) return false;
        if (m_ctrl && std::isalpha(c)) c &= 0x1F;
        latch((uint8_t)c);
        return true;
    }

    // IKeyboardMatrix (no matrix: names and typed text)
    void keyDown(int, int) override {}
    void keyUp(int, int) override {}
    void clearKeys() override { m_queue.clear(); m_shift = m_ctrl = false; }
    bool pressKeyByName(const std::string& n, bool down) override { return key(n, down); }
    IPortDevice* getPort(int) override { return nullptr; }
    void enqueueText(const std::string& text) override {
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { c = '\r'; ++i; }
            if (c == '\n') c = '\r';
            m_queue.push_back((uint8_t)std::toupper((unsigned char)c) & 0x7F);
        }
    }

private:
    int shifted(char c) const {
        static const char base[] = "1234567890-=;',./[]\\`";
        static const char up[] = "!@#$%^&*()_+:\"<>?{}|~";
        if (m_shift)
            if (const char* p = std::strchr(base, c)) c = up[p - base];
        return std::toupper((unsigned char)c);
    }
    void latch(uint8_t c) { m_code = c & 0x7F; m_strobe = true; }

    uint8_t m_code = 0;
    bool m_strobe = false, m_shift = false, m_ctrl = false;
    std::deque<uint8_t> m_queue;
    uint64_t m_wait = 0;
};

// ---------------------------------------------------------------------------
// Speaker (Speaker.cpp): each access to $C030 flips the cone; the level is
// averaged over each sample period, then a DC-blocking filter (the cone
// rests at either position).
// ---------------------------------------------------------------------------
class Apple2Speaker : public IOHandler, public IAudioOutput {
public:
    const char* name() const override { return "Speaker"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { m_out.clear(); }
    void tick(uint64_t) override {}

    void toggle(uint64_t at) { advance(at); m_level = -m_level; ++m_toggles; }
    void advance(uint64_t to) {
        while (m_time < to) {
            const double room = PER_SAMPLE - m_phase;
            const double step = std::min<double>(room, (double)(to - m_time));
            m_acc += step * m_level;
            m_phase += step;
            m_time += (uint64_t)std::max(1.0, step);
            if (m_phase >= PER_SAMPLE - 1e-9) {
                const float x = (float)(m_acc / PER_SAMPLE) * 0.3f;
                const float y = x - m_prevX + 0.995f * m_prevY;     // DC blocker
                m_prevX = x;
                m_prevY = y;
                m_out.push_back(y);
                if (m_out.size() > SAMPLE_RATE) m_out.pop_front();
                m_acc = 0;
                m_phase -= PER_SAMPLE;
            }
        }
    }
    uint64_t toggles() const { return m_toggles; }

    int nativeSampleRate() const override { return SAMPLE_RATE; }
    int pullSamples(float* buffer, int maxSamples) override {
        int n = 0;
        while (n < maxSamples && !m_out.empty()) { buffer[n++] = m_out.front(); m_out.pop_front(); }
        return n;
    }

private:
    static constexpr double PER_SAMPLE = (double)CPU_HZ / SAMPLE_RATE;
    uint64_t m_time = 0, m_toggles = 0;
    double m_phase = 0, m_acc = 0;
    float m_level = 1.0f, m_prevX = 0, m_prevY = 0;
    std::deque<float> m_out;
};

// The video, as the frontends find it (an I/O device with a picture)
class Apple2Display : public IOHandler, public IVideoOutput {
public:
    explicit Apple2Display(Apple2Video* v) : m_video(v) {}
    const char* name() const override { return "Video"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    VideoDimensions getDimensions() const override { return m_video->getDimensions(); }
    void renderFrame(uint32_t* buffer) override { m_video->renderFrame(buffer); }
private:
    Apple2Video* m_video;
};

// ---------------------------------------------------------------------------
// The bus: 48 KB RAM, the soft switches and the language card
// ---------------------------------------------------------------------------
class Apple2Bus : public IBus {
public:
    Apple2Bus(std::vector<uint8_t> rom, Apple2Video* video, Apple2Keyboard* kbd, Apple2Speaker* spk,
              Disk2Card* disk)
        : m_rom(std::move(rom)), m_video(video), m_kbd(kbd), m_spk(spk), m_disk(disk) {
        m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
        m_ram.assign(0x10000, 0);
        m_lc.assign(0x4000, 0);
        video->setMemory(m_ram.data(), nullptr);
        powerOn();
    }

    void powerOn() {
        // AppleWin fills RAM with a pattern; zeroes are as good for the ROMs
        std::fill(m_ram.begin(), m_ram.end(), 0);
        m_lcReadRam = false; m_lcBank2 = true; m_lcWriteRam = true; m_lcPrewrite = false;
        m_vf = Apple2Video::VF_TEXT;
        m_video->setMode(m_vf);
        m_cycles = m_videoCycle = 0;
        m_annunciators = 0;
    }

    // Instruction framing: accesses are counted so I/O happens on its cycle
    void begin() { m_access = 0; ++m_instr; }
    void end(int c) {
        m_cycles += (uint64_t)c;
        syncVideo(m_cycles);
        m_spk->advance(m_cycles);
    }
    uint64_t cycles() const { return m_cycles; }

    // Paddles (Joystick.cpp): $C070 starts the timers; paddle n reads bit 7
    // set for its position x 11.04 cycles
    void setPaddle(int n, int pos) { m_paddle[n & 3] = std::clamp(pos, 0, 255); }
    void setButton(int n, bool down) { m_button[n % 3] = down; }

    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "system"; }

    uint8_t read8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        const uint64_t at = m_cycles + m_access++;
        uint8_t v;
        if (a < 0xC000) v = m_ram[a];
        else if (a < 0xC100) v = io(a, nullptr, at);
        else if (a >= 0xC600 && a < 0xC700) v = m_disk->rom((uint8_t)a);      // slot 6: the Disk II
        else if (a < 0xD000) { syncVideo(at); v = m_video->floatingBus(); }   // empty slots
        else v = m_lcReadRam ? m_lc[lcIndex(a)] : m_rom[a - 0xD000];
        if (m_observer) m_observer->onMemoryRead(this, a, v);
        return v;
    }
    uint8_t peek8(uint32_t addr) override {
        const uint16_t a = addr & 0xFFFF;
        if (a < 0xC000) return m_ram[a];
        if (a < 0xC010) return m_kbd->data();
        if (a >= 0xC600 && a < 0xC700) return m_disk->rom((uint8_t)a);
        if (a < 0xD000) return m_video->floatingBus();
        return m_lcReadRam ? m_lc[lcIndex(a)] : m_rom[a - 0xD000];
    }
    void write8(uint32_t addr, uint8_t v) override {
        const uint16_t a = addr & 0xFFFF;
        const uint64_t at = m_cycles + m_access++;
        if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
        if (a < 0xC000) m_ram[a] = v;
        else if (a < 0xC100) io(a, &v, at);
        else if (a >= 0xD000 && m_lcWriteRam) m_lc[lcIndex(a)] = v;
    }
    void reset() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}

    uint8_t* ram() { return m_ram.data(); }
    uint32_t videoFlags() const { return m_vf; }
    bool lcReadRam() const { return m_lcReadRam; }
    bool lcWriteRam() const { return m_lcWriteRam; }
    bool lcBank2() const { return m_lcBank2; }

private:
    // $D000-$DFFF: bank 1 at 0, bank 2 at $1000; $E000-$FFFF at $2000
    uint32_t lcIndex(uint16_t a) const {
        if (a >= 0xE000) return 0x2000u + (a - 0xE000u);
        return (m_lcBank2 ? 0x1000u : 0u) + (a - 0xD000u);
    }

    void syncVideo(uint64_t to) {
        if (to > m_videoCycle) { m_video->clock((uint32_t)(to - m_videoCycle)); m_videoCycle = to; }
    }
    uint8_t floating(uint64_t at) { syncVideo(at); return m_video->floatingBus(); }
    void setVideo(uint32_t set, uint32_t clear, uint64_t at) {
        syncVideo(at);
        m_vf = (m_vf | set) & ~clear;
        m_video->setMode(m_vf);
    }

    // $C000-$C0FF (Memory.cpp IORead_C0xx / IOWrite_C0xx, II/II+)
    uint8_t io(uint16_t a, const uint8_t* wr, uint64_t at) {
        switch ((a >> 4) & 0x0F) {
            case 0x0:                                   // keyboard data
                return wr ? 0 : m_kbd->data();
            case 0x1:                                   // clear the strobe
                m_kbd->clearStrobe();
                return wr ? 0 : floating(at);
            case 0x2:                                   // cassette output: not emulated
                return wr ? 0 : floating(at);
            case 0x3:                                   // speaker
                m_spk->toggle(at);
                return wr ? 0 : floating(at);
            case 0x4:
                return wr ? 0 : floating(at);
            case 0x5: {
                const int s = a & 0x0F;
                switch (s) {
                    case 0x0: setVideo(0, Apple2Video::VF_TEXT, at); break;
                    case 0x1: setVideo(Apple2Video::VF_TEXT, 0, at); break;
                    case 0x2: setVideo(0, Apple2Video::VF_MIXED, at); break;
                    case 0x3: setVideo(Apple2Video::VF_MIXED, 0, at); break;
                    case 0x4: setVideo(0, Apple2Video::VF_PAGE2, at); break;
                    case 0x5: setVideo(Apple2Video::VF_PAGE2, 0, at); break;
                    case 0x6: setVideo(0, Apple2Video::VF_HIRES, at); break;
                    case 0x7: setVideo(Apple2Video::VF_HIRES, 0, at); break;
                    default:                            // annunciators 0-3
                        if (s & 1) m_annunciators |= (uint8_t)(1 << ((s - 8) >> 1));
                        else m_annunciators &= (uint8_t)~(1 << ((s - 8) >> 1));
                        break;
                }
                return wr ? 0 : floating(at);
            }
            case 0x6: {                                 // cassette in, buttons, paddles
                if (wr) return 0;
                const uint8_t fb = floating(at) & 0x7F;
                switch (a & 0x07) {
                    case 0x0: return fb;                // TAPEIN: no tape
                    case 0x1: case 0x2: case 0x3: return (uint8_t)(fb | (m_button[(a & 7) - 1] ? 0x80 : 0));
                    default: {
                        const int p = (a & 7) - 4;
                        const bool active = (double)(at - m_ptrig) <= m_paddle[p] * (2816.0 / 255.0);
                        return (uint8_t)(fb | (active ? 0x80 : 0));
                    }
                }
            }
            case 0x7:                                   // paddle timers start
                m_ptrig = at;
                return wr ? 0 : floating(at);
            case 0x8: {                                 // the language card (LanguageCard.cpp IO)
                m_lcBank2 = !(a & 8);
                m_lcReadRam = ((a & 2) >> 1) == (a & 1);
                if (a & 1) {
                    // two reads in a row (UTAIIe:5-23), or a read-modify-write
                    // instruction's write after its read (GH#700)
                    const bool rmw = wr && m_lcLastRead == a && m_lcLastInstr == m_instr;
                    if ((!wr && m_lcPrewrite) || (wr && m_lcPrewrite && rmw)) m_lcWriteRam = true;
                } else {
                    m_lcWriteRam = false;
                }
                m_lcPrewrite = (a & 1) && !wr;
                if (!wr) { m_lcLastRead = a; m_lcLastInstr = m_instr; }
                return wr ? 0 : floating(at);
            }
            case 0xE: {                                 // slot 6: the Disk II
                const int r = m_disk->io(a, wr, at);
                return wr ? 0 : r < 0 ? floating(at) : (uint8_t)r;
            }
            default:                                    // the other slots: empty
                return wr ? 0 : floating(at);
        }
    }

    BusConfig m_cfg;
    std::vector<uint8_t> m_rom, m_ram, m_lc;
    Apple2Video* m_video;
    Apple2Keyboard* m_kbd;
    Apple2Speaker* m_spk;
    Disk2Card* m_disk;
    uint32_t m_vf = Apple2Video::VF_TEXT;
    uint64_t m_cycles = 0, m_videoCycle = 0, m_instr = 0, m_ptrig = 0;
    int m_access = 0;
    bool m_lcReadRam = false, m_lcBank2 = true, m_lcWriteRam = true, m_lcPrewrite = false;
    uint16_t m_lcLastRead = 0;
    uint64_t m_lcLastInstr = ~0ull;
    uint8_t m_annunciators = 0;
    int m_paddle[4] = {127, 127, 127, 127};
    bool m_button[3] = {};
};

}  // namespace

MachineDescriptor* create(Model model, std::string* err) {
    const bool plus = model == Model::IIPlus;
    std::vector<uint8_t> rom, chars, disk2;
    if (!loadFile(plus ? "roms/apple2/Apple2_Plus.rom" : "roms/apple2/Apple2.rom", 0x3000, rom, err)) return nullptr;
    if (!loadFile("roms/apple2/Apple2_Video.rom", 0x800, chars, err)) return nullptr;
    if (!loadFile("roms/apple2/DISK2.rom", 0x100, disk2, err)) return nullptr;

    auto* desc = new MachineDescriptor();
    desc->machineId = plus ? "apple2plus" : "apple2";
    desc->displayName = plus ? "Apple ][+" : "Apple ][";
    desc->licenseClass = "proprietary";
    desc->uiPanesSet = true;
    desc->uiPanes = {"drive"};
    desc->clockHz = CPU_HZ;

    auto* video = new Apple2Video(plus ? Apple2Video::Model::IIPlus : Apple2Video::Model::II);
    video->setCharRom(std::move(chars));
    auto* kbd = new Apple2Keyboard();
    auto* spk = new Apple2Speaker();
    auto* display = new Apple2Display(video);
    auto* disk = new Disk2Card(std::move(disk2));
    auto* bus = new Apple2Bus(std::move(rom), video, kbd, spk, disk);
    desc->buses.push_back({"system", bus});
    auto* cpu = new MOS6502();
    cpu->setDataBus(bus);
    cpu->setCodeBus(bus);
    desc->cpus.push_back({"main", cpu, bus, bus, nullptr, true, 1});

    auto* io = new IORegistry();
    desc->ioRegistry = io;
    io->registerHandler(display);
    io->registerHandler(kbd);
    io->registerHandler(spk);
    io->registerHandler(disk);
    desc->deleters.push_back([video, display, kbd, spk, disk]() {
        delete display; delete video; delete kbd; delete spk; delete disk;
    });

    // The RESET key: the CPU and the cards' RESET line (the Disk II's
    // switches off); the language card keeps its state on a II/II+
    // (AppleWin CtrlReset, LanguageCardManager::Reset)
    kbd->onReset = [cpu, disk]() { disk->reset(false); cpu->reset(); };
    desc->onKey = [kbd](const std::string& n, bool down) { return kbd->key(n, down); };
    desc->screenButtons = {{"RESET", {"reset"}, "The RESET key: the CPU restarts (Applesoft's autostart monitor warm-starts)."}};

    // Joysticks: port 0 drives paddles 0/1 and button 0, port 1 paddles 2/3
    // and button 1 (bits active low: 0 up, 1 down, 2 left, 3 right, 4 fire)
    desc->joystickPorts = 2;
    desc->primaryJoystickPort = 0;
    desc->onJoystick = [bus](int port, uint8_t bits) {
        const int p = (port & 1) * 2;
        bus->setPaddle(p, !(bits & 0x04) ? 0 : !(bits & 0x08) ? 255 : 127);
        bus->setPaddle(p + 1, !(bits & 0x01) ? 0 : !(bits & 0x02) ? 255 : 127);
        bus->setButton(port & 1, !(bits & 0x10));
    };

    desc->onReset = [bus, io, cpu](MachineDescriptor&) {     // power on
        bus->powerOn();
        io->resetAll();
        cpu->reset();
    };
    desc->schedulerStep = [bus, cpu, io, disk](MachineDescriptor&) -> int {
        bus->begin();
        const int c = cpu->step();
        bus->end(c);
        disk->update(bus->cycles(), (uint64_t)c);
        io->tickAll((uint64_t)c);
        return c;
    };
    if (desc->onReset) desc->onReset(*desc);
    return desc;
}

}  // namespace apple2
