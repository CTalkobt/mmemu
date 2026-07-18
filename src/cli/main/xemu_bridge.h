#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

/**
 * XemuBridge: Subprocess communication with xemu-xmega65
 *
 * Launches xemu-xmega65 in headless mode and communicates via
 * the serial monitor protocol (text-based commands).
 *
 * Supports:
 * - Loading programs into memory
 * - Reading/writing memory regions
 * - Capturing audio output
 * - Comparing spectral characteristics with mmsim
 */
class XemuBridge {
public:
    /// Result of a read/write operation
    struct OperationResult {
        bool success = false;
        std::string errorMsg;
        std::vector<uint8_t> data;
    };

    /// Audio capture result
    struct AudioCapture {
        bool success = false;
        std::vector<float> samples;
        float sampleRate = 44100.0f;
        std::string errorMsg;
    };

    /// Create an xemu bridge (doesn't launch yet)
    explicit XemuBridge(const std::string& xemuPath = "/usr/local/bin/xemu-xmega65");
    ~XemuBridge();

    /// Launch xemu in headless mode
    bool launch();

    /// Stop xemu subprocess
    bool shutdown();

    /// Check if xemu is running
    bool isRunning() const;

    /// Load a binary program into memory
    OperationResult loadProgram(const std::string& binPath, uint32_t addr);

    /// Read memory region
    OperationResult readMemory(uint32_t addr, uint32_t size);

    /// Write memory region
    OperationResult writeMemory(uint32_t addr, const std::vector<uint8_t>& data);

    /// Set PC (program counter)
    OperationResult setPC(uint32_t addr);

    /// Step CPU by N cycles
    OperationResult step(uint32_t cycles);

    /// Capture audio for N cycles
    AudioCapture captureAudio(uint32_t cycles);

    /// Compare mmsim memory vs xemu memory at same address
    struct ComparisonResult {
        bool dataMatches = false;
        float spectralError = 0.0f;  // Normalized RMSE if audio
        std::string differences;
    };

    ComparisonResult compareMemory(
        const std::vector<uint8_t>& mmsimData,
        const std::vector<uint8_t>& xemuData
    );

private:
    std::string m_xemuPath;
    int m_processId = -1;
    int m_stdinPipe[2] = {-1, -1};   // Pipe to xemu stdin
    int m_stdoutPipe[2] = {-1, -1};  // Pipe from xemu stdout
    FILE* m_toXemu = nullptr;         // Write file descriptor
    FILE* m_fromXemu = nullptr;       // Read file descriptor

    /// Send command to xemu via serial protocol
    bool sendCommand(const std::string& cmd);

    /// Read response from xemu (blocking until newline or timeout)
    std::string readResponse();

    /// Parse memory response (hex format: "AB CD EF...")
    std::vector<uint8_t> parseMemoryResponse(const std::string& response);
};
