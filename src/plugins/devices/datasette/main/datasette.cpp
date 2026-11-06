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
    if (m_senseLine) m_senseLine->set(true);  // no button down
    m_recording = false;
    m_lastWriteLevel = true;
    m_writePulseStart = 0;
    m_totalCycles = 0;
    m_readLevel = true;
    m_secondHalf = false;
    m_fullGap = 0;
}

bool Datasette::mount(const std::string& path) {
    bool ok = m_tape.load(path);
    if (ok) {
        m_offset = 0;
        m_pulseRemaining = 0;
        m_secondHalf = false;
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
    m_secondHalf = false;
}

uint32_t Datasette::nextHalfWave() {
    if (m_tape.version() >= 2) return m_tape.nextPulse(m_offset) * 2;   // one half-wave per value
    if (!m_secondHalf) {                        // a full wave: two equal halves
        m_fullGap = m_tape.nextPulse(m_offset);
        if (m_fullGap == 0) return 0;
    }
    m_secondHalf = !m_secondHalf;
    return m_fullGap;
}

bool Datasette::startRecord() {
    if (!m_writeLine) return false;
    m_playing = false;
    m_tape.startRecording();
    m_recording = true;
    m_buttonPressed = true;
    m_lastWriteLevel = m_writeLine->get();
    m_writePulseStart = m_totalCycles;
    m_haveWriteEdge = false;
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

    // Recording: one pulse per cycle of the write line, from one 0->1
    // transition to the next, as VICE records it (datasette.c; the write
    // signal is inverted on its way to the tape, so this is a falling edge
    // there). A TAP file stores these; playback gives one read edge each.
    if (m_recording && m_motorOn && m_writeLine) {
        bool level = m_writeLine->get();
        if (level != m_lastWriteLevel && m_halfWave) {
            // C16 TAP v2: every edge ends a half-wave (stored as cycles / 2:
            // the value is cycles / 16, a long one cycles / 2 in 24 bits)
            // A wave starts on a falling edge of the write line (VICE also
            // measures waves from one to the next); playback begins with the
            // read line going low, so the first half-wave stored is the one
            // after a falling edge
            if (m_haveWriteEdge)
                m_tape.appendPulse((uint32_t)(m_totalCycles - m_writePulseStart) / 2);
            m_writePulseStart = m_totalCycles;
            m_haveWriteEdge = m_haveWriteEdge || !level;
            m_lastWriteLevel = level;
        } else if (level != m_lastWriteLevel) {
            if (level) {
                if (m_haveWriteEdge)
                    m_tape.appendPulse((uint32_t)(m_totalCycles - m_writePulseStart));
                m_writePulseStart = m_totalCycles;
                m_haveWriteEdge = true;
            }
            m_lastWriteLevel = level;
        }
    }

    if (!m_playing || !m_motorOn) return;

    m_pulseRemaining -= (uint32_t)cycles;

    if (m_pulseRemaining <= 0) {
        uint32_t pulse = m_halfWave ? nextHalfWave() : m_tape.nextPulse(m_offset);
        if (pulse == 0) {
            m_playing = false;
            m_buttonPressed = false;
            if (m_senseLine) m_senseLine->set(true);
            return;
        }
        m_pulseRemaining += (int32_t)pulse;

        if (m_halfWave) {                       // Plus/4: the read level flips
            m_readLevel = !m_readLevel;
            if (m_readPulseLine) m_readPulseLine->set(m_readLevel);
        } else if (m_readPulseLine) {           // a negative edge on the CIA's FLAG pin
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
