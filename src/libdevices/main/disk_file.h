#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Disk image files, plain or compressed (#233): every disk loader reads and
 * writes its image through these, so `game.d64.gz` or `game.zip` holding a
 * `.dsk` mounts like the plain file, after AppleWin's CheckGZipFile and
 * CheckZipFile (DiskImageHelper.cpp).
 *
 *  - `.gz`: one gzip stream; the image's format comes from the inner name
 *    (`x.d64.gz` is a `.d64`).
 *  - `.zip`: the first entry with a known disk extension (else the first
 *    file); stored or deflated entries.
 *  - Writes recompress the whole image and replace the archive (through a
 *    temporary file and a rename). A zip with more than one file is
 *    read-only, as in AppleWin, since the other entries would have to be
 *    rewritten too; so is an archive the host won't let us write.
 */
namespace diskfile {

enum class Kind { Plain, Gzip, Zip };

/** What the path holds, from its extension (`.gz`, `.zip`, else plain). */
Kind kindOf(const std::string& path);

/** True for a `.gz` or `.zip` path. */
inline bool isArchive(const std::string& path) { return kindOf(path) != Kind::Plain; }

/**
 * The image's own name, for telling its format: the path less `.gz`; for a
 * zip, the entry that read() would take (empty if the zip can't be read);
 * otherwise the path itself.
 */
std::string imageName(const std::string& path);

/** The lower-case extension of imageName() without the dot ("d64"), or "". */
std::string imageExt(const std::string& path);

/** The whole image, unpacked. False (with `err`) if it can't be read. */
bool read(const std::string& path, std::vector<uint8_t>& out, std::string* err = nullptr);

/** Whether writes can go back: the file is writable and, for a zip, holds one file. */
bool writable(const std::string& path);

/**
 * Replace the image with `data` (a plain file is truncated to its size; an
 * archive is recompressed).
 */
bool save(const std::string& path, const std::vector<uint8_t>& data, std::string* err = nullptr);

/**
 * Write back `len` bytes at `off` of `data`, the whole image as it now
 * stands: a plain file is patched in place, an archive recompressed whole.
 */
bool patch(const std::string& path, const std::vector<uint8_t>& data, size_t off, size_t len,
           std::string* err = nullptr);

/**
 * Write-back pacing for an archived image written a block at a time (SD
 * cards, the F011): recompressing a large image per 512-byte block would
 * stall the machine, so the image is saved once writes have stopped for a
 * second (host time), and on eject. Plain files are patched at once instead.
 */
class WriteBack {
public:
    void touched() { m_dirty = true; m_last = std::chrono::steady_clock::now(); }
    bool dirty() const { return m_dirty; }
    void clear() { m_dirty = false; }
    /** Call often (a device's tick): true once a save is due. Cheap when clean. */
    bool due() {
        if (!m_dirty || (++m_polls & 0xFFF)) return false;
        return std::chrono::steady_clock::now() - m_last >= std::chrono::seconds(1);
    }
private:
    bool m_dirty = false;
    unsigned m_polls = 0;
    std::chrono::steady_clock::time_point m_last{};
};

// Building blocks (tests, the tape loaders)
/** Unpack gzip (or zlib) data, several gzip members included. */
bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);
/** Pack as one gzip member; `name` goes in the header when given. */
std::vector<uint8_t> gzip(const std::vector<uint8_t>& in, const std::string& name = {});
/** A zip with one deflated entry. */
std::vector<uint8_t> zipOne(const std::string& entryName, const std::vector<uint8_t>& data);

}  // namespace diskfile
