/*  EditsPatcher -- voir EditsPatcher.h */

#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#endif

#include "EditsPatcher.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#define FSEEK64 _fseeki64
#define FTELL64 _ftelli64
#else
#define FSEEK64 fseeko
#define FTELL64 ftello
#endif

namespace EditsPatcher {

// ======================================================================
//  CRC32 (zlib) et EDC/ECC des CD-ROM (algorithme d'ecm.c, N. Corlett)
// ======================================================================

static uint32_t crcTable[256];
static bool crcReady = false;

uint32_t crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    if (!crcReady) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crcTable[n] = c;
        }
        crcReady = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint8_t eccF[256], eccB[256];
static uint32_t edcT[256];
static bool eccReady = false;

static void initEcc()
{
    if (eccReady) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
        eccF[i] = (uint8_t)j;
        eccB[i ^ j] = (uint8_t)i;
        uint32_t edc = i;
        for (int k = 0; k < 8; k++) edc = (edc >> 1) ^ ((edc & 1) ? 0xD8018001u : 0);
        edcT[i] = edc;
    }
    eccReady = true;
}

static uint32_t edcBlock(const uint8_t *src, size_t size)
{
    uint32_t edc = 0;
    while (size--) edc = (edc >> 8) ^ edcT[(edc ^ (*src++)) & 0xFF];
    return edc;
}

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void eccBlockCompute(const uint8_t *src, uint32_t majorCount, uint32_t minorCount,
                            uint32_t majorMult, uint32_t minorInc, uint8_t *dest)
{
    const uint32_t size = majorCount * minorCount;
    for (uint32_t major = 0; major < majorCount; major++) {
        uint32_t index = (major >> 1) * majorMult + (major & 1);
        uint8_t a = 0, b = 0;
        for (uint32_t minor = 0; minor < minorCount; minor++) {
            uint8_t t = src[index];
            index += minorInc;
            if (index >= size) index -= size;
            a ^= t;
            b ^= t;
            a = eccF[a];
        }
        a = eccB[eccF[a] ^ b];
        dest[major] = a;
        dest[major + majorCount] = a ^ b;
    }
}

static void eccGenerate(uint8_t *sector, bool zeroAddress)
{
    uint8_t address[4] = { 0, 0, 0, 0 };
    if (zeroAddress) {
        for (int i = 0; i < 4; i++) { address[i] = sector[12 + i]; sector[12 + i] = 0; }
    }
    eccBlockCompute(sector + 0xC, 86, 24, 2, 86, sector + 0x81C);   // P
    eccBlockCompute(sector + 0xC, 52, 43, 86, 88, sector + 0x8C8);  // Q
    if (zeroAddress) {
        for (int i = 0; i < 4; i++) sector[12 + i] = address[i];
    }
}

// Recalcule EDC/ECC d'un secteur brut (sectorSize 2352 ou 2336, mode 1 ou 2).
void regenerateSector(uint8_t *sector, int sectorSize, int mode)
{
    initEcc();
    uint8_t full[2352];
    uint8_t *s = full;
    if (sectorSize == 2352) {
        s = sector;
    } else if (sectorSize == 2336) {
        memset(full, 0, 16);
        memcpy(full + 16, sector, 2336);
    } else {
        return;
    }

    if (mode == 1) {
        if (sectorSize != 2352) return;
        put32le(s + 0x810, edcBlock(s, 0x810));
        memset(s + 0x814, 0, 8);
        eccGenerate(s, false);
    } else if (mode == 2) {
        if (s[0x12] & 0x20) {                     // Form 2 : EDC seul
            put32le(s + 0x92C, edcBlock(s + 0x10, 0x91C));
        } else {                                  // Form 1
            put32le(s + 0x818, edcBlock(s + 0x10, 0x808));
            eccGenerate(s, true);
        }
    }

    if (sectorSize == 2336) memcpy(sector, full + 16, 2336);
}

// ======================================================================
//  Fichiers
// ======================================================================

