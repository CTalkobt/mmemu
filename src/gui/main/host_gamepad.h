#pragma once

// Host game controllers and joysticks (SDL2), polled from the GUI timer.
// Without SDL2 at build time this finds no controllers.

#include "host_gamepad_map.h"
#include <string>
#include <vector>

class HostGamepads {
public:
    HostGamepads() = default;
    ~HostGamepads();
    HostGamepads(const HostGamepads&) = delete;
    HostGamepads& operator=(const HostGamepads&) = delete;

    /** Start SDL's controller support. False if unavailable. */
    bool init();
    bool available() const { return m_ok; }

    /** A controller added or removed since the last poll. */
    struct Change { bool added; int slot; std::string name; };

    /**
     * Read every controller. buttons[slot] receives PAD_* bits for each
     * slot (a slot keeps its number while its controller stays connected;
     * an empty slot reads 0). Returns plug/unplug changes.
     */
    std::vector<Change> poll(std::vector<uint32_t>& buttons);

    /** Names of the connected controllers, by slot ("" for an empty slot). */
    std::vector<std::string> names() const;

private:
    struct Device;
    std::vector<Device*> m_slots;
    bool m_ok = false;
};
