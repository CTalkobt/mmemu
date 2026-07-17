#include "hardware_test_bridge.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

// Platform-specific includes
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <fcntl.h>
#endif

// ============================================================================
// HardwareTestBridge - Base class
// ============================================================================

HardwareTestBridge::HardwareTestBridge(Mode mode)
    : m_mode(mode), m_connected(false), m_cycles(0) {}

HardwareTestBridge::~HardwareTestBridge() {}

std::unique_ptr<HardwareTestBridge> HardwareTestBridge::connectEmulator(
    const std::string& host, uint16_t port) {
    auto bridge = std::make_unique<EmulatorTestBridge>(host, port);
    if (bridge->connect()) {
        return bridge;
    }
    return nullptr;
}

std::unique_ptr<HardwareTestBridge> HardwareTestBridge::connectHardware(
    const std::string& portPath, uint32_t baudRate) {
    auto bridge = std::make_unique<HardwarePortBridge>(portPath, baudRate);
    if (bridge->connect()) {
        return bridge;
    }
    return nullptr;
}

bool HardwareTestBridge::loadMemory(uint32_t addr, const std::vector<uint8_t>& data) {
    if (!m_connected) {
        spdlog::error("Hardware bridge not connected");
        return false;
    }

    // Use S command for each byte (S addr value format)
    for (size_t i = 0; i < data.size(); ++i) {
        std::ostringstream oss;
        oss << std::hex << "S " << (addr + i) << " " << (int)data[i];
        std::string response = sendCommand(oss.str());
        if (response.find("OK") == std::string::npos) {
            spdlog::error("Failed to write byte at ${:06X}: {}", addr + i, response);
            return false;
        }
    }

    return true;
}

bool HardwareTestBridge::loadMemoryFile(uint32_t addr, const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("Failed to open file: {}", filePath);
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    file.close();

    // Store program path for xemu execution
    m_currentProgramPath = filePath;

    return loadMemory(addr, data);
}

std::vector<uint8_t> HardwareTestBridge::readMemory(uint32_t addr, uint32_t size) {
    std::vector<uint8_t> result;
    if (!m_connected) {
        spdlog::error("Hardware bridge not connected");
        return result;
    }

    // Read in chunks to avoid overwhelming the device
    const uint32_t CHUNK_SIZE = 64;
    for (uint32_t offset = 0; offset < size; offset += CHUNK_SIZE) {
        uint32_t readSize = std::min(CHUNK_SIZE, size - offset);
        std::ostringstream oss;
        oss << std::hex << "X " << (addr + offset) << " " << readSize;

        std::string response = sendCommand(oss.str());
        // Parse hex bytes from response (format: "XX XX XX ...")
        std::istringstream iss(response);
        std::string byte;
        while (iss >> byte) {
            try {
                uint8_t val = std::stoi(byte, nullptr, 16);
                result.push_back(val);
            } catch (...) {
                // Skip non-hex values
            }
        }
    }

    return result;
}

bool HardwareTestBridge::writeMemory(uint32_t addr, uint8_t value) {
    return loadMemory(addr, std::vector<uint8_t>{value});
}

bool HardwareTestBridge::setPC(uint32_t addr) {
    if (!m_connected) {
        spdlog::error("Hardware bridge not connected");
        return false;
    }

    std::ostringstream oss;
    oss << std::hex << "G " << addr;
    std::string response = sendCommand(oss.str());
    return response.find("OK") != std::string::npos;
}

uint32_t HardwareTestBridge::readRegister(const std::string& regName) {
    if (!m_connected) {
        spdlog::error("Hardware bridge not connected");
        return 0;
    }

    std::string response = sendCommand("R");
    return parseRegisterValue(response, regName);
}

bool HardwareTestBridge::step(int count) {
    if (!m_connected) {
        spdlog::error("Hardware bridge not connected");
        return false;
    }

    for (int i = 0; i < count; ++i) {
        std::ostringstream cmd;
        cmd << "N " << std::hex << 1;  // Step 1 instruction
        std::string response = sendCommand(cmd.str());
        if (response.find("OK") == std::string::npos) {
            return false;
        }
    }
    return true;
}