static std::string lowerStr(std::string s)
{
    for (size_t i = 0; i < s.size(); i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
    return s;
}

static std::string normSlashes(std::string s)
{
    for (size_t i = 0; i < s.size(); i++) if (s[i] == '\\') s[i] = '/';
    return s;
}

static std::string dirName(const std::string &p)
{
    std::string s = normSlashes(p);
    size_t i = s.find_last_of('/');
    return i == std::string::npos ? std::string(".") : (i == 0 ? std::string("/") : s.substr(0, i));
}

static std::string baseName(const std::string &p)
{
    std::string s = normSlashes(p);
    size_t i = s.find_last_of('/');
    return i == std::string::npos ? s : s.substr(i + 1);
}

static std::string joinPath(const std::string &d, const std::string &n)
{
    if (d.empty() || d == ".") return n;
    if (d[d.size() - 1] == '/' || d[d.size() - 1] == '\\') return d + n;
    return d + "/" + n;
}

static std::string extOf(const std::string &p)
{
    size_t i = p.find_last_of('.');
    return i == std::string::npos ? std::string() : lowerStr(p.substr(i + 1));
}

static bool fileSizeOf(const std::string &path, uint64_t &size)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    FSEEK64(f, 0, SEEK_END);
    size = (uint64_t)FTELL64(f);
    fclose(f);
    return true;
}

static bool readAt(FILE *f, uint64_t off, void *buf, size_t n)
{
    if (FSEEK64(f, (long long)off, SEEK_SET) != 0) return false;
    return fread(buf, 1, n, f) == n;
}

static bool writeAt(FILE *f, uint64_t off, const void *buf, size_t n)
{
    if (FSEEK64(f, (long long)off, SEEK_SET) != 0) return false;
    return fwrite(buf, 1, n, f) == n;
}

// ======================================================================
//  Ouverture d'une image (.cue / .iso / .bin / .img)
// ======================================================================

static const uint8_t SYNC[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

static bool addRawOrCooked(const std::string &file, Image &img, std::string &err)
{
    uint64_t size = 0;
    if (!fileSizeOf(file, size)) { err = "Cannot open " + file; return false; }
    Track t;
    t.file = file;
    uint8_t head[16];
    FILE *f = fopen(file.c_str(), "rb");
    bool raw = false;
    if (f) {
        raw = (size >= 2352 && size % 2352 == 0 && readAt(f, 0, head, 16) && memcmp(head, SYNC, 12) == 0);
        fclose(f);
    }
    if (raw) {
        t.sectorSize = 2352; t.sectors = size / 2352;
        t.mode = (head[15] == 2) ? 2 : 1;
        t.dataOffset = (t.mode == 2) ? 24 : 16;
        if (head[15] != 1 && head[15] != 2) { err = "Unsupported sector mode in " + file; return false; }
    } else if (size % 2048 == 0 && size > 0) {
        t.sectorSize = 2048; t.dataOffset = 0; t.mode = 0; t.sectors = size / 2048;
    } else {
        err = "Unsupported image layout (size is neither a multiple of 2048 nor 2352): " + file;
        return false;
    }
    img.files.push_back(file);
    img.tracks.push_back(t);
    return true;
}

namespace {
struct CueTrack {
    std::string file;
    std::string type;
    int index01;            // frames, -1 si absent
    CueTrack() : index01(-1) {}
};
}

static std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

static bool parseCue(const std::string &cuePath, Image &img, std::string &err)
{
    FILE *f = fopen(cuePath.c_str(), "rb");
    if (!f) { err = "Cannot open " + cuePath; return false; }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    const std::string dir = dirName(cuePath);
    std::vector<CueTrack> tracks;
    std::vector<std::string> fileList;
    std::string curFile;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.size() < 4) continue;
        std::string up = line;
        for (size_t i = 0; i < up.size(); i++) if (up[i] >= 'a' && up[i] <= 'z') up[i] = (char)(up[i] - 'a' + 'A');
        if (up.compare(0, 5, "FILE ") == 0) {
            std::string name;
            size_t q1 = line.find('"');
            if (q1 != std::string::npos) {
                size_t q2 = line.find('"', q1 + 1);
                if (q2 == std::string::npos) continue;
                name = line.substr(q1 + 1, q2 - q1 - 1);
            } else {
                std::istringstream ls(line); std::string kw; ls >> kw >> name;
            }
            curFile = joinPath(dir, normSlashes(name));
            fileList.push_back(curFile);
        } else if (up.compare(0, 6, "TRACK ") == 0) {
            std::istringstream ls(up); std::string kw, num, type;
            ls >> kw >> num >> type;
            CueTrack t; t.file = curFile; t.type = type;
            tracks.push_back(t);
        } else if (up.compare(0, 6, "INDEX ") == 0 && !tracks.empty()) {
            int idx = 0, mm = 0, ss = 0, ff = 0;
            if (sscanf(up.c_str() + 6, "%d %d:%d:%d", &idx, &mm, &ss, &ff) == 4 && idx == 1)
                tracks.back().index01 = (mm * 60 + ss) * 75 + ff;
        }
    }
    if (tracks.empty()) { err = "No track found in the cue sheet."; return false; }

    for (size_t i = 0; i < fileList.size(); i++) {
        uint64_t sz;
        if (fileSizeOf(fileList[i], sz)) img.files.push_back(fileList[i]);
    }

    for (size_t i = 0; i < tracks.size(); i++) {
        const CueTrack &ct = tracks[i];
        int mode = 0; uint32_t secSize = 0, dataOff = 0;
        if (ct.type == "MODE1/2352") { mode = 1; secSize = 2352; dataOff = 16; }
        else if (ct.type == "MODE2/2352") { mode = 2; secSize = 2352; dataOff = 24; }
        else if (ct.type == "MODE2/2336") { mode = 2; secSize = 2336; dataOff = 8; }
        else if (ct.type == "MODE1/2048" || ct.type == "MODE2/2048") { mode = 0; secSize = 2048; dataOff = 0; }
        else continue;                                       // AUDIO ou inconnu
        uint64_t size = 0;
        if (!fileSizeOf(ct.file, size)) { err = "Missing data file: " + ct.file; return false; }
        const uint64_t startFrames = ct.index01 > 0 ? (uint64_t)ct.index01 : 0;
        uint64_t endFrames = size / secSize;
        for (size_t j = i + 1; j < tracks.size(); j++)
            if (tracks[j].file == ct.file && tracks[j].index01 >= 0) { endFrames = (uint64_t)tracks[j].index01; break; }
        if (endFrames <= startFrames) continue;
        Track t;
        t.file = ct.file; t.fileOffset = startFrames * secSize; t.sectorSize = secSize;
        t.dataOffset = dataOff; t.mode = mode; t.sectors = endFrames - startFrames;
        img.tracks.push_back(t);
    }
    if (img.tracks.empty()) { err = "No data track found in the cue sheet."; return false; }
    return true;
}

