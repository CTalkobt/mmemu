#include "datasette.h"
#include <iostream>

Datasette::Datasette() {
    Datasette::reset();
}

void Datasette::reset() {
    m_offset = 0;
    m_pulseRemaining = 0;
    m_pulseLevel = true;
    m_playing = false;
    m_motorOn = false;
    m_buttonPressed = false;
    m_recording = false;
    m_lastWriteLevel = true;
    m_writePulseStart = 0;
    m_totalCycles = 0;
}

bool Datasette::mount(const std::string& path) {
    bool ok = m_tape.load(path);
    if (ok) {
        m_offset = 0;
        m_pulseRemaining = 0;
        m_playing = true;
        m_buttonPressed = true;
        if (m_senseLine) m_senseLine->set(false); // Sense low = button pressed
    }
    return ok;
}

void Datasette::play() {
    m_playing = true;
    m_buttonPressed = true;
    if (m_senseLine) m_senseLine->set(false);
}

void Datasette::stop() {
    m_playing = false;
}

void Datasette::rewind() {
    m_offset = 0;
    m_pulseRemaining = 0;
}

bool Datasette::startRecord() {
    if (!m_writeLine) return false;
    m_playing = false;
    m_tape.startRecording();
    m_recording = true;
    m_buttonPressed = true;
    m_lastWriteLevel = m_writeLine->get();
    m_writePulseStart = m_totalCycles;
    if (m_senseLine) m_senseLine->set(false);
    return true;
}

void Datasette::stopRecord() {
    if (!m_recording) return;
    m_tape.stopRecording();
    m_recording = false;
    m_buttonPressed = false;
    if (m_senseLine) m_senseLine->set(true);
}

bool Datasette::saveRecording(const std::string& path) {
    return m_tape.saveRecording(path);
}

void Datasette::tick(uint64_t cycles) {
    m_totalCycles += cycles;

    if (m_motorLine) {
        // Motor control line is active-low (LOW = motor on).
        m_motorOn = !m_motorLine->get();
    }

    // Recording: capture write-line edges
    if (m_recording && m_motorOn && m_writeLine) {
        bool level = m_writeLine->get();
        if (level != m_lastWriteLevel) {
            uint32_t pulseLen = (uint32_t)(m_totalCycles - m_writePulseStart);
            m_tape.appendPulse(pulseLen);
            m_writePulseStart = m_totalCycles;
            m_lastWriteLevel = level;
        }
    }

    if (!m_playing || !m_motorOn) return;

    m_pulseRemaining -= (uint32_t)cycles;

    if (m_pulseRemaining <= 0) {
        uint32_t pulse = m_tape.nextPulse(m_offset);
        if (pulse == 0) {
            m_playing = false;
            m_buttonPressed = false;
            if (m_senseLine) m_senseLine->set(true);
            return;
        }
        m_pulseRemaining += (int32_t)pulse;

        // Trigger negative edge on CIA FLAG pin
        if (m_readPulseLine) {
            m_readPulseLine->set(false);
            m_readPulseLine->set(true);
        }
    }
}

void Datasette::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = 0;
    out.addrMask = 0;

    out.state.push_back({"Button Pressed", m_buttonPressed ? "yes" : "no"});
    out.state.push_back({"Motor On", m_motorOn ? "yes" : "no"});
    out.state.push_back({"Playing", m_playing ? "yes" : "no"});
    out.state.push_back({"Recording", m_recording ? "yes" : "no"});
    
    if (m_buttonPressed) {
        std::string pos = std::to_string(m_offset) + " / " + std::to_string(m_tape.data().size()) + " pulses";
        out.state.push_back({"Tape Position", pos});
    }

    out.dependencies.push_back({"Sense Line", m_senseLine ? "connected" : "none"});
    out.dependencies.push_back({"Motor Line", m_motorLine ? "connected" : "none"});
    out.dependencies.push_back({"Write Line", m_writeLine ? "connected" : "none"});
    out.dependencies.push_back({"Read Pulse Line", m_readPulseLine ? "connected" : "none"});
    
    if (m_motorLine) {
        out.state.push_back({"Motor Line Level", m_motorLine->get() ? "HI" : "LO (active)"});
    }
    if (m_senseLine) {
        out.state.push_back({"Sense Line Level", m_senseLine->get() ? "HI" : "LO (active)"});
    }
}
