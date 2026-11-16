/*  Copyright 2026 Kronos fork contributors

    This file is part of Kronos.

    Kronos is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kronos is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kronos; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*  TextTools -- text of a game for translation, read from its disc image.

    - CharTable: character table (.tbl, the usual "hex=text" format of the
      translation tools): "41=A", "8140= " (2 bytes), "/00=<end>" end of
      string, "*FE" new line, "$F0=<name>" control code. Text is UTF-8;
      "\n" in a value is a new line. Bytes with no entry are shown and
      typed as <$XX>.
    - ISO 9660 file list of the data track (Saturn discs: track 1).
    - Searches in the user data (2048 bytes a sector) of the data track:
      encoded text, relative search (to find the codes of an unknown
      table: the differences between letters are kept, e.g. "Hello" with
      'H' = 0x12 gives 'e' = 0x12 + ('e' - 'H') only inside one class
      A-Z / a-z / 0-9), and string dump with a table.
    - Offsets: user data offset in the track = LBA * 2048 + byte, the same
      numbers as EditsPatcher::DiskEdit.

    No dependency on Qt nor on the emulator core (C++11). */

#ifndef TEXTTOOLS_H
#define TEXTTOOLS_H

#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "EditsPatcher.h"

namespace TextTools {

enum EntryKind { Normal = 0, End = 1, NewLine = 2 };

struct TblEntry {
    std::vector<uint8_t> bytes;
    std::string text;           // UTF-8
    int kind;                   // EntryKind
    TblEntry() : kind(Normal) {}
};

class CharTable {
public:
    CharTable();
    void clear();
    bool empty() const { return mEntries.empty(); }
    size_t size() const { return mEntries.size(); }
    const std::vector<TblEntry> &entries() const { return mEntries; }
    void setEntries(const std::vector<TblEntry> &e);
    // Adds (or replaces the entry with the same bytes).
    void set(const std::vector<uint8_t> &bytes, const std::string &text, int kind = Normal);

    // .tbl text (UTF-8). Unknown lines are counted in *badLines.
    bool parse(const std::string &utf8, std::string &err, int *badLines = 0);
    std::string toTbl() const;

