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

/*  TextTools -- see TextTools.h. */

#include "TextTools.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace TextTools {

// ======================================================================
//  Small helpers
// ======================================================================

size_t utf8Length(const std::string &s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string hexBytes(const std::vector<uint8_t> &b, const char *sep)
{
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < b.size(); i++) {
        if (i && sep) out += sep;
        out += digits[b[i] >> 4];
        out += digits[b[i] & 15];
    }
    return out;
}

bool parseHexBytes(const std::string &s, std::vector<uint8_t> &out)
{
    out.clear();
    int hi = -1;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == ',' || c == ':') continue;
        if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X') && hi < 0) { i++; continue; }
        const int v = hexVal(c);
        if (v < 0) return false;
        if (hi < 0) hi = v;
        else { out.push_back((uint8_t)(hi << 4 | v)); hi = -1; }
    }
    return hi < 0 && !out.empty();
}

static std::string escapeText(const std::string &t)
{
    std::string o;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\n') o += "\\n";
        else if (t[i] == '\\') o += "\\\\";
        else o += t[i];
    }
    return o;
}

static std::string unescapeText(const std::string &t)
{
    std::string o;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\\' && i + 1 < t.size()) {
            if (t[i + 1] == 'n') { o += '\n'; i++; continue; }
            if (t[i + 1] == '\\') { o += '\\'; i++; continue; }
        }
        o += t[i];
    }
    return o;
}

// ======================================================================
//  CharTable
// ======================================================================

CharTable::CharTable() : mMaxText(0) {}

void CharTable::clear()
{
    mEntries.clear();
    rebuild();
}

void CharTable::setEntries(const std::vector<TblEntry> &e)
{
    mEntries.clear();
    for (size_t i = 0; i < e.size(); i++)
        if (!e[i].bytes.empty()) mEntries.push_back(e[i]);
    rebuild();
}

void CharTable::set(const std::vector<uint8_t> &bytes, const std::string &text, int kind)
{
    if (bytes.empty()) return;
    for (size_t i = 0; i < mEntries.size(); i++)
        if (mEntries[i].bytes == bytes) {
            mEntries[i].text = text;
            mEntries[i].kind = kind;
            rebuild();
            return;
        }
    TblEntry e;
    e.bytes = bytes;
    e.text = text;
    e.kind = kind;
    mEntries.push_back(e);
    rebuild();
}

void CharTable::rebuild()
{
    for (int i = 0; i < 256; i++) mByFirst[i].clear();
    mByText.clear();
    mMaxText = 0;
    for (size_t i = 0; i < mEntries.size(); i++) {
        const TblEntry &e = mEntries[i];
        mByFirst[e.bytes[0]].push_back((int)i);
        if (e.text.empty()) continue;
        std::map<std::string, int>::iterator it = mByText.find(e.text);
        // a normal entry wins over an end / new line entry with the same text
        if (it == mByText.end()) mByText[e.text] = (int)i;
        else if (mEntries[it->second].kind != Normal && e.kind == Normal) it->second = (int)i;
        mMaxText = std::max(mMaxText, e.text.size());
    }
    for (int b = 0; b < 256; b++) {
        std::vector<int> &v = mByFirst[b];
        std::stable_sort(v.begin(), v.end(), [this](int x, int y) {
            return mEntries[x].bytes.size() > mEntries[y].bytes.size();
        });
    }
}

bool CharTable::parse(const std::string &utf8In, std::string &err, int *badLines)
{
    std::string utf8 = utf8In;
    if (utf8.size() >= 3 && (unsigned char)utf8[0] == 0xEF && (unsigned char)utf8[1] == 0xBB && (unsigned char)utf8[2] == 0xBF)
        utf8.erase(0, 3);
    std::vector<TblEntry> out;
    int bad = 0;
    size_t pos = 0;
    while (pos <= utf8.size()) {
        size_t eol = utf8.find('\n', pos);
        if (eol == std::string::npos) eol = utf8.size();
        std::string line = utf8.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        size_t a = 0;
        while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) a++;
        if (a == line.size()) continue;
        if (line[a] == '@' || line[a] == '#' || line[a] == ';' || line.compare(a, 2, "//") == 0) continue;
        int kind = Normal;
        if (line[a] == '/') { kind = End; a++; }
        else if (line[a] == '*') { kind = NewLine; a++; }
        else if (line[a] == '$') { a++; }
        const size_t eq = line.find('=', a);
        std::string key = line.substr(a, eq == std::string::npos ? std::string::npos : eq - a);
        while (!key.empty() && (key[key.size() - 1] == ' ' || key[key.size() - 1] == '\t')) key.erase(key.size() - 1);
        TblEntry e;
        e.kind = kind;
        bool ok = !key.empty() && (key.size() % 2) == 0;
        for (size_t i = 0; ok && i < key.size(); i += 2) {
            const int h = hexVal(key[i]), l = hexVal(key[i + 1]);
            if (h < 0 || l < 0) ok = false;
            else e.bytes.push_back((uint8_t)(h << 4 | l));
        }
        if (!ok || (eq == std::string::npos && kind == Normal)) { bad++; continue; }
        if (eq != std::string::npos) e.text = unescapeText(line.substr(eq + 1));
        if (kind == NewLine && e.text.empty()) e.text = "\n";
        out.push_back(e);
    }
    if (out.empty()) {
        err = "No entry in this table.";
        if (badLines) *badLines = bad;
        return false;
    }
    setEntries(out);
    if (badLines) *badLines = bad;
    return true;
}