bool openImage(const std::string &path, Image &img, std::string &err)
{
    img = Image();
    img.path = path;
    const std::string e = extOf(path);
    if (e == "cue") return parseCue(path, img, err);
    if (e == "iso" || e == "bin" || e == "img") return addRawOrCooked(path, img, err);
    err = "Unsupported image format '." + e + "'. Use a .cue/.bin, .iso or .img image "
          "(compressed and proprietary formats such as CHD or MDS are not supported).";
    return false;
}

// ======================================================================
//  Recherche des octets d'origine dans les donnees utilisateur
// ======================================================================

namespace {
struct Pat {
    std::vector<uint8_t> bytes;
    std::vector<std::pair<int, uint64_t> > hits;    // (piste, decalage dans le flux utilisateur)
};
struct Run { uint64_t off; std::vector<uint8_t> bytes; };
}

static uint64_t key8(const uint8_t *p) { uint64_t k; memcpy(&k, p, 8); return k; }

static bool patOk(const std::vector<uint8_t> &o, const std::vector<uint8_t> &r, std::string &why)
{
    if (o.size() < 16) { why = "too short to be searched reliably"; return false; }
    if (o.size() != r.size()) { why = "invalid record"; return false; }
    if (o == r) { why = "unchanged"; return false; }
    int seen[256]; memset(seen, 0, sizeof(seen));
    int distinct = 0;
    for (size_t i = 0; i < o.size(); i++) if (!seen[o[i]]++) distinct++;
    if (distinct < 3) { why = "too generic (almost uniform data)"; return false; }
    return true;
}

static std::string hexStr(uint64_t v)
{
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v);
    return b;
}

static void diffRuns(const uint8_t *a, const uint8_t *b, size_t n, size_t gap, std::vector<std::pair<size_t, size_t> > &runs)
{
    size_t i = 0;
    while (i < n) {
        if (a[i] == b[i]) { i++; continue; }
        size_t start = i, last = i;
        while (i < n && i - last <= gap) { if (a[i] != b[i]) last = i; i++; }
        runs.push_back(std::make_pair(start, last + 1));
    }
}