bool HardwareTestBridge::run() {
    if (!m_connected) {
        spdlog::error("Hardware bridge not connected");
        return false;
    }

    std::string response = sendCommand("G");
    return response.find("OK") != std::string::npos;
}

HardwareTestBridge::TestResult HardwareTestBridge::runTest(
    uint32_t programAddr,
    uint32_t resultAddr,
    uint32_t resultSize,
    uint32_t timeoutMs) {

    TestResult result;
    result.success = false;

    // Set PC to program start
    if (!setPC(programAddr)) {
        result.error = "Failed to set PC";
        return result;
    }

    // Clear serial output buffer
    clearSerialOutput();

    // Run until breakpoint (or timeout)
    auto startTime = std::chrono::high_resolution_clock::now();
    while (true) {
        if (!step(1)) {
            result.error = "Step failed";
            return result;
        }

        // Check timeout
        if (timeoutMs > 0) {
            auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeoutMs) {
                result.error = "Test timeout";
                return result;
            }
        }

        // TODO: Check for completion signal (serial write, PC at specific address, etc.)
        // For now, assume test completes after running
        break;
    }

    // Capture execution cycles (emulator only)
    result.executionCycles = m_cycles;

    // Read result memory region
    result.memorySnapshot = readMemory(resultAddr, resultSize);
    if (result.memorySnapshot.size() != resultSize) {
        result.error = "Failed to read complete result memory";
        return result;
    }

    result.output = m_serialOutput;
    result.success = true;
    return result;
}

uint32_t HardwareTestBridge::parseHexValue(const std::string& response) {
    try {
        return std::stoi(response, nullptr, 16);
    } catch (...) {
        return 0;
    }
}

uint32_t HardwareTestBridge::parseRegisterValue(
    const std::string& response, const std::string& regName) {
    // Response format: "A:XX X:XX Y:XX PC:XXXX SP:XX SR:XX"
    size_t pos = response.find(regName + ":");
    if (pos == std::string::npos) {
        return 0;
    }

    pos += regName.length() + 1;
    std::string hexStr;
    while (pos < response.length() && std::isxdigit(response[pos])) {
        hexStr += response[pos];
        ++pos;
    }

    try {
        return std::stoi(hexStr, nullptr, 16);
    } catch (...) {
        return 0;
    }
}

// ============================================================================
// EmulatorTestBridge - TCP connection to SerialMonitorServer
// ============================================================================

EmulatorTestBridge::EmulatorTestBridge(const std::string& host, uint16_t port)
    : HardwareTestBridge(Mode::EMULATOR), m_host(host), m_port(port), m_socket(-1) {}

EmulatorTestBridge::~EmulatorTestBridge() {
    if (m_socket >= 0) {
#ifdef _WIN32
        closesocket(m_socket);
#else
        close(m_socket);
#endif
    }
}

bool EmulatorTestBridge::connect() {
    struct sockaddr_in serverAddr;
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        spdlog::error("Failed to create socket");
        return false;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_host.c_str(), &serverAddr.sin_addr) <= 0) {
        spdlog::error("Invalid address: {}", m_host);
        return false;
    }

    if (::connect(m_socket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        spdlog::error("Failed to connect to {}:{}", m_host, m_port);
        return false;
    }

    m_connected = true;
    spdlog::info("Connected to emulator at {}:{}", m_host, m_port);
    return true;
}

bool EmulatorTestBridge::sendRaw(const std::string& data) {
    if (::send(m_socket, data.c_str(), data.length(), 0) < 0) {
        spdlog::error("Failed to send command");
        return false;
    }
    return true;
}

std::string EmulatorTestBridge::receiveResponse() {
    char buffer[4096] = {0};
    int bytes = recv(m_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes < 0) {
        spdlog::error("Failed to receive response");
        return "";
    }
    buffer[bytes] = '\0';

    std::string response(buffer);
    m_serialOutput += response;
    return response;
}