std::string CharTable::toTbl() const
{
    std::string out;
    for (size_t i = 0; i < mEntries.size(); i++) {
        const TblEntry &e = mEntries[i];
        if (e.kind == End) out += "/";
        else if (e.kind == NewLine) out += "*";
        out += hexBytes(e.bytes, "");
        if (e.kind == Normal || !(e.text.empty() || (e.kind == NewLine && e.text == "\n")))
            out += "=" + escapeText(e.text);
        out += "\n";
    }
    return out;
}

const TblEntry *CharTable::match(const uint8_t *p, size_t n) const
{
    if (n == 0) return 0;
    const std::vector<int> &v = mByFirst[p[0]];
    for (size_t i = 0; i < v.size(); i++) {
        const TblEntry &e = mEntries[v[i]];
        if (e.bytes.size() <= n && memcmp(&e.bytes[0], p, e.bytes.size()) == 0) return &e;
    }
    return 0;
}

std::string CharTable::decode(const uint8_t *p, size_t n, bool stopAtEnd, size_t *used, int *unknown,
                              size_t *termLen) const
{
    std::string out;
    size_t i = 0;
    int unk = 0;
    if (termLen) *termLen = 0;
    while (i < n) {
        const TblEntry *e = match(p + i, n - i);
        if (!e) {
            static const char *d = "0123456789ABCDEF";
            out += "<$";
            out += d[p[i] >> 4];
            out += d[p[i] & 15];
            out += ">";
            unk++;
            i++;
            continue;
        }
        if (e->kind == End && stopAtEnd) {
            i += e->bytes.size();
            if (termLen) *termLen = e->bytes.size();
            break;
        }
        out += e->text;
        i += e->bytes.size();
    }
    if (used) *used = i;
    if (unknown) *unknown = unk;
    return out;
}