    // Longest entry at p (0 = none).
    const TblEntry *match(const uint8_t *p, size_t n) const;
    // Decodes up to n bytes; stops after an end entry when stopAtEnd
    // (its bytes are counted in *used but its text is not added).
    std::string decode(const uint8_t *p, size_t n, bool stopAtEnd, size_t *used = 0,
                       int *unknown = 0, size_t *termLen = 0) const;
    // Text -> bytes, longest text first; <$XX> = raw byte. False on a
    // character with no entry (*errPos = its position in bytes).
    bool encode(const std::string &text, std::vector<uint8_t> &out, std::string &err, size_t *errPos = 0) const;
    // Bytes of the first end entry (empty if none).
    std::vector<uint8_t> endBytes() const;
    // Width of the codes (most common entry size, 1 or 2).
    int codeWidth() const;

private:
    void rebuild();
    std::vector<TblEntry> mEntries;
    std::vector<int> mByFirst[256];        // entries by first byte, longest first
    std::map<std::string, int> mByText;    // text -> entry
    size_t mMaxText;
};

// Ready-made tables.
CharTable asciiTable();                    // 20-7E, end 00

// ---- where the bytes come from: the data track of the disc image, or a
// buffer in memory (a decompressed block) ----
struct Source {
    const EditsPatcher::Image *img;
    int track;
    const uint8_t *mem;
    size_t memLen;
    Source(const EditsPatcher::Image &i, int t) : img(&i), track(t), mem(0), memLen(0) {}
    Source(const uint8_t *m, size_t n) : img(0), track(0), mem(m), memLen(n) {}
    uint64_t size() const;
    bool read(uint64_t offset, size_t n, std::vector<uint8_t> &out, std::string &err) const;
};

// ---- disc ----
struct IsoFile {
    std::string path;                       // "/DIR/FILE.BIN"
    uint32_t lba;
    uint32_t size;
};
bool listIsoFiles(const EditsPatcher::Image &img, int track, std::vector<IsoFile> &files, std::string &err);
// File holding a user data offset (NULL if none).
const IsoFile *fileAt(const std::vector<IsoFile> &files, uint64_t offset);

typedef std::function<bool(double fraction)> Progress;

// Exact bytes in [start, start + length) of the track.
bool searchBytes(const EditsPatcher::Image &img, int track, uint64_t start, uint64_t length,
                 const std::vector<uint8_t> &pattern, std::vector<uint64_t> &hits, size_t maxHits,
                 Progress prog, std::string &err);

struct RelHit {
    uint64_t offset;
    int width;                              // 1, 2 (big endian) or -2 (little endian)
    std::vector<uint32_t> values;           // code of each character of the query (spaces: any)
};
// Query: ASCII letters / digits; a space or '?' matches any code.
bool relativeSearch(const EditsPatcher::Image &img, int track, uint64_t start, uint64_t length,
                    const std::string &query, std::vector<RelHit> &hits, size_t maxHits,
                    Progress prog, std::string &err);
// Table entries deduced from a hit: whole A-Z / a-z / 0-9 classes met in the query.
std::vector<TblEntry> relativeProposal(const std::string &query, const RelHit &hit);

struct Found {
    uint64_t offset;
    uint32_t length;                        // text bytes, without the end code
    uint32_t termLength;                    // end code bytes after the text
    std::string text;
};
bool dumpStrings(const EditsPatcher::Image &img, int track, uint64_t start, uint64_t length,
                 const CharTable &table, int minChars, bool requireEnd, std::vector<Found> &out,
                 size_t maxStrings, Progress prog, std::string &err);
// String starting at offset (up to the end code, an unknown byte or maxBytes).
bool readString(const EditsPatcher::Image &img, int track, uint64_t offset, const CharTable &table,
                size_t maxBytes, Found &out, std::string &err);

// Same searches on any source (disc or decompressed block).
bool searchBytes(const Source &src, uint64_t start, uint64_t length, const std::vector<uint8_t> &pattern,
                 std::vector<uint64_t> &hits, size_t maxHits, Progress prog, std::string &err);
bool relativeSearch(const Source &src, uint64_t start, uint64_t length, const std::string &query,
                    std::vector<RelHit> &hits, size_t maxHits, Progress prog, std::string &err);
bool dumpStrings(const Source &src, uint64_t start, uint64_t length, const CharTable &table, int minChars,
                 bool requireEnd, std::vector<Found> &out, size_t maxStrings, Progress prog, std::string &err);
bool readString(const Source &src, uint64_t offset, const CharTable &table, size_t maxBytes, Found &out,
                std::string &err);

// ---- compression ----
// Formats whose description is public and stable:
//  - LZSS of H. Okumura (LZSS.C, 1989): flag byte read LSB first, 1 = literal
//    byte, 0 = 2 bytes i, j: ring position i | (j & F0) << 4, length (j & 0F) + 3;
//    ring of 4096 bytes starting at 4096 - 18, filled with 0x20 (classic) or
//    0x00 (variant). Without size header the data ends with the block.
//    "with size header": a 32-bit big endian unpacked size comes first.
//  - PRS (Sega): control bits read LSB first; 1 = literal byte; 0 1 = long
//    copy, 16-bit little endian word w: offset (w >> 3) - 0x2000, length w & 7
//    + 2, or the next byte + 1 when w & 7 = 0, w = 0 ends the data; 0 0 = short
//    copy, 2 more bits = length - 2, next byte = offset + 256.
enum Codec { CodecLzss = 0, CodecLzssZero = 1, CodecLzssSize = 2, CodecPrs = 3, CodecCount = 4 };
const char *codecName(int codec);
// Unpacks from src (n bytes available). *consumed = compressed bytes used.
bool decompress(int codec, const uint8_t *src, size_t n, std::vector<uint8_t> &out, size_t *consumed,
                size_t maxOut, std::string &err);
std::vector<uint8_t> compress(int codec, const std::vector<uint8_t> &data);
struct PackedBlock { uint64_t offset; uint32_t packedSize; uint32_t size; };
// PRS blocks of a buffer: they end with their end code, every copy stays
// inside the data already unpacked, and unpack to minSize bytes at least.
std::vector<PackedBlock> findPrsBlocks(const uint8_t *data, size_t n, size_t minSize, size_t maxBlocks,
                                       Progress prog);

// ---- pointers ----
// A pointer is a word (2 or 4 bytes, big or little endian) whose value is
// base + offset of the string in its container (the file, or the unpacked
// block). base = RAM address where the game loads the container (32-bit
// absolute pointers, e.g. 0x06040000), or a table start (relative pointers).
struct PtrFormat {
    int width;          // 2 or 4
    bool bigEndian;
    int64_t base;
    PtrFormat() : width(4), bigEndian(true), base(0) {}
};
uint32_t readPtr(const uint8_t *p, const PtrFormat &f);
void writePtr(uint8_t *p, const PtrFormat &f, uint32_t value);
// Bases for which many words of 'data' point to the strings at 'offsets'
// (sorted), best first: (base, number of words).
std::vector<std::pair<int64_t, int> > guessPointerBase(const uint8_t *data, size_t n,
                                                       const std::vector<uint64_t> &offsets, int width,
                                                       bool bigEndian, size_t maxResults);
// A word holds the wanted address (32-bit: also through the SH-2 cache-through
// mirror, 0x2xxxxxxx for 0x0xxxxxxx).
bool ptrMatches(uint32_t value, const PtrFormat &f, uint32_t want);
// New word value for a pointer, keeping the mirror the old one used.
uint32_t ptrRetarget(uint32_t oldValue, const PtrFormat &f, uint32_t newValue);
// Positions in 'data' of the words pointing to 'offset' (aligned on the width).
std::vector<uint64_t> findPointers(const uint8_t *data, size_t n, const PtrFormat &f, uint64_t offset);
// Free space: runs of one byte value (00 or FF) of minLen bytes at least.
std::vector<std::pair<uint64_t, uint64_t> > findFreeSpace(const uint8_t *data, size_t n, size_t minLen);

// ---- translation ----
// Bytes written for a translation that must fit in 'capacity' bytes (the
// original text) followed by the original end code 'term'. The rest is
// filled with 'pad' (repeated). Returns false (with err) when too long.
bool buildReplacement(const CharTable &table, const std::string &translation, uint32_t capacity,
                      const std::vector<uint8_t> &term, const std::vector<uint8_t> &pad,
                      std::vector<uint8_t> &out, int *overflow, std::string &err);

// Counts UTF-8 characters.
size_t utf8Length(const std::string &s);
std::string hexBytes(const std::vector<uint8_t> &b, const char *sep = " ");
bool parseHexBytes(const std::string &s, std::vector<uint8_t> &out);

} // namespace TextTools

#endif