bool buildPatches(const Image &img, const std::vector<Record> &recs, Build &out,
                  std::string &err, Progress prog)
{
    out = Build();
    out.status.resize(recs.size());

    // ---- 1. motifs a chercher (dedoublonnes) ----
    std::vector<Pat> pats;
    std::map<std::string, int> patIndex;
    std::vector<int> primary(recs.size(), -1), alt(recs.size(), -1);
    size_t maxLen = 16;

    for (size_t i = 0; i < recs.size(); i++) {
        const Record &r = recs[i];
        std::string why;
        if (patOk(r.orig, r.repl, why)) {
            std::string k(r.orig.begin(), r.orig.end());
            std::map<std::string, int>::iterator it = patIndex.find(k);
            if (it == patIndex.end()) { Pat p; p.bytes = r.orig; pats.push_back(p); it = patIndex.insert(std::make_pair(k, (int)pats.size() - 1)).first; }
            primary[i] = it->second;
            maxLen = std::max(maxLen, r.orig.size());
        } else if (r.origAlt.empty()) {
            out.status[i].note = why;
        }
        std::string why2;
        if (!r.origAlt.empty() && patOk(r.origAlt, r.replAlt, why2)) {
            std::string k(r.origAlt.begin(), r.origAlt.end());
            std::map<std::string, int>::iterator it = patIndex.find(k);
            if (it == patIndex.end()) { Pat p; p.bytes = r.origAlt; pats.push_back(p); it = patIndex.insert(std::make_pair(k, (int)pats.size() - 1)).first; }
            alt[i] = it->second;
            maxLen = std::max(maxLen, r.origAlt.size());
        } else if (primary[i] < 0 && !r.origAlt.empty()) {
            out.status[i].note = why.empty() ? why2 : why;
        }
    }

    // ---- 2. index par prefixe de 8 octets + filtre de Bloom ----
    std::unordered_map<uint64_t, std::vector<int> > prefix;
    std::vector<uint8_t> bloom(1 << 18, 0);                     // 2^21 bits
    for (size_t p = 0; p < pats.size(); p++) {
        const uint64_t k = key8(&pats[p].bytes[0]);
        prefix[k].push_back((int)p);
        const uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 43);
        bloom[h >> 3] |= (uint8_t)(1u << (h & 7));
    }

    // ---- 3. balayage des pistes de donnees ----
    if (!pats.empty()) {
        uint64_t totalBytes = 0, doneBytes = 0;
        for (size_t t = 0; t < img.tracks.size(); t++) totalBytes += img.tracks[t].sectors * 2048;
        if (totalBytes == 0) totalBytes = 1;

        for (size_t t = 0; t < img.tracks.size(); t++) {
            const Track &tr = img.tracks[t];
            FILE *f = fopen(tr.file.c_str(), "rb");
            if (!f) { err = "Cannot open " + tr.file; return false; }
            const uint64_t chunkSectors = 512;
            std::vector<uint8_t> raw((size_t)chunkSectors * tr.sectorSize);
            std::vector<uint8_t> comb, carry;
            uint64_t base = 0;                                   // decalage virtuel de comb[0]
            for (uint64_t s0 = 0; s0 < tr.sectors; s0 += chunkSectors) {
                const uint64_t ns = std::min<uint64_t>(chunkSectors, tr.sectors - s0);
                if (!readAt(f, tr.fileOffset + s0 * tr.sectorSize, &raw[0], (size_t)(ns * tr.sectorSize))) break;
                comb = carry;
                const size_t carryLen = carry.size();
                comb.resize(carryLen + (size_t)ns * 2048);
                for (uint64_t k = 0; k < ns; k++)
                    memcpy(&comb[carryLen + (size_t)k * 2048], &raw[(size_t)(k * tr.sectorSize) + tr.dataOffset], 2048);

                if (comb.size() >= 8) {
                    const size_t last = comb.size() - 8;
                    for (size_t p = 0; p <= last; p++) {
                        const uint64_t k = key8(&comb[p]);
                        const uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 43);
                        if (!(bloom[h >> 3] & (1u << (h & 7)))) continue;
                        std::unordered_map<uint64_t, std::vector<int> >::const_iterator it = prefix.find(k);
                        if (it == prefix.end()) continue;
                        for (size_t q = 0; q < it->second.size(); q++) {
                            Pat &pt = pats[it->second[q]];
                            const size_t len = pt.bytes.size();
                            if (p + len > comb.size() || p + len <= carryLen) continue;
                            if (memcmp(&comb[p], &pt.bytes[0], len) == 0)
                                pt.hits.push_back(std::make_pair((int)t, base + p));
                        }
                    }
                }
                const size_t keep = std::min(comb.size(), maxLen - 1);
                carry.assign(comb.end() - keep, comb.end());
                base += comb.size() - keep;
                doneBytes += ns * 2048;
                if (prog && !prog("Searching the disc image...", (double)doneBytes / (double)totalBytes)) {
                    fclose(f); err = "Cancelled."; return false;
                }
            }
            fclose(f);
        }
    }

    // ---- 4. choix des occurrences et ecritures (avec detection de conflits) ----
    std::vector<std::map<uint64_t, uint8_t> > overlay(img.tracks.size());
    for (size_t i = 0; i < recs.size(); i++) {
        const Record &r = recs[i];
        Status &st = out.status[i];
        int pi = primary[i];
        bool useAlt = false;
        if ((pi < 0 || pats[pi].hits.empty()) && alt[i] >= 0 && !pats[alt[i]].hits.empty()) { pi = alt[i]; useAlt = true; }
        const std::string label = r.label.empty() ? ("edit #" + std::to_string(i)) : r.label;
        if (pi < 0) {
            out.log.push_back(label + ": skipped (" + st.note + ")");
            continue;
        }
        const std::vector<std::pair<int, uint64_t> > &hits = pats[pi].hits;
        if (hits.empty()) {
            st.note = "not found on the disc (compressed data or different layout)";
            out.log.push_back(label + ": " + st.note);
            continue;
        }
        if (hits.size() > 64) {
            st.note = "too many matches (" + std::to_string(hits.size()) + "), skipped";
            out.log.push_back(label + ": " + st.note);
            continue;
        }
        const std::vector<uint8_t> &repl = useAlt ? r.replAlt : r.repl;
        int applied = 0, conflicts = 0;
        for (size_t h = 0; h < hits.size(); h++) {
            std::map<uint64_t, uint8_t> &ov = overlay[hits[h].first];
            bool conflict = false;
            for (size_t k = 0; k < repl.size() && !conflict; k++) {
                std::map<uint64_t, uint8_t>::const_iterator it = ov.find(hits[h].second + k);
                if (it != ov.end() && it->second != repl[k]) conflict = true;
            }
            if (conflict) { conflicts++; continue; }
            for (size_t k = 0; k < repl.size(); k++) ov[hits[h].second + k] = repl[k];
            applied++;
        }
        st.matches = applied; st.usedAlt = useAlt;
        if (applied > 0) {
            out.applied++;
            st.note = std::string("patched ") + std::to_string(applied) + " occurrence(s)" + (useAlt ? " (palette context)" : "");
            if (conflicts) st.note += ", " + std::to_string(conflicts) + " conflicting skipped";
        } else {
            st.note = "conflicts with another edit, skipped";
        }
        out.log.push_back(label + ": " + st.note);
    }

    // ---- 5. octets utilisateur -> changements physiques (EDC/ECC regeneres) ----
    for (size_t t = 0; t < img.tracks.size(); t++) {
        const Track &tr = img.tracks[t];
        const std::map<uint64_t, uint8_t> &ov = overlay[t];
        if (ov.empty()) continue;
        std::vector<Change> &chg = out.changes[tr.file];

        if (tr.mode == 0) {                                      // ISO : secteurs contigus
            std::map<uint64_t, uint8_t>::const_iterator it = ov.begin();
            while (it != ov.end()) {
                Change c; c.offset = tr.fileOffset + it->first;
                uint64_t expect = it->first;
                while (it != ov.end() && it->first == expect) { c.bytes.push_back(it->second); expect++; ++it; }
                chg.push_back(c);
            }
        } else {                                                 // brut : secteur par secteur
            FILE *f = fopen(tr.file.c_str(), "rb");
            if (!f) { err = "Cannot open " + tr.file; return false; }
            std::map<uint64_t, uint8_t>::const_iterator it = ov.begin();
            std::vector<uint8_t> orig(tr.sectorSize), mod(tr.sectorSize);
            while (it != ov.end()) {
                const uint64_t sector = it->first / 2048;
                if (sector >= tr.sectors) { ++it; continue; }
                const uint64_t sbase = tr.fileOffset + sector * tr.sectorSize;
                if (!readAt(f, sbase, &orig[0], tr.sectorSize)) { fclose(f); err = "Read error in " + tr.file; return false; }
                mod = orig;
                while (it != ov.end() && it->first / 2048 == sector) {
                    mod[tr.dataOffset + (size_t)(it->first % 2048)] = it->second;
                    ++it;
                }
                regenerateSector(&mod[0], (int)tr.sectorSize, tr.mode);
                std::vector<std::pair<size_t, size_t> > runs;
                diffRuns(&orig[0], &mod[0], tr.sectorSize, 8, runs);
                for (size_t r = 0; r < runs.size(); r++) {
                    Change c; c.offset = sbase + runs[r].first;
                    c.bytes.assign(mod.begin() + runs[r].first, mod.begin() + runs[r].second);
                    chg.push_back(c);
                }
            }
            fclose(f);
        }
    }

    // trie + fusion des changements adjacents, par fichier
    for (std::map<std::string, std::vector<Change> >::iterator it = out.changes.begin(); it != out.changes.end(); ++it) {
        std::vector<Change> &v = it->second;
        std::sort(v.begin(), v.end(), [](const Change &a, const Change &b) { return a.offset < b.offset; });
        std::vector<Change> merged;
        for (size_t i = 0; i < v.size(); i++) {
            if (!merged.empty() && merged.back().offset + merged.back().bytes.size() == v[i].offset)
                merged.back().bytes.insert(merged.back().bytes.end(), v[i].bytes.begin(), v[i].bytes.end());
            else
                merged.push_back(v[i]);
        }
        v.swap(merged);
        uint64_t sz = 0; fileSizeOf(it->first, sz);
        out.fileSize[it->first] = sz;
    }
    return true;
}

