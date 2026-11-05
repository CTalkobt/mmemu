#include "mmemu_plugin_api.h"
#include "libcore/main/json_machine_loader.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/iport_device.h"
#include "keyboard_matrix_pet.h"
#include <algorithm>

class KbdPetWrapper : public IOHandler, public IKeyboardMatrix {
public:
    explicit KbdPetWrapper(PetKeyboardMatrix::Layout layout)
        : m_layout(layout), m_kbd(layout), m_colPort(&m_kbd), m_rowPort(&m_kbd) {}

    // IOHandler
    const char* name()     const override { return "kbd_pet"; }
    uint32_t    baseAddr() const override { return 0; }
    uint32_t    addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t)  override { return false; }
    void reset() override { m_kbd = PetKeyboardMatrix(m_layout); }
    void tick(uint64_t cycles) override;

    // IKeyboardMatrix
    void keyDown(int /*row*/, int /*col*/) override {}
    void keyUp(int /*row*/, int /*col*/)   override {}
    void clearKeys() override { m_kbd = PetKeyboardMatrix(m_layout); }
    bool pressKeyByName(const std::string& name, bool down) override {
        if (name.empty()) return false;

        // Handle uppercase by shifting
        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
            std::string lower(1, (char)std::tolower(name[0]));
            if (down) { m_kbd.keyDown("left_shift"); m_kbd.keyDown(lower); }
            else      { m_kbd.keyUp(lower); m_kbd.keyUp("left_shift"); }
            return true;
        }

        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "backslash") lower = "leftarrow";
        if (lower == "shift")     lower = "left_shift";

        if (!m_kbd.hasKey(lower)) return false;     // e.g. "restore": the PET has none
        if (down) m_kbd.keyDown(lower);
        else      m_kbd.keyUp(lower);
        return true;
    }
    void enqueueText(const std::string& text) override;
    IPortDevice* getPort(int index) override {
        if (index == 0) return &m_colPort; // column: CPU reads
        if (index == 1) return &m_rowPort; // row:    CPU writes
        return nullptr;
    }

private:
    // Column port: CPU reads column bits from PIA1 Port B
    class ColumnPort : public IPortDevice {
    public:
        explicit ColumnPort(PetKeyboardMatrix* kbd) : m_kbd(kbd) {}
        uint8_t readPort() override       { return m_kbd->readPort(); }
        void    writePort(uint8_t) override {}
        void    setDdr(uint8_t)    override {}
    private:
        PetKeyboardMatrix* m_kbd;
    };

    // Row port: CPU writes row select to PIA1 Port A
    class RowPort : public IPortDevice {
    public:
        explicit RowPort(PetKeyboardMatrix* kbd) : m_kbd(kbd) {}
        uint8_t readPort() override            { return 0xFF; }
        void    writePort(uint8_t val) override { m_kbd->writePort(val); }
        void    setDdr(uint8_t)        override {}
    private:
        PetKeyboardMatrix* m_kbd;
    };

    PetKeyboardMatrix::Layout m_layout;
    PetKeyboardMatrix m_kbd;
    ColumnPort        m_colPort;
    RowPort           m_rowPort;

    struct TypeEvent {
        std::string keyName;
        bool        down;
    };
    std::vector<TypeEvent> m_typeQueue;
    uint64_t               m_typeTimer = 0;
};

static IOHandler* createKbdPetGraphics() {
    return new KbdPetWrapper(PetKeyboardMatrix::Layout::GRAPHICS);
}

void KbdPetWrapper::enqueueText(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        std::string name;
        if (text[i] == '\\' && i + 1 < text.size()) {
            i++;
            switch (text[i]) {
                case 'n': case 'r': name = "return"; break;
                case 't':           name = "space"; break; 
                case '\\':          name = "leftarrow"; break;
                default:            name = std::string(1, text[i]); break;
            }
        } else {
            char c = text[i];
            if (c == '\n' || c == '\r') name = "return";
            else if (c == '\t')         name = "space";
            else if (c == '\\')         name = "leftarrow";
            else {
                // Unshifted letter keys give capitals on a PET (SHIFT gives
                // graphics), so typed capitals are plain key presses.
                if (c >= 'A' && c <= 'Z') name = std::string(1, (char)(c - 'A' + 'a'));
                else {
                    switch (c) {
                        case ' ': name = "space"; break;
                        case '!': name = "exclaim"; break;
                        case '"': name = "dquote"; break;
                        case '#': name = "hash"; break;
                        case '$': name = "dollar"; break;
                        case '%': name = "percent"; break;
                        case '&': name = "ampersand"; break;
                        case '\'': name = "squote"; break;
                        case '(': name = "lparen"; break;
                        case ')': name = "rparen"; break;
                        case '*': name = "asterisk"; break;
                        case '+': name = "plus"; break;
                        case ',': name = "comma"; break;
                        case '-': name = "minus"; break;
                        case '.': name = "period"; break;
                        case '/': name = "slash"; break;
                        case ':': name = "colon"; break;
                        case ';': name = "semicolon"; break;
                        case '<': name = "less"; break;
                        case '=': name = "equal"; break;
                        case '>': name = "greater"; break;
                        case '?': name = "question"; break;
                        case '@': name = "at"; break;
                        case '[': name = "bracket_left"; break;
                        case ']': name = "bracket_right"; break;
                        case '^': name = "uparrow"; break;
                        case '_': name = "leftarrow"; break;
                        default:  name = std::string(1, c); break;
                    }
                }
            }
        }
        if (!name.empty()) {
            m_typeQueue.push_back({name, true});
            m_typeQueue.push_back({name, false});
            m_typeQueue.push_back({"idle", false});
        }
    }
}

void KbdPetWrapper::tick(uint64_t cycles) {
    if (m_typeQueue.empty()) return;
    if (m_typeTimer > cycles) {
        m_typeTimer -= cycles;
        return;
    }
    TypeEvent ev = m_typeQueue.front();
    m_typeQueue.erase(m_typeQueue.begin());
    if (ev.keyName == "idle") {
        m_kbd = PetKeyboardMatrix(m_layout);
    } else {
        pressKeyByName(ev.keyName, ev.down);
    }
    m_typeTimer = 100000;
}

static IOHandler* createKbdPetBusiness() {
    return new KbdPetWrapper(PetKeyboardMatrix::Layout::BUSINESS);
}

static const SimPluginHostAPI* g_host = nullptr;

static const char* s_deps[] = { "6502", "pia6520", "via6522", "crtc6545", "pet-video", "cbm-loader", nullptr };

static DevicePluginInfo s_devices[] = {
    {"kbd_pet_graphics", createKbdPetGraphics},
    {"kbd_pet_business", createKbdPetBusiness}
};

static SimPluginManifest s_manifest = {
    MMEMU_PLUGIN_API_VERSION,
    "pet",
    "Commodore PET Series",
    "1.0.0",
    s_deps,
    nullptr,
    0, nullptr,
    0, nullptr,
    2, s_devices,
    0, nullptr,
    0, nullptr,
    0, nullptr
};

extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host) {
    g_host = host;
    JsonMachineLoader loader;
    loader.loadFile("machines/pet.json");
    return &s_manifest;
}
