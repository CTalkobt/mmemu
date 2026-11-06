#pragma once

// A Commodore DOS on a disk image (D64, D71, D80, D81, D82), read-only:
// LOAD, `$` directories as the drive sends them, OPEN/GET#/INPUT# of PRG,
// SEQ and USR files, the error channel, and direct access: `#` buffer
// channels with U1 (UA) / B-R block reads, U2 (UB) / B-W block writes
// (back to the image file) and B-P (what the C128's boot sector and CP/M
// use). Writing files (SAVE, OPEN ...,W) reports 26,WRITE PROTECT ON, as do
// block writes to an image the host will not let us write.

#include "cbm_dos.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CbmSectorDisk;

class CbmImageDos : public ICbmDos {
public:
    CbmImageDos();
    ~CbmImageDos() override;

    /** Open an image (type from the extension). False if it cannot be read. */
    bool open(const std::string& path);
    const std::string& path() const { return m_path; }
    void setUnit(int unit) { m_unit = unit; }

    // ICbmDos
    int listen(uint8_t a) override;
    int second(uint8_t a) override;
    int ciout(uint8_t a) override;
    int unlsn() override;
    int talk(uint8_t a) override;
    int tksa(uint8_t a) override;
    int acptr(uint8_t* a) override;
    int untlk() override;
    int unit() const override { return m_unit; }
    void reset() override;
    bool active() const override { return m_active; }

    /** The directory as a BASIC program loaded at $0401 (what `LOAD"$",8` reads). */
    std::vector<uint8_t> directory(const std::string& pattern);
    /** CBM wildcard match: `*` ends the comparison, `?` matches one character. */
    static bool matches(const std::string& pattern, const std::string& name);

private:
    struct Channel {
        std::vector<uint8_t> data;
        size_t pos = 0;
        bool open = false;
        // A `#` buffer channel: 256 bytes, the pointer, the last byte to send
        bool buffer = false;
        uint8_t buf[256] = {};
        int ptr = 0, last = 255;
    };
    void blockRead(const std::string& args, bool u1);
    void blockWrite(const std::string& args, bool u2);
    void bufferPointer(const std::string& args);
    void setError(int code, const char* text, int track = 0, int sector = 0);
    void openChannel(int channel, const std::string& spec);
    void command(const std::string& cmd);

    std::unique_ptr<CbmSectorDisk> m_disk;
    std::string m_path;
    int m_unit = 8;
    bool m_listening = false, m_talking = false, m_opening = false, m_active = false;
    int m_channel = 0;
    std::string m_name, m_cmd;
    Channel m_ch[16];
    std::string m_error;
    size_t m_errorPos = 0;
};
