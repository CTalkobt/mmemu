#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * A bar code wand (the Model 100/200's bar code reader port: Tandy's
 * 26-1183 wand, a Hewlett-Packard HEDS-3050-type digital wand, MAME's
 * kyocera.cpp notes) swiped across a bar code at a steady speed.
 * mmsim's own code.
 *
 * A bar code is a row of modules (the narrowest bar or space), each a bar
 * (dark) or a space (light). swipe() plays one pass over it, with a quiet
 * zone of 10 modules each side; MODULE_SECONDS a module (1 ms: a 0.25 mm
 * module at 25 cm/s, a brisk hand swipe; change it with setModuleTime).
 * tick() advances time in clocks of the rate given to the constructor.
 *
 * The output (data()): true over a light space or with the wand lifted
 * (an open-collector output pulled up), false over a dark bar. That
 * polarity is unverified (neither MAME nor VirtualT model the wand); the
 * machine feeds data() to port C bit 3 and its inverse to RST 5.5 (MAME's
 * "!RxDB -> RST5.5" note).
 *
 * Encoders (modules, true = bar):
 *  - code39(text): Code 39 with the * start/stop characters added,
 *    wide = 3 narrow, a narrow space between characters; 0-9, A-Z (lower
 *    case is upper-cased), space and - . $ / + %. Unknown characters: empty.
 *  - ean13(digits): EAN-13 from 12 digits (the check digit is added) or
 *    13 (checked: a wrong one gives empty); upca(): UPC-A from 11 or 12.
 *  - fromBits("10110..."): raw modules ('1' or '|' a bar, '0', '.' or ' ' a space).
 */
class BarcodeWand {
public:
    static constexpr double MODULE_SECONDS = 0.001;
    static constexpr int QUIET_MODULES = 10;

    explicit BarcodeWand(double clockHz = 2457600.0) : m_hz(clockHz) {}

    static std::vector<bool> code39(const std::string& text);
    static std::vector<bool> ean13(const std::string& digits);
    static std::vector<bool> upca(const std::string& digits);
    static std::vector<bool> fromBits(const std::string& bits);
    /** "code39:TEXT", "ean13:DIGITS", "upca:DIGITS", "bits:1011..."; empty if not one. */
    static std::vector<bool> parse(const std::string& spec);

    /** Start a pass over `modules` (replacing one in progress). */
    void swipe(const std::vector<bool>& modules);
    void setModuleTime(double seconds) { m_moduleSec = seconds > 0 ? seconds : MODULE_SECONDS; }
    /** A pass is in progress. */
    bool scanning() const { return m_pos < m_modules.size(); }
    /** The wand's output now (true: light / lifted). */
    bool data() const;
    void tick(uint64_t clocks);

    /** The number of passes started (the debugger). */
    int swipes() const { return m_swipes; }

private:
    double m_hz;
    double m_moduleSec = MODULE_SECONDS;
    std::vector<bool> m_modules;
    size_t m_pos = 0;
    double m_inModule = 0;          // seconds into the current module
    int m_swipes = 0;
};
