#include "xemu_bridge.h"
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <iomanip>

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

    // Create pipes for communication
    if (pipe(m_stdinPipe) == -1 || pipe(m_stdoutPipe) == -1) {
        return false;
    }

    // Fork subprocess for xemu-xmega65
    m_processId = fork();

    if (m_processId == -1) {
        // Fork failed, clean up pipes
        close(m_stdinPipe[0]);
        close(m_stdinPipe[1]);
        close(m_stdoutPipe[0]);
        close(m_stdoutPipe[1]);
        m_processId = -1;
        return false;
    }

    if (m_processId == 0) {
        // Child process: launch xemu
        close(m_stdinPipe[1]);   // Close write end of stdin pipe
        close(m_stdoutPipe[0]);  // Close read end of stdout pipe

        // Redirect stdin/stdout
        dup2(m_stdinPipe[0], STDIN_FILENO);
        dup2(m_stdoutPipe[1], STDOUT_FILENO);

        // Close original descriptors
        close(m_stdinPipe[0]);
        close(m_stdoutPipe[1]);

        // Launch xemu in headless mode
        execlp(m_xemuPath.c_str(), m_xemuPath.c_str(),
               "-m", "mega65", "-no-display", nullptr);
        exit(1);  // If execlp fails
    }

    // Parent process
    close(m_stdinPipe[0]);   // Close read end of stdin pipe
    close(m_stdoutPipe[1]);  // Close write end of stdout pipe

    // Open file descriptors for communication
    m_toXemu = fdopen(m_stdinPipe[1], "w");
    m_fromXemu = fdopen(m_stdoutPipe[0], "r");

    if (!m_toXemu || !m_fromXemu) {
        shutdown();
        return false;
    }

    // Make stdout non-blocking for timeout support
    int flags = fcntl(m_stdoutPipe[0], F_GETFL, 0);
    fcntl(m_stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);

    // Give xemu time to start and report ready
    usleep(1000000);  // 1s startup

    return true;
}

bool XemuBridge::shutdown() {
    if (!isRunning()) {
        return true;
    }

    if (m_toXemu) {
        fclose(m_toXemu);
        m_toXemu = nullptr;
    }
    if (m_fromXemu) {
        fclose(m_fromXemu);
        m_fromXemu = nullptr;
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

    // Read binary file
    FILE* f = fopen(binPath.c_str(), "rb");
    if (!f) {
        result.errorMsg = "Cannot open binary file";
        return result;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> data(size);
    if (fread(data.data(), 1, size, f) != (size_t)size) {
        fclose(f);
        result.errorMsg = "Failed to read binary file";
        return result;
    }
    fclose(f);

    // Write to xemu memory
    return writeMemory(addr, data);
}

XemuBridge::OperationResult XemuBridge::readMemory(
    uint32_t addr, uint32_t size) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // Send read command: "M<addr> <size>"
    std::ostringstream cmd;
    cmd << "M" << std::hex << addr << " " << std::hex << size;

    if (!sendCommand(cmd.str())) {
        result.errorMsg = "Failed to send command";
        return result;
    }

    std::string response = readResponse();
    if (response.empty()) {
        result.errorMsg = "No response from xemu";
        return result;
    }

    result.data = parseMemoryResponse(response);
    result.success = !result.data.empty() || size == 0;

    return result;
}

XemuBridge::OperationResult XemuBridge::writeMemory(
    uint32_t addr, const std::vector<uint8_t>& data) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    if (data.empty()) {
        result.success = true;
        return result;
    }

    // Send write command: "S<addr> <hexdata>"
    std::ostringstream cmd;
    cmd << "S" << std::hex << addr;

    for (uint8_t byte : data) {
        cmd << " " << std::hex << std::setfill('0') << std::setw(2) << (int)byte;
    }

    if (!sendCommand(cmd.str())) {
        result.errorMsg = "Failed to send command";
        return result;
    }

    std::string response = readResponse();
    result.success = (response.find("OK") != std::string::npos);
    if (!result.success) {
        result.errorMsg = "Write failed: " + response;
    }

    return result;
}

XemuBridge::OperationResult XemuBridge::setPC(uint32_t addr) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // Send set PC command: "G<addr>"
    std::ostringstream cmd;
    cmd << "G" << std::hex << addr;

    if (!sendCommand(cmd.str())) {
        result.errorMsg = "Failed to send command";
        return result;
    }

    std::string response = readResponse();
    result.success = (response.find("OK") != std::string::npos);

    return result;
}

XemuBridge::OperationResult XemuBridge::step(uint32_t cycles) {
    OperationResult result;

    if (!isRunning()) {
        result.errorMsg = "Xemu not running";
        return result;
    }

    // Send step command: "T<count>"
    std::ostringstream cmd;
    cmd << "T" << cycles;

    if (!sendCommand(cmd.str())) {
        result.errorMsg = "Failed to send command";
        return result;
    }

    std::string response = readResponse();
    result.success = (response.find("OK") != std::string::npos);

    return result;
}

XemuBridge::AudioCapture XemuBridge::captureAudio(uint32_t cycles) {
    AudioCapture capture;

    if (!isRunning()) {
        capture.errorMsg = "Xemu not running";
        return capture;
    }

    // Step the CPU while audio accumulates
    OperationResult stepResult = step(cycles);
    if (!stepResult.success) {
        capture.errorMsg = "Step failed: " + stepResult.errorMsg;
        return capture;
    }

    // TODO: Read audio buffer from xemu
    // This requires xemu to expose audio via memory-mapped region or serial protocol

    capture.success = true;
    return capture;
}

bool XemuBridge::sendCommand(const std::string& cmd) {
    if (!m_toXemu) {
        return false;
    }

    fprintf(m_toXemu, "%s\n", cmd.c_str());
    fflush(m_toXemu);
    return true;
}

std::string XemuBridge::readResponse() {
    if (!m_fromXemu) {
        return "";
    }

    char buffer[4096] = {0};
    int attempts = 0;
    const int maxAttempts = 100;  // ~1 second timeout with 10ms waits

    // Read with timeout (non-blocking)
    while (attempts < maxAttempts) {
        int result = fscanf(m_fromXemu, "%4095[^\n]\n", buffer);

        if (result == 1) {
            return std::string(buffer);
        }

        // Wait and retry
        usleep(10000);  // 10ms
        attempts++;
    }

    return "";
}

std::vector<uint8_t> XemuBridge::parseMemoryResponse(
    const std::string& response) {
    std::vector<uint8_t> data;

    // Parse hex-encoded memory response
    // Format: "AB CD EF 01 23 45 67 89"
    std::istringstream ss(response);
    std::string hexByte;

    while (ss >> hexByte) {
        if (hexByte.length() == 2) {
            try {
                uint8_t byte = static_cast<uint8_t>(std::stoi(hexByte, nullptr, 16));
                data.push_back(byte);
            } catch (...) {
                // Invalid hex, skip
            }
        }
    }

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
