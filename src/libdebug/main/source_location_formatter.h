#pragma once

#include <string>
#include <cstdint>
#include <memory>

/**
 * Generic representation of a source location with display formatting.
 * Can be rendered differently for CLI, GUI, MCP, etc.
 */
struct FormattedSourceLocation {
    std::string file;
    int line;
    int column = -1;  // optional column number
    uint32_t address = 0;  // optional PC address for context

    // Generic formatted output
    std::string text;  // e.g., "test.c:14" or "test.c:14:5"

    FormattedSourceLocation() : line(-1) {}
    FormattedSourceLocation(const std::string& f, int l) : file(f), line(l) {
        updateText();
    }

    void updateText() {
        if (line < 0) {
            text = "[no source]";
        } else if (column >= 0) {
            text = file + ":" + std::to_string(line) + ":" + std::to_string(column);
        } else {
            text = file + ":" + std::to_string(line);
        }
    }
};

/**
 * Abstract formatter for rendering source locations in different contexts.
 * Implementations: CLI (OSC 8), GUI (clickable), MCP (JSON), etc.
 */
class ISourceLocationFormatter {
public:
    virtual ~ISourceLocationFormatter() = default;

    /**
     * Format a source location for display in this context.
     * Returns formatted string with any hyperlinks/metadata.
     */
    virtual std::string format(const FormattedSourceLocation& loc) = 0;

    /**
     * Format a location with just file and line.
     */
    std::string format(const std::string& file, int line) {
        FormattedSourceLocation loc(file, line);
        return format(loc);
    }

    /**
     * Format a location with address context.
     */
    virtual std::string formatWithAddress(const FormattedSourceLocation& loc) {
        return format(loc);
    }
};

/**
 * CLI formatter: uses OSC 8 terminal hyperlinks for clickable file:line
 * Format: ESC ] 8 ; id ; file://path/to/file ; text ESC \
 */
class CLISourceLocationFormatter : public ISourceLocationFormatter {
public:
    std::string format(const FormattedSourceLocation& loc) override;
    std::string formatWithAddress(const FormattedSourceLocation& loc) override;

private:
    // OSC 8 hyperlink: ESC + bracket 8 + params + URL + text + ESC
    std::string createTerminalHyperlink(const std::string& filePath,
                                       const std::string& displayText);
};

/**
 * GUI formatter: returns structured data for GUI to render as clickable elements.
 * GUI will create buttons/links that trigger file open actions.
 */
class GUISourceLocationFormatter : public ISourceLocationFormatter {
public:
    std::string format(const FormattedSourceLocation& loc) override;
    std::string formatWithAddress(const FormattedSourceLocation& loc) override;

    // For GUI: return JSON or structured format
    std::string toJSON(const FormattedSourceLocation& loc);
};

/**
 * MCP formatter: returns JSON with file/line metadata.
 * MCP clients can render as clickable links or integrate with editors.
 */
class MCPSourceLocationFormatter : public ISourceLocationFormatter {
public:
    std::string format(const FormattedSourceLocation& loc) override;
    std::string formatWithAddress(const FormattedSourceLocation& loc) override;

    // For MCP: return structured JSON
    std::string toJSON(const FormattedSourceLocation& loc);
};

/**
 * Factory for creating the appropriate formatter based on context.
 */
class SourceLocationFormatterFactory {
public:
    enum class Context {
        CLI,   // Terminal with OSC 8 support
        GUI,   // GUI with clickable elements
        MCP,   // JSON for MCP clients
    };

    static std::unique_ptr<ISourceLocationFormatter> create(Context ctx);
};
