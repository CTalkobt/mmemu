#include "host_gamepad.h"

#ifdef MMEMU_HAVE_SDL2
#include <SDL2/SDL.h>

// A device SDL knows as a game controller uses its standard layout; any
// other joystick reads axes 0/1, hat 0 and buttons 0-3 (fire).
struct HostGamepads::Device {
    SDL_GameController* pad = nullptr;
    SDL_Joystick* joy = nullptr;
    SDL_JoystickID id = -1;
    std::string name;
};

HostGamepads::~HostGamepads() {
    for (auto* d : m_slots) {
        if (!d) continue;
        if (d->pad) SDL_GameControllerClose(d->pad);
        else if (d->joy) SDL_JoystickClose(d->joy);
        delete d;
    }
    if (m_ok) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

bool HostGamepads::init() {
    if (m_ok) return true;
    // The GUI window, not SDL, has the focus: keep reading the controllers.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Leave SIGINT/SIGTERM to the GUI (SDL would turn them into an SDL_QUIT event).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    m_ok = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    return m_ok;
}

std::vector<HostGamepads::Change> HostGamepads::poll(std::vector<uint32_t>& buttons) {
    std::vector<Change> changes;
    if (!m_ok) { buttons.clear(); return changes; }
    SDL_PumpEvents();
    SDL_Event ev;
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_JOYDEVICEADDED, SDL_JOYDEVICEREMOVED) > 0) {
        if (ev.type == SDL_JOYDEVICEADDED) {
            int index = ev.jdevice.which;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
            bool known = false;
            for (auto* d : m_slots) if (d && d->id == id) known = true;
            if (known) continue;
            auto* d = new Device();
            if (SDL_IsGameController(index)) {
                d->pad = SDL_GameControllerOpen(index);
                if (d->pad) d->joy = SDL_GameControllerGetJoystick(d->pad);
            } else {
                d->joy = SDL_JoystickOpen(index);
            }
            if (!d->joy) { delete d; continue; }
            d->id = SDL_JoystickInstanceID(d->joy);
            const char* n = d->pad ? SDL_GameControllerName(d->pad) : SDL_JoystickName(d->joy);
            d->name = n ? n : "controller";
            int slot = 0;
            while (slot < (int)m_slots.size() && m_slots[slot]) slot++;
            if (slot == (int)m_slots.size()) m_slots.push_back(nullptr);
            m_slots[slot] = d;
            changes.push_back({true, slot, d->name});
        } else {
            for (int s = 0; s < (int)m_slots.size(); s++) {
                Device* d = m_slots[s];
                if (!d || d->id != ev.jdevice.which) continue;
                changes.push_back({false, s, d->name});
                if (d->pad) SDL_GameControllerClose(d->pad);
                else SDL_JoystickClose(d->joy);
                delete d;
                m_slots[s] = nullptr;
            }
        }
    }
    // Drop the other input events this pump queued; state is read directly.
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    buttons.assign(m_slots.size(), 0);
    for (int s = 0; s < (int)m_slots.size(); s++) {
        Device* d = m_slots[s];
        if (!d) continue;
        HostPadState st;
        if (d->pad) {
            auto btn = [d](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(d->pad, b) != 0; };
            st.up = btn(SDL_CONTROLLER_BUTTON_DPAD_UP);
            st.down = btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            st.left = btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            st.right = btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            st.stickX = SDL_GameControllerGetAxis(d->pad, SDL_CONTROLLER_AXIS_LEFTX);
            st.stickY = SDL_GameControllerGetAxis(d->pad, SDL_CONTROLLER_AXIS_LEFTY);
            st.south = btn(SDL_CONTROLLER_BUTTON_A);
            st.east = btn(SDL_CONTROLLER_BUTTON_B);
            st.west = btn(SDL_CONTROLLER_BUTTON_X);
            st.north = btn(SDL_CONTROLLER_BUTTON_Y);
            st.shoulderL = btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            st.shoulderR = btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
            st.back = btn(SDL_CONTROLLER_BUTTON_BACK);
            st.start = btn(SDL_CONTROLLER_BUTTON_START);
        } else {
            SDL_Joystick* j = d->joy;
            if (SDL_JoystickNumAxes(j) >= 2) {
                st.stickX = SDL_JoystickGetAxis(j, 0);
                st.stickY = SDL_JoystickGetAxis(j, 1);
            }
            if (SDL_JoystickNumHats(j) >= 1) {
                Uint8 h = SDL_JoystickGetHat(j, 0);
                st.up = h & SDL_HAT_UP; st.down = h & SDL_HAT_DOWN;
                st.left = h & SDL_HAT_LEFT; st.right = h & SDL_HAT_RIGHT;
            }
            int nb = SDL_JoystickNumButtons(j);
            auto jb = [j, nb](int b) { return b < nb && SDL_JoystickGetButton(j, b) != 0; };
            st.south = jb(0); st.east = jb(1); st.west = jb(2); st.north = jb(3);
        }
        buttons[s] = hostPadButtons(st);
    }
    return changes;
}

std::vector<std::string> HostGamepads::names() const {
    std::vector<std::string> out;
    for (auto* d : m_slots) out.push_back(d ? d->name : "");
    return out;
}

#else  // no SDL2: no host controllers

struct HostGamepads::Device {};
HostGamepads::~HostGamepads() = default;
bool HostGamepads::init() { return false; }
std::vector<HostGamepads::Change> HostGamepads::poll(std::vector<uint32_t>& buttons) {
    buttons.clear();
    return {};
}
std::vector<std::string> HostGamepads::names() const { return {}; }

#endif