std::string EmulatorTestBridge::sendCommand(const std::string& cmd) {
    if (!sendRaw(cmd + "\n")) {
        return "";
    }
    return receiveResponse();
}

// ============================================================================
// HardwarePortBridge - Direct serial port connection
// ============================================================================

HardwarePortBridge::HardwarePortBridge(const std::string& portPath, uint32_t baudRate)
    : HardwareTestBridge(Mode::HARDWARE), m_portPath(portPath), m_baudRate(baudRate),
      m_serialFd(-1) {}

HardwarePortBridge::~HardwarePortBridge() {
    closePort();
}

bool HardwarePortBridge::connect() {
    if (!openPort()) {
        return false;
    }

    m_connected = true;
    spdlog::info("Connected to hardware at {} ({}bps)", m_portPath, m_baudRate);
    return true;
}

bool HardwarePortBridge::openPort() {
#ifdef _WIN32
    // Windows serial port implementation would go here
    spdlog::error("Windows serial port not yet implemented");
    return false;
#else
    m_serialFd = ::open(m_portPath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_serialFd < 0) {
        spdlog::error("Failed to open serial port: {}", m_portPath);
        return false;
    }

    struct termios tty;
    if (tcgetattr(m_serialFd, &tty) != 0) {
        spdlog::error("Failed to get serial port attributes");
        closePort();
        return false;
    }

    // Set baud rate
    speed_t speed;
    switch (m_baudRate) {
        case 9600:     speed = B9600; break;
        case 19200:    speed = B19200; break;
        case 38400:    speed = B38400; break;
        case 115200:   speed = B115200; break;
        case 2000000:  speed = B2000000; break;  // Custom high speed
        default:
            spdlog::error("Unsupported baud rate: {}", m_baudRate);
            closePort();
            return false;
    }

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    // 8N1 (8 bits, no parity, 1 stop bit)
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= CREAD | CLOCAL;

    // Disable flow control
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_lflag = 0;
    tty.c_oflag = 0;

    tty.c_cc[VMIN] = 0;   // Non-blocking read
    tty.c_cc[VTIME] = 10; // 1-second timeout

    if (tcsetattr(m_serialFd, TCSANOW, &tty) != 0) {
        spdlog::error("Failed to set serial port attributes");
        closePort();
        return false;
    }

    // Flush buffers
    tcflush(m_serialFd, TCIOFLUSH);

    return true;
#endif
}

void HardwarePortBridge::closePort() {
    if (m_serialFd >= 0) {
#ifdef _WIN32
        // Windows cleanup
#else
        close(m_serialFd);
#endif
        m_serialFd = -1;
    }
}

bool HardwarePortBridge::sendRaw(const std::string& data) {
    if (m_serialFd < 0) {
        return false;
    }

#ifdef _WIN32
    // Windows write would go here
    return false;
#else
    ssize_t written = ::write(m_serialFd, data.c_str(), data.length());
    if (written < 0 || (size_t)written != data.length()) {
        spdlog::error("Failed to write to serial port");
        return false;
    }
    return true;
#endif
}

