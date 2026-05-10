#include "ca45_assembler.h"
#include "../../../libcore/main/sim_config.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>

Ca45AssemblerBackend::Ca45AssemblerBackend() = default;

bool Ca45AssemblerBackend::isaSupported(const std::string& isa) const {
    return isa == "45GS02" || isa == "6502" || isa == "65CE02";
}

AssemblerResult Ca45AssemblerBackend::assemble(const std::string& sourcePath,
                                               const std::string& outputPath) {
    AssemblerResult result;
    result.success = false;
    result.errorCount = 0;
    result.warningCount = 0;

    // Determine ca45 command
    std::string command = m_config.binaryPath;
    if (command.empty()) {
        command = "ca45";  // Default to PATH
    }

    // Build full command: ca45 <sourcePath> -o <outputPath>
    std::string fullCmd = command + " \"" + sourcePath + "\" -o \"" + outputPath + "\"";
    if (!m_config.extraOptions.empty()) {
        fullCmd += " " + m_config.extraOptions;
    }

    spdlog::debug("Executing ca45 assembler: {}", fullCmd);

    // Execute the command and capture stderr
    std::string stderrCapture = outputPath + ".ca45_stderr";
    std::string fullCmdWithErr = fullCmd + " 2>\"" + stderrCapture + "\"";

    int exitCode = std::system(fullCmdWithErr.c_str());

    // Read stderr to extract errors
    std::ifstream stderrFile(stderrCapture);
    std::string errorLine;
    while (std::getline(stderrFile, errorLine)) {
        if (errorLine.find("error:") != std::string::npos ||
            errorLine.find("Error") != std::string::npos) {
            result.errorCount++;
            result.errorMessage += errorLine + "\n";
        } else if (errorLine.find("warning:") != std::string::npos ||
                   errorLine.find("Warning") != std::string::npos) {
            result.warningCount++;
        }
    }
    stderrFile.close();
    std::remove(stderrCapture.c_str());

    if (exitCode != 0) {
        spdlog::error("ca45 assembler failed with exit code {}", exitCode);
        result.errorMessage = "ca45 assembler failed (exit code " + std::to_string(exitCode) + ")";
        result.errorCount = std::max(1, result.errorCount);
        return result;
    }

    // Check if output file was created
    std::ifstream outFile(outputPath);
    if (!outFile.good()) {
        spdlog::error("ca45 did not produce output file: {}", outputPath);
        result.errorMessage = "ca45 did not produce output file";
        result.errorCount = 1;
        return result;
    }

    // SUCCESS
    result.success = true;
    result.outputPath = outputPath;
    result.symPath = "";     // ca45 doesn't produce .sym by default yet
    result.listPath = "";    // ca45 doesn't produce .lst by default
    spdlog::info("ca45 assembly successful: {}", outputPath);

    return result;
}