// ======================================================================
//  Sorties : image patchee, BPS, IPS
// ======================================================================

static bool copyFile(const std::string &src, const std::string &dst, std::string &err,
                     Progress prog, const char *what, uint64_t &done, uint64_t total)
{
    FILE *in = fopen(src.c_str(), "rb");
    if (!in) { err = "Cannot open " + src; return false; }
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); err = "Cannot create " + dst; return false; }
    std::vector<uint8_t> buf(4 << 20);
    size_t n;
    bool ok = true;
    while ((n = fread(&buf[0], 1, buf.size(), in)) > 0) {
        if (fwrite(&buf[0], 1, n, out) != n) { err = "Write error on " + dst; ok = false; break; }
        done += n;
        if (prog && !prog(what, total ? (double)done / (double)total : 1.0)) { err = "Cancelled."; ok = false; break; }
    }
    fclose(in);
    if (fclose(out) != 0 && ok) { err = "Write error on " + dst; ok = false; }
    return ok;
}

bool writePatchedImage(const Image &img, const Build &b, const std::string &destDir,
                       std::string &err, Progress prog)
{
    uint64_t total = 0, done = 0;
    for (size_t i = 0; i < img.files.size(); i++) {
        if (normSlashes(dirName(img.files[i])) == normSlashes(destDir)) {
            err = "Choose a different folder from the original image.";
            return false;
        }
        uint64_t sz = 0; fileSizeOf(img.files[i], sz); total += sz;
    }
    for (size_t i = 0; i < img.files.size(); i++) {
        const std::string dst = joinPath(destDir, baseName(img.files[i]));
        if (!copyFile(img.files[i], dst, err, prog, "Copying the image...", done, total)) return false;
        std::map<std::string, std::vector<Change> >::const_iterator it = b.changes.find(img.files[i]);
        if (it == b.changes.end()) continue;
        FILE *f = fopen(dst.c_str(), "r+b");
        if (!f) { err = "Cannot reopen " + dst; return false; }
        for (size_t c = 0; c < it->second.size(); c++)
            if (!writeAt(f, it->second[c].offset, &it->second[c].bytes[0], it->second[c].bytes.size())) {
                fclose(f); err = "Write error on " + dst; return false;
            }
        if (fclose(f) != 0) { err = "Write error on " + dst; return false; }
    }
    if (extOf(img.path) == "cue") {                              // la feuille de pistes suit l'image
        std::string dst = joinPath(destDir, baseName(img.path));
        uint64_t d = 0;
        if (!copyFile(img.path, dst, err, Progress(), "", d, 0)) return false;
    }
    return true;
}