std::string HardwarePortBridge::receiveResponse() {
    if (m_serialFd < 0) {
        return "";
    }

    std::string response;
    char buffer[1024];
    ssize_t bytes;

    // Read with timeout for response
    auto endTime = std::chrono::high_resolution_clock::now() +
                   std::chrono::milliseconds(500);
    while (std::chrono::high_resolution_clock::now() < endTime) {
#ifdef _WIN32
        // Windows read would go here
        break;
#else
        bytes = ::read(m_serialFd, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            response += buffer;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
#endif
    }

    m_serialOutput += response;
    return response;
}

std::string HardwarePortBridge::sendCommand(const std::string& cmd) {
    if (!sendRaw(cmd + "\n")) {
        return "";
    }
    return receiveResponse();
}

// ============================================================================
// XemuTestBridge - Subprocess-based xemu-xmega65 integration with TCP serial
// ============================================================================

XemuTestBridge::XemuTestBridge(const std::string& xemuPath)
    : HardwareTestBridge(Mode::HARDWARE), m_xemuPath(xemuPath) {}

XemuTestBridge::~XemuTestBridge() {
    cleanupXemu();
}

bool XemuTestBridge::connect() {
    // Verify xemu binary exists
    std::ifstream xemu_check(m_xemuPath);
    if (!xemu_check.good()) {
        spdlog::error("xemu-xmega65 not found at: {}", m_xemuPath);
        spdlog::error("Install with: apt-get install xemu-xmega65");
        return false;
    }

    m_connected = true;
    spdlog::info("Connected to xemu-xmega65 at {}", m_xemuPath);
    spdlog::info("Will attempt TCP serial integration when running tests");
    return true;
}

bool XemuTestBridge::loadMemoryFile(uint32_t addr, const std::string& filePath) {
    // For xemu, we defer program loading until runTest is called
    // because we need to start xemu with special flags first.
    // Just store the path here.
    m_currentProgramPath = filePath;
    spdlog::info("[Xemu] Program stored: {} (will load during test execution)", filePath);
    return true;
}

bool XemuTestBridge::startXemuWithTcpSerial(uint16_t port) {
    // Start xemu with -serialtcp flag
    std::vector<std::string> cmd = {
        m_xemuPath,
        "-headless", "-sleepless",
        "-gui", "none",
        "-serialtcp", "127.0.0.1:" + std::to_string(port),
        "-testing", "-prgexit"
    };

    // Convert to C-style argv
    std::vector<const char*> argv;
    for (const auto& arg : cmd) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    // Fork xemu
    m_xemuPid = fork();
    if (m_xemuPid == 0) {
        // Child: run xemu
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        exit(1);
    } else if (m_xemuPid < 0) {
        spdlog::error("Failed to fork xemu process");
        return false;
    }

    // Give xemu time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Try to connect
    return connectTcpSerial(port, 2000);
}

bool XemuTestBridge::connectTcpSerial(uint16_t port, int timeoutMs) {
    struct sockaddr_in addr;
    m_tcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_tcpSocket < 0) {
        spdlog::error("Failed to create socket");
        return false;
    }

    // Set non-blocking for timeout
    int flags = fcntl(m_tcpSocket, F_GETFL, 0);
    fcntl(m_tcpSocket, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    auto startTime = std::chrono::high_resolution_clock::now();
    while (true) {
        int result = ::connect(m_tcpSocket, (struct sockaddr*)&addr, sizeof(addr));
        if (result == 0) {
            // Set back to blocking
            fcntl(m_tcpSocket, F_SETFL, flags);
            spdlog::info("Successfully connected to xemu TCP serial on port {}", port);
            m_serialPort = port;
            m_useTcpSerial = true;
            return true;
        }

        auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeoutMs) {
            spdlog::warn("Timeout connecting to xemu TCP serial (xemu may lack -serialtcp support)");
            close(m_tcpSocket);
            m_tcpSocket = -1;
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

std::string XemuTestBridge::sendCommandViaTcp(const std::string& cmd) {
    if (m_tcpSocket < 0 || !m_useTcpSerial) {
        return "";
    }

    // Send command
    std::string fullCmd = cmd + "\n";
    ssize_t written = ::write(m_tcpSocket, fullCmd.c_str(), fullCmd.length());
    if (written < 0) {
        spdlog::error("Failed to write to xemu TCP socket");
        m_useTcpSerial = false;
        close(m_tcpSocket);
        m_tcpSocket = -1;
        return "";
    }

    // Read response with timeout
    std::string response;
    char buffer[256];
    auto endTime = std::chrono::high_resolution_clock::now() +
                   std::chrono::milliseconds(500);

    while (std::chrono::high_resolution_clock::now() < endTime) {
        ssize_t bytes = ::read(m_tcpSocket, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            response += buffer;
            if (response.find("OK") != std::string::npos) {
                break;
            }
        } else if (bytes < 0 && errno != EAGAIN) {
            m_useTcpSerial = false;
            close(m_tcpSocket);
            m_tcpSocket = -1;
            return "";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return response;
}

std::string XemuTestBridge::sendCommand(const std::string& cmd) {
    if (m_useTcpSerial) {
        return sendCommandViaTcp(cmd);
    }
    return "";
}

void XemuTestBridge::cleanupXemu() {
    if (m_tcpSocket >= 0) {
        close(m_tcpSocket);
        m_tcpSocket = -1;
    }

    if (m_xemuPid > 0) {
        kill(m_xemuPid, SIGTERM);
        int status;
        waitpid(m_xemuPid, &status, 0);
        m_xemuPid = -1;
    }
}

HardwareTestBridge::TestResult XemuTestBridge::runTest(
    uint32_t programAddr, uint32_t resultAddr, uint32_t resultSize, uint32_t timeoutMs) {

    TestResult result;
    result.success = false;

    if (!m_connected) {
        result.error = "Xemu not connected";
        return result;
    }

    if (m_currentProgramPath.empty()) {
        result.error = "No program loaded (call loadMemoryFile first)";
        return result;
    }

    // Use file-based execution: xemu has built-in -dumpmem support
    // This is the most reliable method for headless operation
    spdlog::info("[Xemu] Running test with file-based method");
    result.memorySnapshot = runXemuAndCapture(
        m_currentProgramPath, resultAddr, resultSize, timeoutMs);

    if (!result.memorySnapshot.empty()) {
        result.success = true;
        result.executionCycles = 0;  // Xemu doesn't report cycle count
        spdlog::info("[Xemu] ✓ Test completed successfully!");
        return result;
    }

    result.error = "Failed to run test on xemu";
    return result;
}

std::vector<uint8_t> XemuTestBridge::runXemuAndCapture(
    const std::string& programPath, uint32_t resultAddr, uint32_t resultSize,
    uint32_t timeoutMs) {

    std::string dumpFile = "/tmp/xemu_dump.bin";
    if (std::ifstream(dumpFile).good()) {
        std::remove(dumpFile.c_str());
    }

    // Build xemu command line
    std::vector<std::string> cmd = {
        m_xemuPath,
        "-headless", "-sleepless", "-testing",
        "-gui", "none",
        "-prg", programPath,
        "-autoload",
        "-prgexit",
        "-dumpmem", dumpFile
    };

    // Convert to C-style argv
    std::vector<const char*> argv;
    for (const auto& arg : cmd) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    // Launch xemu
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: run xemu
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        exit(1);
    } else if (pid < 0) {
        spdlog::error("Failed to fork xemu process");
        return {};
    }

    // Parent process: wait for completion with timeout
    auto startTime = std::chrono::high_resolution_clock::now();
    bool timedOut = false;
    while (true) {
        auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeoutMs) {
            spdlog::warn("[Xemu] Timeout after {}ms, terminating", timeoutMs);
            kill(pid, SIGTERM);
            timedOut = true;
            break;
        }

        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            break;  // Process exited normally
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Wait a bit longer for xemu to flush files
    if (timedOut) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Read memory dump
    std::vector<uint8_t> result;
    std::ifstream dump(dumpFile, std::ios::binary);
    if (!dump.is_open()) {
        spdlog::error("[Xemu] Failed to open memory dump file: {}", dumpFile);
        return result;
    }

    // Check file size
    dump.seekg(0, std::ios::end);
    size_t fileSize = dump.tellg();
    spdlog::info("[Xemu] Memory dump file size: {} bytes", fileSize);

    // Seek to result address
    if (resultAddr >= fileSize) {
        spdlog::error("[Xemu] Result address ${:X} is beyond dump file size {}", resultAddr, fileSize);
        return result;
    }

    dump.seekg(resultAddr);
    std::vector<uint8_t> buffer(resultSize, 0);
    dump.read(reinterpret_cast<char*>(buffer.data()), resultSize);
    size_t bytesRead = dump.gcount();

    if (bytesRead == 0) {
        spdlog::error("[Xemu] Failed to read from dump file at ${:X}", resultAddr);
        return result;
    }

    spdlog::info("[Xemu] Read {} bytes from ${:X}", bytesRead, resultAddr);
    result = buffer;

    // Cleanup
    std::remove(dumpFile.c_str());

    return result;
}

