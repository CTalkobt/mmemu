// ImageDisk (.imd) files for the 1571's MFM disks (see imd_disk.h).

#include "imd_disk.h"
#include "libdevices/main/disk_file.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <unistd.h>

bool ImdDisk::open(const std::string& path) {
    std::vector<uint8_t> d;
    if (!diskfile::read(path, d)) return false;           // plain, .gz or .zip
    if (d.size() < 4 || d[0] != 'I' || d[1] != 'M' || d[2] != 'D') return false;
    size_t o = 0;
    while (o < d.size() && d[o] != 0x1A) ++o;
    if (o >= d.size()) return false;
    std::string header(d.begin(), d.begin() + (long)o);
    ++o;
    std::vector<Track> tracks;
    while (o < d.size()) {
        if (o + 5 > d.size()) return false;
        Track t;
        t.mode = d[o];
        t.cyl = d[o + 1];
        const uint8_t hd = d[o + 2];
        t.head = hd & 1;
        t.cylMap = hd & 0x80;
        t.headMap = hd & 0x40;
        const int n = d[o + 3];
        t.sizeCode = d[o + 4];
        o += 5;
        if (t.sizeCode > 6) return false;                   // variable sizes ($FF): not supported
        const size_t size = 128u << t.sizeCode;
        if (o + (size_t)n * (1 + t.cylMap + t.headMap) > d.size()) return false;
        t.sectors.resize((size_t)n);
        for (int i = 0; i < n; ++i) {
            t.sectors[i].id = d[o + i];
            t.sectors[i].cyl = t.cyl;
            t.sectors[i].head = t.head;
        }
        o += (size_t)n;
        if (t.cylMap) { for (int i = 0; i < n; ++i) t.sectors[i].cyl = d[o + i]; o += (size_t)n; }
        if (t.headMap) { for (int i = 0; i < n; ++i) t.sectors[i].head = d[o + i]; o += (size_t)n; }
        for (int i = 0; i < n; ++i) {
            if (o >= d.size()) return false;
            Sector& s = t.sectors[i];
            s.type = d[o++];
            if (s.type == 0) continue;
            if (s.type > 8) return false;
            if (s.type % 2 == 0) {                          // compressed: one byte
                if (o >= d.size()) return false;
                s.data.assign(size, d[o++]);
            } else {
                if (o + size > d.size()) return false;
                s.data.assign(d.begin() + (long)o, d.begin() + (long)(o + size));
                o += size;
            }
        }
        tracks.push_back(std::move(t));
    }
    m_tracks = std::move(tracks);
    m_header = header;
    m_path = path;
    m_writable = diskfile::writable(path);
    return true;
}

bool ImdDisk::save() {
    if (!m_writable) return false;
    std::vector<uint8_t> d(m_header.begin(), m_header.end());
    d.push_back(0x1A);
    for (const Track& t : m_tracks) {
        d.push_back(t.mode);
        d.push_back(t.cyl);
        d.push_back((uint8_t)(t.head | (t.cylMap ? 0x80 : 0) | (t.headMap ? 0x40 : 0)));
        d.push_back((uint8_t)t.sectors.size());
        d.push_back(t.sizeCode);
        for (const Sector& s : t.sectors) d.push_back(s.id);
        if (t.cylMap) for (const Sector& s : t.sectors) d.push_back(s.cyl);
        if (t.headMap) for (const Sector& s : t.sectors) d.push_back(s.head);
        for (const Sector& s : t.sectors) {
            if (s.type == 0 || s.data.empty()) { d.push_back(0); continue; }
            const bool same = std::all_of(s.data.begin(), s.data.end(), [&](uint8_t b) { return b == s.data[0]; });
            const uint8_t base = (uint8_t)((s.type - 1) & ~1);      // 0 plain, 2 deleted, 4 error, 6 both
            if (same) { d.push_back((uint8_t)(base + 2)); d.push_back(s.data[0]); }
            else { d.push_back((uint8_t)(base + 1)); d.insert(d.end(), s.data.begin(), s.data.end()); }
        }
    }
    return diskfile::save(m_path, d);
}

ImdDisk::Track* ImdDisk::track(int cyl, int side) {
    for (Track& t : m_tracks)
        if (t.cyl == cyl && t.head == side) return &t;
    return nullptr;
}

ImdDisk::Sector* ImdDisk::sector(int cyl, int side, int id) {
    Track* t = track(cyl, side);
    if (!t) return nullptr;
    for (Sector& s : t->sectors)
        if (s.id == id) return &s;
    return nullptr;
}

int ImdDisk::sectorSize(int cyl, int side) {
    Track* t = track(cyl, side);
    return t ? (128 << t->sizeCode) : 0;
}

bool ImdDisk::format(int lastTrack, int sectors, int sizeCode, int firstSector, int interleave,
                     int logicalTrack, int trackOffset, uint8_t fill, bool doubleSided, int side,
                     const std::vector<uint8_t>& table) {
    if (!m_writable || sectors <= 0 || sectors > 64 || sizeCode < 0 || sizeCode > 3) return false;
    if (interleave <= 0) interleave = 1;
    std::vector<Track> tracks;
    for (int cyl = trackOffset; cyl <= lastTrack + trackOffset && cyl < 256; ++cyl) {
        for (int h = 0; h < 2; ++h) {
            if (doubleSided ? false : h != side) continue;
            Track t;
            t.cyl = (uint8_t)cyl;
            t.head = (uint8_t)h;
            t.sizeCode = (uint8_t)sizeCode;
            const uint8_t logical = (uint8_t)(logicalTrack + cyl - trackOffset);
            t.cylMap = logical != t.cyl;
            t.sectors.resize((size_t)sectors);
            std::vector<bool> used((size_t)sectors, false);
            int pos = 0;
            for (int i = 0; i < sectors; ++i) {
                if (table.size() >= (size_t)sectors) pos = i;   // a sector table: the IDs in physical order
                else while (used[(size_t)pos]) pos = (pos + 1) % sectors;   // the interleave
                used[(size_t)pos] = true;
                Sector& s = t.sectors[(size_t)pos];
                s.id = table.size() >= (size_t)sectors ? table[(size_t)i] : (uint8_t)(firstSector + i);
                s.cyl = logical;
                s.head = (uint8_t)h;
                s.type = 1;
                s.data.assign((size_t)(128 << sizeCode), fill);
                pos = (pos + interleave) % sectors;
            }
            tracks.push_back(std::move(t));
        }
    }
    m_tracks = std::move(tracks);
    return save();
}