static void bpsEncode(std::vector<uint8_t> &v, uint64_t data)
{
    for (;;) {
        uint8_t x = data & 0x7F;
        data >>= 7;
        if (data == 0) { v.push_back(0x80 | x); break; }
        v.push_back(x);
        data--;
    }
}

static void put32leVec(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}

bool writeBps(const std::string &sourceFile, const std::vector<Change> &changes,
              const std::string &destPath, std::string &err, Progress prog)
{
    uint64_t size = 0;
    if (!fileSizeOf(sourceFile, size)) { err = "Cannot open " + sourceFile; return false; }

    std::vector<uint8_t> p;
    p.push_back('B'); p.push_back('P'); p.push_back('S'); p.push_back('1');
    bpsEncode(p, size); bpsEncode(p, size); bpsEncode(p, 0);
    uint64_t pos = 0;
    for (size_t i = 0; i < changes.size(); i++) {
        const Change &c = changes[i];
        if (c.offset > pos) bpsEncode(p, ((c.offset - pos - 1) << 2) | 0);     // SourceRead
        bpsEncode(p, ((uint64_t)(c.bytes.size() - 1) << 2) | 1);                // TargetRead
        p.insert(p.end(), c.bytes.begin(), c.bytes.end());
        pos = c.offset + c.bytes.size();
    }
    if (pos < size) bpsEncode(p, ((size - pos - 1) << 2) | 0);

    // CRC32 de la source et de la cible (source + changements), en une passe
    FILE *f = fopen(sourceFile.c_str(), "rb");
    if (!f) { err = "Cannot open " + sourceFile; return false; }
    std::vector<uint8_t> buf(4 << 20), tgt;
    uint32_t crcS = 0, crcT = 0;
    uint64_t off = 0;
    size_t ci = 0;
    size_t n;
    while ((n = fread(&buf[0], 1, buf.size(), f)) > 0) {
        crcS = crc32(crcS, &buf[0], n);
        tgt.assign(buf.begin(), buf.begin() + n);
        while (ci < changes.size() && changes[ci].offset + changes[ci].bytes.size() <= off) ci++;
        for (size_t k = ci; k < changes.size() && changes[k].offset < off + n; k++) {
            const Change &c = changes[k];
            const uint64_t a = std::max<uint64_t>(c.offset, off), e = std::min<uint64_t>(c.offset + c.bytes.size(), off + n);
            for (uint64_t x = a; x < e; x++) tgt[(size_t)(x - off)] = c.bytes[(size_t)(x - c.offset)];
        }
        crcT = crc32(crcT, &tgt[0], n);
        off += n;
        if (prog && !prog("Computing checksums...", size ? (double)off / (double)size : 1.0)) { fclose(f); err = "Cancelled."; return false; }
    }
    fclose(f);
    put32leVec(p, crcS); put32leVec(p, crcT);
    put32leVec(p, crc32(0, &p[0], p.size()));

    FILE *o = fopen(destPath.c_str(), "wb");
    if (!o) { err = "Cannot create " + destPath; return false; }
    const bool ok = fwrite(&p[0], 1, p.size(), o) == p.size();
    fclose(o);
    if (!ok) err = "Write error on " + destPath;
    return ok;
}