bool CharTable::encode(const std::string &text, std::vector<uint8_t> &out, std::string &err, size_t *errPos) const
{
    out.clear();
    size_t i = 0;
    while (i < text.size()) {
        // raw byte <$XX>
        if (text.compare(i, 2, "<$") == 0 && i + 4 < text.size() && text[i + 4] == '>') {
            const int h = hexVal(text[i + 2]), l = hexVal(text[i + 3]);
            if (h >= 0 && l >= 0) {
                out.push_back((uint8_t)(h << 4 | l));
                i += 5;
                continue;
            }
        }
        bool found = false;
        for (size_t len = std::min(mMaxText, text.size() - i); len > 0 && !found; len--) {
            std::map<std::string, int>::const_iterator it = mByText.find(text.substr(i, len));
            if (it == mByText.end()) continue;
            const TblEntry &e = mEntries[it->second];
            out.insert(out.end(), e.bytes.begin(), e.bytes.end());
            i += len;
            found = true;
        }
        if (!found) {
            size_t clen = 1;
            while (i + clen < text.size() && ((unsigned char)text[i + clen] & 0xC0) == 0x80) clen++;
            err = "No code for \"" + text.substr(i, clen) + "\" in the table.";
            if (errPos) *errPos = i;
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> CharTable::endBytes() const
{
    for (size_t i = 0; i < mEntries.size(); i++)
        if (mEntries[i].kind == End) return mEntries[i].bytes;
    return std::vector<uint8_t>();
}

int CharTable::codeWidth() const
{
    size_t one = 0, two = 0;
    for (size_t i = 0; i < mEntries.size(); i++) {
        if (mEntries[i].kind != Normal) continue;
        if (mEntries[i].bytes.size() == 1) one++;
        else if (mEntries[i].bytes.size() == 2) two++;
    }
    return two > one ? 2 : 1;
}

CharTable asciiTable()
{
    std::vector<TblEntry> v;
    for (int c = 0x20; c < 0x7F; c++) {
        TblEntry e;
        e.bytes.push_back((uint8_t)c);
        e.text = std::string(1, (char)c);
        v.push_back(e);
    }
    TblEntry end;
    end.bytes.push_back(0);
    end.kind = End;
    v.push_back(end);
    CharTable t;
    t.setEntries(v);
    return t;
}

// ======================================================================
//  ISO 9660 (ECMA-119): volume descriptor at LBA 16, root directory
//  record at byte 156; directory records: length (0), extent LBA (2, LE),
//  data length (10, LE), flags (25, bit 1 = directory), name length (32),
//  name (33).
// ======================================================================

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool walkDir(const EditsPatcher::Image &img, int track, uint32_t lba, uint32_t size, const std::string &prefix,
                    int depth, std::vector<IsoFile> &files, std::string &err)
{
    if (depth > 16 || files.size() > 200000) return true;
    if (size > (16u << 20)) size = 16u << 20;
    std::vector<uint8_t> d;
    if (!EditsPatcher::readUserData(img, track, (uint64_t)lba * 2048, size, d, err)) return false;
    size_t p = 0;
    while (p < d.size()) {
        const uint8_t len = d[p];
        if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }
        if (len < 34 || p + len > d.size()) break;
        const uint8_t *r = &d[p];
        const uint32_t elba = le32(r + 2), esize = le32(r + 10);
        const bool dir = (r[25] & 2) != 0;
        const uint8_t nlen = r[32];
        std::string name((const char *)r + 33, nlen);
        p += len;
        if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue;
        const size_t semi = name.find(';');
        if (semi != std::string::npos) name.erase(semi);
        if (!name.empty() && name[name.size() - 1] == '.') name.erase(name.size() - 1);
        const std::string path = prefix + "/" + name;
        if (dir) {
            if (elba != lba && !walkDir(img, track, elba, esize, path, depth + 1, files, err)) return false;
        } else {
            IsoFile f;
            f.path = path;
            f.lba = elba;
            f.size = esize;
            files.push_back(f);
        }
    }
    return true;
}

bool listIsoFiles(const EditsPatcher::Image &img, int track, std::vector<IsoFile> &files, std::string &err)
{
    files.clear();
    std::vector<uint8_t> pvd;
    if (!EditsPatcher::readUserData(img, track, 16 * 2048, 2048, pvd, err)) return false;
    if (pvd[0] != 1 || memcmp(&pvd[1], "CD001", 5) != 0) {
        err = "No ISO 9660 file system in this track.";
        return false;
    }
    const uint8_t *root = &pvd[156];
    if (!walkDir(img, track, le32(root + 2), le32(root + 10), "", 0, files, err)) return false;
    std::sort(files.begin(), files.end(), [](const IsoFile &a, const IsoFile &b) { return a.lba < b.lba; });
    return true;
}

const IsoFile *fileAt(const std::vector<IsoFile> &files, uint64_t offset)
{
    const uint64_t lba = offset / 2048;
    // files are sorted by LBA: last one starting at or before lba
    size_t lo = 0, hi = files.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (files[mid].lba <= lba) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return 0;
    const IsoFile &f = files[lo - 1];
    if (offset < (uint64_t)f.lba * 2048 + f.size) return &f;
    return 0;
}

// ======================================================================
//  Scanning the user data by windows
// ======================================================================

// fn(buf, n, base, procEnd): positions [0, procEnd) must be looked at, the
// data goes on up to n (look-ahead). Returns false to stop.
typedef std::function<bool(const uint8_t *, size_t, uint64_t, size_t)> WindowFn;

uint64_t Source::size() const
{
    return img ? EditsPatcher::userDataSize(*img, track) : (uint64_t)memLen;
}

bool Source::read(uint64_t offset, size_t n, std::vector<uint8_t> &out, std::string &err) const
{
    if (img) return EditsPatcher::readUserData(*img, track, offset, n, out, err);
    if (offset + n > memLen) { err = "Read past the end of the block."; return false; }
    out.assign(mem + offset, mem + offset + n);
    return true;
}

static bool scanWindows(const Source &src, uint64_t start, uint64_t length,
                        size_t lookahead, Progress prog, std::string &err, WindowFn fn)
{
    const uint64_t total = src.size();
    if (total == 0) { err = src.img ? "No data track." : "Empty block."; return false; }
    if (start >= total) return true;
    if (length == 0 || start + length > total) length = total - start;
    const uint64_t end = start + length;
    const size_t chunk = 4u << 20;
    uint64_t pos = start;
    std::vector<uint8_t> buf;
    while (pos < end) {
        const size_t want = (size_t)std::min<uint64_t>(chunk + lookahead, end - pos);
        if (!src.read(pos, want, buf, err)) return false;
        const bool last = pos + want >= end;
        const size_t proc = last ? want : want - lookahead;
        if (!fn(&buf[0], want, pos, proc)) return true;
        pos += proc;
        if (prog && !prog((double)(pos - start) / (double)length)) { err = "Cancelled."; return false; }
    }
    return true;
}

bool searchBytes(const Source &src, uint64_t start, uint64_t length,
                 const std::vector<uint8_t> &pattern, std::vector<uint64_t> &hits, size_t maxHits,
                 Progress prog, std::string &err)
{
    hits.clear();
    if (pattern.empty()) { err = "Nothing to search."; return false; }
    const size_t len = pattern.size();
    return scanWindows(src, start, length, len, prog, err,
        [&](const uint8_t *b, size_t n, uint64_t base, size_t proc) -> bool {
            size_t p = 0;
            while (p < proc) {
                const void *q = memchr(b + p, pattern[0], proc - p);
                if (!q) break;
                p = (size_t)((const uint8_t *)q - b);
                if (p + len <= n && memcmp(b + p, &pattern[0], len) == 0) {
                    hits.push_back(base + p);
                    if (hits.size() >= maxHits) return false;
                }
                p++;
            }
            return true;
        });
}

// ---- relative search ----

static int charClass(char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 2;
    if (c >= '0' && c <= '9') return 3;
    return 0;
}

static char classStart(int k) { return k == 1 ? 'A' : k == 2 ? 'a' : '0'; }
static int classSize(int k) { return k == 3 ? 10 : 26; }

bool relativeSearch(const Source &src, uint64_t start, uint64_t length,
                    const std::string &query, std::vector<RelHit> &hits, size_t maxHits,
                    Progress prog, std::string &err)
{
    hits.clear();
    struct Pair { size_t i, j; int delta; };
    std::vector<Pair> pairs;
    int last[4] = { -1, -1, -1, -1 };
    bool moving = false;
    for (size_t i = 0; i < query.size(); i++) {
        const char c = query[i];
        const int k = charClass(c);
        if (k == 0) {
            if (c != ' ' && c != '?') { err = "Relative search: use letters, digits, and spaces or ? for any code."; return false; }
            continue;
        }
        if (last[k] >= 0) {
            Pair p;
            p.i = (size_t)last[k];
            p.j = i;
            p.delta = c - query[(size_t)last[k]];
            if (p.delta) moving = true;
            pairs.push_back(p);
        }
        last[k] = (int)i;
    }
    if (pairs.size() < 2 || !moving) {
        err = "Relative search needs at least 3 letters of the same kind (A-Z, a-z or 0-9) that are not all the same.";
        return false;
    }
    const size_t qn = query.size();
    const int widths[3] = { 1, 2, -2 };
    return scanWindows(src, start, length, qn * 2, prog, err,
        [&](const uint8_t *b, size_t n, uint64_t base, size_t proc) -> bool {
            for (int wi = 0; wi < 3; wi++) {
                const int w = widths[wi], step = w < 0 ? -w : w;
                const uint32_t mask = step == 1 ? 0xFF : 0xFFFF;
                const size_t span = qn * (size_t)step;
                for (size_t p = 0; p < proc; p++) {
                    if (p + span > n) break;
                    bool ok = true;
                    for (size_t k = 0; k < pairs.size() && ok; k++) {
                        const uint8_t *a = b + p + pairs[k].i * step, *c = b + p + pairs[k].j * step;
                        uint32_t va, vc;
                        if (step == 1) { va = a[0]; vc = c[0]; }
                        else if (w > 0) { va = (a[0] << 8) | a[1]; vc = (c[0] << 8) | c[1]; }
                        else { va = a[0] | (a[1] << 8); vc = c[0] | (c[1] << 8); }
                        if (((vc - va) & mask) != ((uint32_t)pairs[k].delta & mask)) ok = false;
                    }
                    if (!ok) continue;
                    RelHit h;
                    h.offset = base + p;
                    h.width = w;
                    for (size_t i = 0; i < qn; i++) {
                        const uint8_t *a = b + p + i * step;
                        h.values.push_back(step == 1 ? a[0] : w > 0 ? (uint32_t)((a[0] << 8) | a[1]) : (uint32_t)(a[0] | (a[1] << 8)));
                    }
                    hits.push_back(h);
                    if (hits.size() >= maxHits) return false;
                }
            }
            return true;
        });
}

std::vector<TblEntry> relativeProposal(const std::string &query, const RelHit &hit)
{
    std::vector<TblEntry> out;
    bool done[4] = { false, false, false, false };
    const int step = hit.width < 0 ? -hit.width : hit.width;
    const long maxCode = step == 1 ? 0xFF : 0xFFFF;
    for (size_t i = 0; i < query.size() && i < hit.values.size(); i++) {
        const int k = charClass(query[i]);
        if (k == 0 || done[k]) continue;
        done[k] = true;
        const long base = (long)hit.values[i] - (query[i] - classStart(k));
        for (int c = 0; c < classSize(k); c++) {
            const long code = base + c;
            if (code < 0 || code > maxCode) continue;
            TblEntry e;
            if (step == 1) e.bytes.push_back((uint8_t)code);
            else if (hit.width > 0) { e.bytes.push_back((uint8_t)(code >> 8)); e.bytes.push_back((uint8_t)code); }
            else { e.bytes.push_back((uint8_t)code); e.bytes.push_back((uint8_t)(code >> 8)); }
            e.text = std::string(1, (char)(classStart(k) + c));
            out.push_back(e);
        }
    }
    return out;
}

// ---- strings ----

static const size_t kMaxStringBytes = 2048;

bool dumpStrings(const Source &src, uint64_t start, uint64_t length,
                 const CharTable &table, int minChars, bool requireEnd, std::vector<Found> &out,
                 size_t maxStrings, Progress prog, std::string &err)
{
    out.clear();
    if (table.empty()) { err = "The character table is empty."; return false; }
    if (minChars < 1) minChars = 1;
    uint64_t nextAbs = 0;
    return scanWindows(src, start, length, kMaxStringBytes + 16, prog, err,
        [&](const uint8_t *b, size_t n, uint64_t base, size_t proc) -> bool {
            size_t p = nextAbs > base ? (size_t)(nextAbs - base) : 0;
            while (p < proc) {
                size_t q = p, chars = 0, term = 0;
                while (q < n && q - p < kMaxStringBytes) {
                    const TblEntry *e = table.match(b + q, n - q);
                    if (!e) break;
                    if (e->kind == End) { term = e->bytes.size(); break; }
                    chars += e->kind == NewLine ? 1 : std::max<size_t>(1, utf8Length(e->text));
                    q += e->bytes.size();
                }
                if (q > p && (int)chars >= minChars && (!requireEnd || term > 0)) {
                    Found f;
                    f.offset = base + p;
                    f.length = (uint32_t)(q - p);
                    f.termLength = (uint32_t)term;
                    f.text = table.decode(b + p, q - p, false);
                    out.push_back(f);
                    if (out.size() >= maxStrings) return false;
                    p = q + term;
                    nextAbs = base + p;
                } else {
                    p++;
                }
            }
            return true;
        });
}

bool readString(const Source &src, uint64_t offset, const CharTable &table,
                size_t maxBytes, Found &out, std::string &err)
{
    const uint64_t total = src.size();
    if (offset >= total) { err = "Offset past the end of the data."; return false; }
    const size_t n = (size_t)std::min<uint64_t>(maxBytes, total - offset);
    std::vector<uint8_t> b;
    if (!src.read(offset, n, b, err)) return false;
    // up to the end code, or the first byte with no entry
    size_t q = 0, term = 0;
    while (q < n) {
        const TblEntry *e = table.match(&b[q], n - q);
        if (!e) break;
        if (e->kind == End) { term = e->bytes.size(); break; }
        q += e->bytes.size();
    }
    out.offset = offset;
    out.length = (uint32_t)q;
    out.termLength = (uint32_t)term;
    out.text = table.decode(&b[0], q, false);
    return true;
}

// ---- disc image versions ----

bool searchBytes(const EditsPatcher::Image &img, int track, uint64_t start, uint64_t length,
                 const std::vector<uint8_t> &pattern, std::vector<uint64_t> &hits, size_t maxHits,
                 Progress prog, std::string &err)
{
    return searchBytes(Source(img, track), start, length, pattern, hits, maxHits, prog, err);
}

bool relativeSearch(const EditsPatcher::Image &img, int track, uint64_t start, uint64_t length,
                    const std::string &query, std::vector<RelHit> &hits, size_t maxHits,
                    Progress prog, std::string &err)
{
    return relativeSearch(Source(img, track), start, length, query, hits, maxHits, prog, err);
}

bool dumpStrings(const EditsPatcher::Image &img, int track, uint64_t start, uint64_t length,
                 const CharTable &table, int minChars, bool requireEnd, std::vector<Found> &out,
                 size_t maxStrings, Progress prog, std::string &err)
{
    return dumpStrings(Source(img, track), start, length, table, minChars, requireEnd, out, maxStrings, prog, err);
}

bool readString(const EditsPatcher::Image &img, int track, uint64_t offset, const CharTable &table,
                size_t maxBytes, Found &out, std::string &err)
{
    return readString(Source(img, track), offset, table, maxBytes, out, err);
}

// ======================================================================
//  Compression
// ======================================================================

const char *codecName(int codec)
{
    switch (codec) {
    case CodecLzss:     return "LZSS (Okumura, ring 0x20)";
    case CodecLzssZero: return "LZSS (Okumura, ring 0x00)";
    case CodecLzssSize: return "LZSS + size header";
    case CodecPrs:      return "PRS (Sega)";
    default:            return "?";
    }
}

static const int kLzN = 4096, kLzF = 18, kLzThreshold = 2;

static bool lzssDecode(const uint8_t *src, size_t n, uint8_t fill, size_t limit, std::vector<uint8_t> &out,
                       size_t *consumed)
{
    std::vector<uint8_t> ring(kLzN, fill);
    int r = kLzN - kLzF;
    unsigned flags = 0;
    size_t i = 0;
    while (out.size() < limit) {
        if (((flags >>= 1) & 0x100) == 0) {
            if (i >= n) break;
            flags = src[i++] | 0xFF00;
        }
        if (flags & 1) {
            if (i >= n) break;
            const uint8_t c = src[i++];
            out.push_back(c);
            ring[r++] = c;
            r &= kLzN - 1;
        } else {
            if (i + 1 >= n) break;
            const int a = src[i++], b = src[i++];
            const int pos = a | ((b & 0xF0) << 4), len = (b & 0x0F) + kLzThreshold;
            for (int k = 0; k <= len && out.size() < limit; k++) {
                const uint8_t c = ring[(pos + k) & (kLzN - 1)];
                out.push_back(c);
                ring[r++] = c;
                r &= kLzN - 1;
            }
        }
    }
    if (consumed) *consumed = i;
    return true;
}

// LZSS: for each position the nearest copy of every length (hash chains on
// the data already written), then the cheapest parse (literal 9 bits, copy
// 17 bits). The copies use the ring positions the decoder will have.
static std::vector<uint8_t> lzssEncode(const std::vector<uint8_t> &d)
{
    const size_t n = d.size();
    const int maxDist = kLzN - kLzF;
    std::vector<int> head(1 << 16, -1), prev(n, -1);
    // copies at each position: (length reached, nearest distance), lengths
    // growing; every length up to that one is available at that distance
    std::vector<uint32_t> first(n + 1, 0);
    std::vector<std::pair<uint16_t, uint16_t> > cands;
    for (size_t i = 0; i < n; i++) {
        first[i] = (uint32_t)cands.size();
        if (i + 3 <= n) {
            const unsigned h = (d[i] << 8 ^ d[i + 1] << 4 ^ d[i + 2]) & 0xFFFF;
            int have = kLzThreshold;
            for (int p = head[h], tries = 0; p >= 0 && tries < 512 && have < kLzF; p = prev[p], tries++) {
                const int dist = (int)i - p;
                if (dist > maxDist) break;
                int l = 0;
                while (l < kLzF && i + l < n && d[p + l] == d[i + l]) l++;
                if (l > have) { cands.push_back(std::make_pair((uint16_t)l, (uint16_t)dist)); have = l; }
            }
            prev[i] = head[h];
            head[h] = (int)i;
        }
    }
    first[n] = (uint32_t)cands.size();
    std::vector<uint32_t> cost(n + 1, 0xFFFFFFFFu);
    std::vector<int> step(n + 1, 1), stepDist(n + 1, 0);
    cost[n] = 0;
    for (size_t i = n; i-- > 0;) {
        cost[i] = cost[i + 1] + 9;
        step[i] = 1;
        int from = kLzThreshold + 1;
        for (uint32_t c = first[i]; c < first[i + 1]; c++) {
            for (int l = from; l <= cands[c].first; l++) {
                const uint32_t v = cost[i + l] + 17;
                if (v < cost[i]) { cost[i] = v; step[i] = l; stepDist[i] = cands[c].second; }
            }
            from = cands[c].first + 1;
        }
    }
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < n) {
        const size_t flagPos = out.size();
        out.push_back(0);
        uint8_t flags = 0;
        for (int bit = 0; bit < 8 && i < n; bit++) {
            const int l = step[i];
            if (l == 1) {
                flags |= (uint8_t)(1 << bit);
                out.push_back(d[i]);
                i++;
            } else {
                const int dist = stepDist[i];
                const int pos = (int)((kLzN - kLzF + i - dist) & (kLzN - 1));
                out.push_back((uint8_t)(pos & 0xFF));
                out.push_back((uint8_t)(((pos >> 4) & 0xF0) | (l - kLzThreshold - 1)));
                i += (size_t)l;
            }
        }
        out[flagPos] = flags;
    }
    return out;
}

static bool prsDecode(const uint8_t *src, size_t n, size_t limit, std::vector<uint8_t> &out, size_t *consumed,
                      std::string &err)
{
    size_t i = 0;
    int bits = 0;
    unsigned ctrl = 0;
    bool overrun = false;
    auto bit = [&]() -> int {
        if (bits == 0) {
            if (i >= n) { overrun = true; return 0; }
            ctrl = src[i++];
            bits = 8;
        }
        const int b = (int)(ctrl & 1);
        ctrl >>= 1;
        bits--;
        return b;
    };
    for (;;) {
        if (out.size() > limit) { err = "Unpacked data too big."; return false; }
        if (bit()) {
            if (overrun || i >= n) { err = "Data cut short."; return false; }
            out.push_back(src[i++]);
            continue;
        }
        int len;
        long off;
        if (bit()) {
            if (overrun || i + 1 >= n) { err = "Data cut short."; return false; }
            const unsigned w = src[i] | (src[i + 1] << 8);
            i += 2;
            if (w == 0) break;
            off = (long)(w >> 3) - 0x2000;
            len = (int)(w & 7);
            if (len == 0) {
                if (i >= n) { err = "Data cut short."; return false; }
                len = src[i++] + 1;
            } else {
                len += 2;
            }
        } else {
            len = bit() << 1;
            len |= bit();
            len += 2;
            if (overrun || i >= n) { err = "Data cut short."; return false; }
            off = (long)src[i++] - 0x100;
        }
        if (overrun) { err = "Data cut short."; return false; }
        if ((long)out.size() + off < 0) { err = "Copy before the start of the data."; return false; }
        const size_t from = out.size() + off;
        for (int k = 0; k < len; k++) out.push_back(out[from + k]);
    }
    if (consumed) *consumed = i;
    return true;
}

// PRS: nearest copy of every length at each position, then the cheapest
// parse in bits: literal 9, short copy (2-5 bytes, 256 back) 12, long copy
// (3-9 bytes, 8191 back) 18, long copy with a length byte (1-256) 26.
static std::vector<uint8_t> prsEncode(const std::vector<uint8_t> &d)
{
    const size_t n = d.size();
    const int maxLen = 256;
    std::vector<int> head(1 << 16, -1), prev(n, -1);
    std::vector<uint32_t> first(n + 1, 0);
    std::vector<std::pair<uint16_t, uint16_t> > cands;
    for (size_t i = 0; i < n; i++) {
        first[i] = (uint32_t)cands.size();
        if (i + 2 <= n) {
            const unsigned h = (d[i] << 8 | d[i + 1]) & 0xFFFF;
            int have = 1;
            for (int p = head[h], tries = 0; p >= 0 && tries < 256 && have < maxLen; p = prev[p], tries++) {
                const int dist = (int)i - p;
                if (dist > 0x1FFF) break;
                int l = 0;
                while (l < maxLen && i + l < n && d[p + l] == d[i + l]) l++;
                if (l > have) { cands.push_back(std::make_pair((uint16_t)l, (uint16_t)dist)); have = l; }
            }
            prev[i] = head[h];
            head[h] = (int)i;
        }
    }
    first[n] = (uint32_t)cands.size();
    std::vector<uint32_t> cost(n + 1, 0xFFFFFFFFu);
    std::vector<int> step(n + 1, 1), stepDist(n + 1, 0);
    cost[n] = 0;
    for (size_t i = n; i-- > 0;) {
        cost[i] = cost[i + 1] + 9;
        step[i] = 1;
        int from = 2;
        for (uint32_t c = first[i]; c < first[i + 1]; c++) {
            const int dist = cands[c].second;
            for (int l = from; l <= cands[c].first; l++) {
                uint32_t v;
                if (l <= 5 && dist <= 256) v = 12;
                else if (l < 3) continue;
                else if (l <= 9) v = 18;
                else v = 26;
                v += cost[i + l];
                if (v < cost[i]) { cost[i] = v; step[i] = l; stepDist[i] = dist; }
            }
            from = cands[c].first + 1;
        }
    }
    std::vector<uint8_t> out;
    size_t ctrlPos = 0;
    int bits = 8;
    auto putBit = [&](int b) {
        if (bits == 8) { ctrlPos = out.size(); out.push_back(0); bits = 0; }
        if (b) out[ctrlPos] |= (uint8_t)(1 << bits);
        bits++;
    };
    size_t i = 0;
    while (i < n) {
        const int l = step[i];
        if (l == 1) {
            putBit(1);
            out.push_back(d[i]);
            i++;
            continue;
        }
        const int dist = stepDist[i];
        if (l <= 5 && dist <= 256) {
            putBit(0); putBit(0);
            putBit(((l - 2) >> 1) & 1); putBit((l - 2) & 1);
            out.push_back((uint8_t)(0x100 - dist));
        } else {
            putBit(0); putBit(1);
            const unsigned offField = (unsigned)(0x2000 - dist) << 3;
            if (l <= 9) {
                const unsigned w = offField | (unsigned)(l - 2);
                out.push_back((uint8_t)w); out.push_back((uint8_t)(w >> 8));
            } else {
                out.push_back((uint8_t)offField); out.push_back((uint8_t)(offField >> 8));
                out.push_back((uint8_t)(l - 1));
            }
        }
        i += (size_t)l;
    }
    putBit(0); putBit(1);
    out.push_back(0); out.push_back(0);
    return out;
}

bool decompress(int codec, const uint8_t *src, size_t n, std::vector<uint8_t> &out, size_t *consumed,
                size_t maxOut, std::string &err)
{
    out.clear();
    if (consumed) *consumed = 0;
    switch (codec) {
    case CodecLzss:     return lzssDecode(src, n, 0x20, maxOut, out, consumed);
    case CodecLzssZero: return lzssDecode(src, n, 0x00, maxOut, out, consumed);
    case CodecLzssSize: {
        if (n < 4) { err = "No size header."; return false; }
        const uint32_t size = ((uint32_t)src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
        if (size == 0 || size > maxOut) { err = "Size header out of range."; return false; }
        size_t used = 0;
        lzssDecode(src + 4, n - 4, 0x20, size, out, &used);
        if (out.size() != size) { err = "Data cut short."; return false; }
        if (consumed) *consumed = used + 4;
        return true;
    }
    case CodecPrs: return prsDecode(src, n, maxOut, out, consumed, err);
    default: err = "Unknown format."; return false;
    }
}

std::vector<uint8_t> compress(int codec, const std::vector<uint8_t> &data)
{
    switch (codec) {
    case CodecLzss:
    case CodecLzssZero:
        return lzssEncode(data);
    case CodecLzssSize: {
        std::vector<uint8_t> out(4);
        out[0] = (uint8_t)(data.size() >> 24); out[1] = (uint8_t)(data.size() >> 16);
        out[2] = (uint8_t)(data.size() >> 8);  out[3] = (uint8_t)data.size();
        const std::vector<uint8_t> body = lzssEncode(data);
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    case CodecPrs: return prsEncode(data);
    default: return data;
    }
}

std::vector<PackedBlock> findPrsBlocks(const uint8_t *data, size_t n, size_t minSize, size_t maxBlocks,
                                       Progress prog)
{
    std::vector<PackedBlock> out;
    std::vector<uint8_t> tmp;
    std::string err;
    size_t p = 0;
    while (p < n && out.size() < maxBlocks) {
        size_t used = 0;
        tmp.clear();
        if (prsDecode(data + p, n - p, 4u << 20, tmp, &used, err) && tmp.size() >= minSize && used >= 16 &&
            tmp.size() >= used) {
            PackedBlock b;
            b.offset = p;
            b.packedSize = (uint32_t)used;
            b.size = (uint32_t)tmp.size();
            out.push_back(b);
            p += used;
            continue;
        }
        p++;
        if (prog && (p & 0xFFFF) == 0 && !prog((double)p / (double)n)) break;
    }
    return out;
}

// ======================================================================
//  Pointers
// ======================================================================

uint32_t readPtr(const uint8_t *p, const PtrFormat &f)
{
    if (f.width == 2) return f.bigEndian ? (uint32_t)(p[0] << 8 | p[1]) : (uint32_t)(p[1] << 8 | p[0]);
    return f.bigEndian ? ((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3])
                       : ((uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]);
}

void writePtr(uint8_t *p, const PtrFormat &f, uint32_t v)
{
    if (f.width == 2) {
        if (f.bigEndian) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
        else { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
        return;
    }
    if (f.bigEndian) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
    else { p[3] = (uint8_t)(v >> 24); p[2] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[0] = (uint8_t)v; }
}

bool ptrMatches(uint32_t value, const PtrFormat &f, uint32_t want)
{
    if (value == want) return true;
    // 32-bit SH-2 addresses: 0x0xxxxxxx and its cache-through mirror 0x2xxxxxxx
    return f.width == 4 && (value & 0xD0000000) == 0 && (want & 0xD0000000) == 0 && (value & 0x0FFFFFFF) == (want & 0x0FFFFFFF);
}

uint32_t ptrRetarget(uint32_t oldValue, const PtrFormat &f, uint32_t newValue)
{
    if (f.width == 4 && (oldValue & 0xF0000000) == 0x20000000 && (newValue & 0xF0000000) == 0)
        return newValue | 0x20000000;
    return newValue;
}

std::vector<std::pair<int64_t, int> > guessPointerBase(const uint8_t *data, size_t n,
                                                       const std::vector<uint64_t> &offsets, int width,
                                                       bool bigEndian, size_t maxResults)
{
    std::vector<std::pair<int64_t, int> > out;
    if (offsets.empty() || n < (size_t)width) return out;
    PtrFormat f;
    f.width = width;
    f.bigEndian = bigEndian;
    // Distinct word values only (a fill of one value must not win), then the
    // bases they give with the first strings; a base scores the number of
    // different strings it reaches.
    std::vector<int64_t> values;
    for (size_t p = 0; p + width <= n; p += width) {
        int64_t v = readPtr(data + p, f);
        if (width == 4 && (v & 0xF0000000) == 0x20000000) v &= 0x0FFFFFFF;   // SH-2 cache-through mirror
        if (v != 0) values.push_back(v);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::map<int64_t, int> votes;
    const size_t sample = std::min<size_t>(offsets.size(), 48);
    for (size_t i = 0; i < values.size(); i++)
        for (size_t k = 0; k < sample; k++) {
            const int64_t base = values[i] - (int64_t)offsets[k];
            if (width == 4 && base < 0) continue;
            votes[base]++;
        }
    std::vector<std::pair<int, int64_t> > ranked;
    for (std::map<int64_t, int>::const_iterator it = votes.begin(); it != votes.end(); ++it)
        if (it->second >= 2) ranked.push_back(std::make_pair(it->second, it->first));
    std::sort(ranked.begin(), ranked.end(), [](const std::pair<int, int64_t> &a, const std::pair<int, int64_t> &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    if (ranked.size() > 32) ranked.resize(32);
    std::vector<uint64_t> sorted(offsets);
    std::sort(sorted.begin(), sorted.end());
    for (size_t c = 0; c < ranked.size(); c++) {
        const int64_t base = ranked[c].second;
        int hits = 0;
        for (size_t i = 0; i < values.size(); i++) {
            const int64_t s = values[i] - base;
            if (s >= 0 && std::binary_search(sorted.begin(), sorted.end(), (uint64_t)s)) hits++;
        }
        out.push_back(std::make_pair(base, hits));
    }
    std::sort(out.begin(), out.end(), [](const std::pair<int64_t, int> &a, const std::pair<int64_t, int> &b) {
        return a.second > b.second;
    });
    if (out.size() > maxResults) out.resize(maxResults);
    return out;
}

std::vector<uint64_t> findPointers(const uint8_t *data, size_t n, const PtrFormat &f, uint64_t offset)
{
    std::vector<uint64_t> out;
    const int64_t want = f.base + (int64_t)offset;
    if (want < 0) return out;
    for (size_t p = 0; p + f.width <= n; p += f.width)
        if (ptrMatches(readPtr(data + p, f), f, (uint32_t)want)) out.push_back(p);
    return out;
}

std::vector<std::pair<uint64_t, uint64_t> > findFreeSpace(const uint8_t *data, size_t n, size_t minLen)
{
    std::vector<std::pair<uint64_t, uint64_t> > out;
    size_t p = 0;
    while (p < n) {
        if (data[p] != 0x00 && data[p] != 0xFF) { p++; continue; }
        size_t q = p;
        while (q < n && data[q] == data[p]) q++;
        if (q - p >= minLen) {
            // keep the first bytes: they may end the data before the run
            const uint64_t a = (p + 4 + 3) & ~(uint64_t)3;
            if (a < q) out.push_back(std::make_pair(a, (uint64_t)q));
        }
        p = q;
    }
    return out;
}

bool buildReplacement(const CharTable &table, const std::string &translation, uint32_t capacity,
                      const std::vector<uint8_t> &term, const std::vector<uint8_t> &pad,
                      std::vector<uint8_t> &out, int *overflow, std::string &err)
{
    if (overflow) *overflow = 0;
    std::vector<uint8_t> enc;
    if (!table.encode(translation, enc, err)) return false;
    if (enc.size() > capacity) {
        if (overflow) *overflow = (int)(enc.size() - capacity);
        err = "Too long by " + std::to_string(enc.size() - capacity) + " byte(s).";
        return false;
    }
    out = enc;
    const size_t total = capacity + term.size();
    // the end code right after the text (shorter text), then the padding
    if (out.size() < total) out.insert(out.end(), term.begin(), term.end());
    const std::vector<uint8_t> fill = pad.empty() ? (term.empty() ? std::vector<uint8_t>(1, 0) : term) : pad;
    size_t k = 0;
    while (out.size() < total) out.push_back(fill[k++ % fill.size()]);
    out.resize(total);
    return true;
}

} // namespace TextTools
