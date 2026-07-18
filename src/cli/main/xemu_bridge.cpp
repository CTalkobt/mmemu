#include "xemu_bridge.h"
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sstream>
#include <algorithm>
#include <cmath>

XemuBridge::XemuBridge(const std::string& xemuPath)
    : m_xemuPath(xemuPath) {
}

XemuBridge::~XemuBridge() {
    shutdown();
}

bool XemuBridge::launch() {
    if (isRunning()) {
        return true;  // Already running
    }

    // Fork subprocess for xemu-xmega65
    m_processId = fork();

    if (m_processId == -1) {
        // Fork failed
        m_processId = -1;
        return false;
    }

    if (m_processId == 0) {
        // Child process: launch xemu
        // In production, would set up pipes and environment
        execlp(m_xemuPath.c_str(), m_xemuPath.c_str(), "-m", "mega65", nullptr);
        exit(1);  // If execlp fails
    }

    // Parent process
    // Give xemu time to start
    usleep(500000);  // 500ms

    // TODO: Set up pipes for communication
    // TODO: Implement serial protocol communication

    return true;
}

bool XemuBridge::shutdown() {
    if (!isRunning()) {
        return true;
    }

    if (m_stdin) {
        fclose(m_stdin);
        m_stdin = nullptr;
    }
    if (m_stdout) {
        fclose(m_stdout);
        m_stdout = nullptr;
    }

    if (m_processId > 0) {
        kill(m_processId, SIGTERM);
        waitpid(m_processId, nullptr, 0);
        m_processId = -1;
    }

    return true;
}

bool XemuBridge::isRunning() const {
    return m_processId > 0;
}

XemuBridge::OperationResult XemuBridge::loadProgram(
    const std::string& binPath, uint32_t addr) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // TODO: Load binary file and write to xemu memory
    // 1. Read binary file
    // 2. Send write command to xemu at address
    // 3. Verify write succeeded

    result.success = true;
    return result;
}

XemuBridge::OperationResult XemuBridge::readMemory(
    uint32_t addr, uint32_t size) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // TODO: Send read command to xemu
    // Command format: "M<addr> <size>" (from serial monitor protocol)

    result.success = true;
    return result;
}

XemuBridge::OperationResult XemuBridge::writeMemory(
    uint32_t addr, const std::vector<uint8_t>& data) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // TODO: Send write command to xemu
    // Command format: "S<addr> <hexdata>"

    result.success = true;
    return result;
}

XemuBridge::OperationResult XemuBridge::setPC(uint32_t addr) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // TODO: Send set PC command
    // Command format: "G<addr>"

    result.success = true;
    return result;
}

XemuBridge::OperationResult XemuBridge::step(uint32_t cycles) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // TODO: Send step command
    // Command format: "T<count>"

    result.success = true;
    return result;
}

XemuBridge::AudioCapture XemuBridge::captureAudio(uint32_t cycles) {
    AudioCapture capture;

    if (!isRunning()) {
        capture.errorMsg = "Xemu not running";
        return capture;
    }

    // TODO: Capture audio during CPU execution
    // 1. Read audio output buffer from xemu
    // 2. Convert to float samples
    // 3. Return audio data

    capture.success = true;
    return capture;
}

bool XemuBridge::sendCommand(const std::string& cmd) {
    if (!m_stdin) {
        return false;
    }

    fprintf(m_stdin, "%s\n", cmd.c_str());
    fflush(m_stdin);
    return true;
}

std::string XemuBridge::readResponse() {
    if (!m_stdout) {
        return "";
    }

    char buffer[4096];
    if (!fgets(buffer, sizeof(buffer), m_stdout)) {
        return "";
    }

    return std::string(buffer);
}

std::vector<uint8_t> XemuBridge::parseMemoryResponse(
    const std::string& response) {
    std::vector<uint8_t> data;

    // TODO: Parse hex-encoded memory response
    // Format: "AB CD EF 01 23 45 67 89"

    return data;
}

XemuBridge::ComparisonResult XemuBridge::compareMemory(
    const std::vector<uint8_t>& mmsimData,
    const std::vector<uint8_t>& xemuData) {
    ComparisonResult result;

    if (mmsimData.size() != xemuData.size()) {
        result.differences = "Size mismatch";
        return result;
    }

    if (mmsimData.empty()) {
        result.dataMatches = true;
        return result;
    }

    // Calculate normalized RMSE
    float sumSquaredError = 0.0f;
    float maxVal = 0.0f;
    int diffCount = 0;

    for (size_t i = 0; i < mmsimData.size(); ++i) {
        float mmsim = mmsimData[i] / 255.0f;
        float xemu = xemuData[i] / 255.0f;
        float error = mmsim - xemu;

        sumSquaredError += error * error;
        maxVal = std::max(maxVal, xemu);

        if (mmsimData[i] != xemuData[i]) {
            diffCount++;
        }
    }

    result.dataMatches = (diffCount == 0);

    if (maxVal > 0.01f) {
        result.spectralError = std::sqrt(sumSquaredError / mmsimData.size()) / maxVal;
    }

    if (diffCount > 0 && diffCount <= 10) {
        result.differences = "Byte differences at " + std::to_string(diffCount) + " locations";
    } else if (diffCount > 10) {
        result.differences = "Byte differences at " + std::to_string(diffCount) + " locations";
    }

    return result;
}