bool writeIps(const std::vector<Change> &changes, const std::string &destPath, std::string &err)
{
    std::vector<uint8_t> p;
    const char *hdr = "PATCH";
    p.insert(p.end(), hdr, hdr + 5);
    for (size_t i = 0; i < changes.size(); i++) {
        const Change &c = changes[i];
        size_t done = 0;
        while (done < c.bytes.size()) {
            const uint64_t off = c.offset + done;
            const size_t n = std::min<size_t>(c.bytes.size() - done, 0xFFFF);
            if (off + n > 0x1000000ULL) {
                err = "IPS can only address the first 16 MiB of a file; this patch reaches offset " + hexStr(off) +
                      ". Use the BPS patch or the patched image instead.";
                return false;
            }
            if (off == 0x454F46ULL) {
                err = "An edit starts exactly at the IPS end marker offset (0x454F46); use the BPS patch instead.";
                return false;
            }
            p.push_back((uint8_t)(off >> 16)); p.push_back((uint8_t)(off >> 8)); p.push_back((uint8_t)off);
            p.push_back((uint8_t)(n >> 8)); p.push_back((uint8_t)n);
            p.insert(p.end(), c.bytes.begin() + done, c.bytes.begin() + done + n);
            done += n;
        }
    }
    p.push_back('E'); p.push_back('O'); p.push_back('F');
    FILE *o = fopen(destPath.c_str(), "wb");
    if (!o) { err = "Cannot create " + destPath; return false; }
    const bool ok = fwrite(&p[0], 1, p.size(), o) == p.size();
    fclose(o);
    if (!ok) err = "Write error on " + destPath;
    return ok;
}

