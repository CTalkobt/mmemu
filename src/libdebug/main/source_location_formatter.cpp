#include "source_location_formatter.h"
#include <sstream>
#include <iomanip>
#include <filesystem>

// CLI Formatter: Terminal hyperlinks using OSC 8
std::string CLISourceLocationFormatter::format(const FormattedSourceLocation& loc) {
    if (loc.line < 0 || loc.file.empty()) {
        return "[no source]";
    }
    return createTerminalHyperlink(loc.file, loc.text);
}

std::string CLISourceLocationFormatter::formatWithAddress(const FormattedSourceLocation& loc) {
    if (loc.line < 0 || loc.file.empty()) {
        return "[no source]";
    }
    std::string text = loc.text;
    if (loc.address > 0) {
        std::ostringstream oss;
        oss << text << " (0x" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << loc.address << ")";
        text = oss.str();
    }
    return createTerminalHyperlink(loc.file, text);
}

std::string CLISourceLocationFormatter::createTerminalHyperlink(const std::string& filePath,
                                                                const std::string& displayText) {
    // OSC 8 hyperlink format:
    // ESC ] 8 ; params ; file://path ; text ESC \
    // Using \x1b ] 8 ; ; file://<path> ; <text> \x1b \

    std::string absPath = filePath;
    // Convert to absolute path if relative
    try {
        absPath = std::filesystem::absolute(filePath).string();
    } catch (...) {
        // If conversion fails, use the original path
        absPath = filePath;
    }

    // Build file:// URL
    std::string fileUrl = "file://" + absPath;

    // OSC 8 escape sequence
    // \x1b is ESC, ] starts OSC, 8 is hyperlink, ; separates params
    // ST (String Terminator) is \x1b\\ or \a
    std::string hyperlink = "\x1b]8;;";
    hyperlink += fileUrl;
    hyperlink += "\x1b\\";  // ST (ESC \)
    hyperlink += displayText;
    hyperlink += "\x1b]8;;\x1b\\";  // Close hyperlink

    return hyperlink;
}

// GUI Formatter: Structured data for GUI rendering
std::string GUISourceLocationFormatter::format(const FormattedSourceLocation& loc) {
    // For GUI display, just return the text - the GUI will handle it as a clickable element
    return loc.text;
}

std::string GUISourceLocationFormatter::formatWithAddress(const FormattedSourceLocation& loc) {
    if (loc.line < 0) {
        return "[no source]";
    }

    std::ostringstream oss;
    oss << loc.text;
    if (loc.address > 0) {
        oss << " (0x" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << loc.address << ")";
    }
    return oss.str();
}

std::string GUISourceLocationFormatter::toJSON(const FormattedSourceLocation& loc) {
    std::ostringstream oss;
    oss << "{"
        << "\"file\":\"" << loc.file << "\","
        << "\"line\":" << loc.line;

    if (loc.column >= 0) {
        oss << ",\"column\":" << loc.column;
    }
    if (loc.address > 0) {
        oss << ",\"address\":\"0x" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << loc.address << "\"";
    }
    oss << "}";

    return oss.str();
}

// MCP Formatter: JSON structure for MCP clients
std::string MCPSourceLocationFormatter::format(const FormattedSourceLocation& loc) {
    // For MCP, return the structured JSON representation
    return toJSON(loc);
}

std::string MCPSourceLocationFormatter::formatWithAddress(const FormattedSourceLocation& loc) {
    // MCP always returns JSON with all available data
    return toJSON(loc);
}

std::string MCPSourceLocationFormatter::toJSON(const FormattedSourceLocation& loc) {
    std::ostringstream oss;
    oss << "{"
        << "\"display\":\"" << loc.text << "\","
        << "\"file\":\"" << loc.file << "\","
        << "\"line\":" << loc.line;

    if (loc.column >= 0) {
        oss << ",\"column\":" << loc.column;
    }
    if (loc.address > 0) {
        oss << ",\"address\":\"0x" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << loc.address << "\"";
    }
    oss << ",\"clickable\":true";
    oss << "}";

    return oss.str();
}

// Factory implementation
std::unique_ptr<ISourceLocationFormatter> SourceLocationFormatterFactory::create(Context ctx) {
    switch (ctx) {
        case Context::CLI:
            return std::make_unique<CLISourceLocationFormatter>();
        case Context::GUI:
            return std::make_unique<GUISourceLocationFormatter>();
        case Context::MCP:
            return std::make_unique<MCPSourceLocationFormatter>();
        default:
            return std::make_unique<CLISourceLocationFormatter>();
    }
}