// ======================================================================
//  Pack .kmod
//    "KMOD" | version u32 | meta (u32 len + octets) | n u32 |
//    n x { type u32, addr u32, len u32, orig[len], repl[len] }   (little-endian)
//  Version 2 : pour le type 2 (Color RAM), addr est l'adresse OCTET dans la CRAM
//  (une couleur = 2 octets, 4 en mode 24 bits). Version 1 : addr etait l'index
//  de la couleur (modes 15 bits seulement) ; converti au chargement.
// ======================================================================

static void putU32(std::vector<uint8_t> &v, uint32_t x) { put32leVec(v, x); }

bool savePack(const std::string &path, const std::vector<Record> &recs, const std::string &meta,
              std::string &err)
{
    std::vector<uint8_t> p;
    p.push_back('K'); p.push_back('M'); p.push_back('O'); p.push_back('D');
    putU32(p, 2);
    putU32(p, (uint32_t)meta.size());
    p.insert(p.end(), meta.begin(), meta.end());
    putU32(p, (uint32_t)recs.size());
    for (size_t i = 0; i < recs.size(); i++) {
        putU32(p, (uint32_t)recs[i].type); putU32(p, recs[i].addr); putU32(p, (uint32_t)recs[i].orig.size());
        p.insert(p.end(), recs[i].orig.begin(), recs[i].orig.end());
        p.insert(p.end(), recs[i].repl.begin(), recs[i].repl.end());
    }
    FILE *o = fopen(path.c_str(), "wb");
    if (!o) { err = "Cannot create " + path; return false; }
    const bool ok = fwrite(&p[0], 1, p.size(), o) == p.size();
    fclose(o);
    if (!ok) err = "Write error on " + path;
    return ok;
}

bool loadPack(const std::string &path, std::vector<Record> &recs, std::string &meta, std::string &err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { err = "Cannot open " + path; return false; }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);

    size_t pos = 0;
    auto get32 = [&](uint32_t &x) -> bool {
        if (pos + 4 > d.size()) return false;
        x = (uint32_t)d[pos] | ((uint32_t)d[pos + 1] << 8) | ((uint32_t)d[pos + 2] << 16) | ((uint32_t)d[pos + 3] << 24);
        pos += 4;
        return true;
    };
    if (d.size() < 8 || memcmp(&d[0], "KMOD", 4) != 0) { err = "Not a Kronos edit pack."; return false; }
    pos = 4;
    uint32_t ver = 0, ml = 0, cnt = 0;
    if (!get32(ver) || (ver != 1 && ver != 2)) { err = "Unsupported edit pack version."; return false; }
    if (!get32(ml) || pos + ml > d.size()) { err = "Corrupted edit pack."; return false; }
    meta.assign((const char *)&d[pos], ml); pos += ml;
    if (!get32(cnt)) { err = "Corrupted edit pack."; return false; }
    recs.clear();
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t type, addr, len;
        if (!get32(type) || !get32(addr) || !get32(len) || len > (1u << 24) || pos + 2 * (size_t)len > d.size()) {
            err = "Corrupted edit pack."; return false;
        }
        Record r; r.type = (int)type; r.addr = addr;
        if (ver == 1 && type == 2) r.addr = (addr * 2) & 0xFFF;   // index de couleur -> adresse octet
        r.orig.assign(d.begin() + pos, d.begin() + pos + len); pos += len;
        r.repl.assign(d.begin() + pos, d.begin() + pos + len); pos += len;
        recs.push_back(r);
    }
    return true;
}

} // namespace EditsPatcher
