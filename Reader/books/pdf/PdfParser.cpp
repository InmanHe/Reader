#include "PdfParser.h"
#include "PdfCrypt.h"
#include "miniz.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace pdf {
namespace {

struct GlyphName { const char* name; uint16_t uni; };
#include "PdfGlyphs.inc"

const size_t kMaxStream  = 256u << 20;   // zip-bomb guard
const int    kMaxDepth   = 64;           // nesting guard for objects / trees
const int    kMaxXObject = 8;            // form xobject recursion guard

/////////////////////////////////////////////
// objects
struct Obj
{
    enum Type : uint8_t { NONE, BOOL, NUM, STR, NAME, ARR, DICT, REF, STREAM, KW };
    Type   t = NONE;
    bool   b = false;
    bool   isInt = false;
    double n = 0;
    int    num = 0, gen = 0;            // REF target, or the object id of a STREAM
    std::string s;                      // STR bytes, NAME, KW
    std::vector<std::string> keys;      // DICT / STREAM
    std::vector<Obj> items;             // ARR items or DICT values
    size_t soff = 0, slen = 0;          // STREAM raw data position in the file

    bool isDict() const { return t == DICT || t == STREAM; }
    bool isName(const char* v) const { return t == NAME && s == v; }
    int  i() const { return t == NUM ? (int)n : 0; }
    const Obj* find(const char* k) const
    {
        if (!isDict()) return nullptr;
        for (size_t x = 0; x < keys.size(); x++) if (keys[x] == k) return &items[x];
        return nullptr;
    }
};
const Obj kNull = Obj();

inline bool isWs(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
inline bool isDelim(uint8_t c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
inline int hexVal(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/////////////////////////////////////////////
// lexer / object parser
struct Lexer
{
    const uint8_t* b;
    const uint8_t* p;
    const uint8_t* e;
    bool refs = true; // recognise "n g R" (off for content streams)

    Lexer(const void* data, size_t size, size_t off = 0)
        : b((const uint8_t*)data), p((const uint8_t*)data + (off < size ? off : size)), e((const uint8_t*)data + size) {}

    size_t pos() const { return (size_t)(p - b); }

    void skipWs()
    {
        while (p < e)
        {
            if (isWs(*p)) p++;
            else if (*p == '%') { while (p < e && *p != '\r' && *p != '\n') p++; }
            else break;
        }
    }

    bool startsWith(const char* kw) const
    {
        size_t n = strlen(kw);
        return (size_t)(e - p) >= n && memcmp(p, kw, n) == 0;
    }

    bool readInt(long long& v)
    {
        skipWs();
        const uint8_t* s = p;
        bool neg = false;
        if (p < e && (*p == '+' || *p == '-')) neg = *p++ == '-';
        if (p >= e || *p < '0' || *p > '9') { p = s; return false; }
        v = 0;
        while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (neg) v = -v;
        return true;
    }

    bool number(Obj& o)
    {
        const uint8_t* s = p;
        bool neg = false, dot = false;
        double v = 0, frac = 0.1;
        if (*p == '+' || *p == '-') neg = *p++ == '-';
        while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
        {
            if (*p == '.') { if (dot) break; dot = true; }
            else if (*p == '-') { /* "1--2" garbage: ignore */ }
            else if (!dot) v = v * 10 + (*p - '0');
            else { v += (*p - '0') * frac; frac /= 10; }
            p++;
        }
        if (p == s + (neg || *s == '+' ? 1 : 0) && !dot) { p = s + 1; o.t = Obj::KW; o.s.assign((const char*)s, 1); return true; }
        o.t = Obj::NUM;
        o.n = neg ? -v : v;
        o.isInt = !dot;
        if (refs && o.isInt && !neg && v < 1e9)
        {
            const uint8_t* save = p;
            long long g;
            if (readInt(g) && g >= 0 && g < 65536)
            {
                skipWs();
                if (p < e && *p == 'R' && (p + 1 == e || isWs(p[1]) || isDelim(p[1])))
                {
                    p++;
                    o.t = Obj::REF;
                    o.num = (int)v;
                    o.gen = (int)g;
                    return true;
                }
            }
            p = save;
        }
        return true;
    }

    bool parse(Obj& o, int depth = 0)
    {
        o = Obj();
        skipWs();
        if (p >= e || depth > kMaxDepth) return false;
        uint8_t c = *p;
        if (c == '/')
        {
            p++;
            o.t = Obj::NAME;
            while (p < e && !isWs(*p) && !isDelim(*p))
            {
                if (*p == '#' && p + 2 < e && hexVal(p[1]) >= 0 && hexVal(p[2]) >= 0)
                {
                    o.s += (char)(hexVal(p[1]) * 16 + hexVal(p[2]));
                    p += 3;
                }
                else o.s += (char)*p++;
            }
            return true;
        }
        if (c == '(')
        {
            p++;
            o.t = Obj::STR;
            int level = 1;
            while (p < e)
            {
                uint8_t ch = *p++;
                if (ch == '\\')
                {
                    if (p >= e) break;
                    uint8_t x = *p++;
                    switch (x)
                    {
                    case 'n': o.s += '\n'; break;
                    case 'r': o.s += '\r'; break;
                    case 't': o.s += '\t'; break;
                    case 'b': o.s += '\b'; break;
                    case 'f': o.s += '\f'; break;
                    case '\r': if (p < e && *p == '\n') p++; break;
                    case '\n': break;
                    default:
                        if (x >= '0' && x <= '7')
                        {
                            int v = x - '0';
                            for (int k = 0; k < 2 && p < e && *p >= '0' && *p <= '7'; k++) v = v * 8 + (*p++ - '0');
                            o.s += (char)(v & 0xff);
                        }
                        else o.s += (char)x;
                    }
                }
                else if (ch == '(') { level++; o.s += '('; }
                else if (ch == ')') { if (--level == 0) break; o.s += ')'; }
                else if (ch == '\r') { o.s += '\n'; if (p < e && *p == '\n') p++; }
                else o.s += (char)ch;
            }
            return true;
        }
        if (c == '<')
        {
            if (p + 1 < e && p[1] == '<')
            {
                p += 2;
                o.t = Obj::DICT;
                while (true)
                {
                    skipWs();
                    if (p >= e) return true;
                    if (*p == '>') { p += (p + 1 < e && p[1] == '>') ? 2 : 1; return true; }
                    const uint8_t* save = p;
                    Obj k;
                    if (!parse(k, depth + 1)) return true;
                    if (k.t != Obj::NAME)
                    {
                        if (k.t == Obj::KW && k.s.size() > 1) { p = save; return true; } // "endobj", "stream"...
                        continue;
                    }
                    save = p;
                    Obj v;
                    if (!parse(v, depth + 1)) { o.keys.push_back(k.s); o.items.push_back(Obj()); return true; }
                    if (v.t == Obj::KW && v.s.size() > 1) { p = save; return true; }
                    o.keys.push_back(std::move(k.s));
                    o.items.push_back(std::move(v));
                }
            }
            p++;
            o.t = Obj::STR;
            int hi = -1;
            while (p < e && *p != '>')
            {
                int v = hexVal(*p++);
                if (v < 0) continue;
                if (hi < 0) hi = v;
                else { o.s += (char)(hi * 16 + v); hi = -1; }
            }
            if (hi >= 0) o.s += (char)(hi * 16);
            if (p < e) p++;
            return true;
        }
        if (c == '[')
        {
            p++;
            o.t = Obj::ARR;
            while (true)
            {
                skipWs();
                if (p >= e) return true;
                if (*p == ']') { p++; return true; }
                const uint8_t* save = p;
                Obj v;
                if (!parse(v, depth + 1)) return true;
                if (v.t == Obj::KW && v.s.size() > 1 && (v.s == "endobj" || v.s == "stream" || v.s == "obj")) { p = save; return true; }
                o.items.push_back(std::move(v));
            }
        }
        if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}')
        {
            p++;
            o.t = Obj::KW;
            o.s.assign(1, (char)c);
            return true;
        }
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') return number(o);

        const uint8_t* s = p;
        while (p < e && !isWs(*p) && !isDelim(*p)) p++;
        if (p == s) p++;
        o.s.assign((const char*)s, p - s);
        if (o.s == "true" || o.s == "false") { o.t = Obj::BOOL; o.b = o.s[0] == 't'; o.s.clear(); }
        else if (o.s == "null") { o.t = Obj::NONE; o.s.clear(); }
        else o.t = Obj::KW;
        return true;
    }
};

/////////////////////////////////////////////
// filters
bool inflateData(const std::string& in, std::string& out)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        mz_stream zs;
        memset(&zs, 0, sizeof(zs));
        int rc = attempt == 0 ? mz_inflateInit(&zs) : mz_inflateInit2(&zs, -MZ_DEFAULT_WINDOW_BITS);
        if (rc != MZ_OK) return false;
        zs.next_in = (const unsigned char*)in.data();
        zs.avail_in = (unsigned int)in.size();
        out.clear();
        unsigned char buf[32768];
        int st;
        do
        {
            zs.next_out = buf;
            zs.avail_out = sizeof(buf);
            st = mz_inflate(&zs, MZ_NO_FLUSH);
            out.append((const char*)buf, sizeof(buf) - zs.avail_out);
            if (out.size() > kMaxStream) { st = MZ_DATA_ERROR; break; }
        } while (st == MZ_OK && (zs.avail_in > 0 || zs.avail_out == 0));
        mz_inflateEnd(&zs);
        if (st == MZ_STREAM_END || (st != MZ_DATA_ERROR && !out.empty())) return true;
        if (!out.empty()) return true; // damaged tail: keep what we have
    }
    return false;
}

bool lzwDecode(const std::string& in, std::string& out, int early)
{
    std::vector<std::string> dict;
    auto reset = [&] {
        dict.clear();
        for (int i = 0; i < 256; i++) dict.push_back(std::string(1, (char)i));
        dict.push_back(std::string());
        dict.push_back(std::string());
    };
    reset();
    size_t bitPos = 0, total = in.size() * 8;
    int codeLen = 9, prev = -1;
    out.clear();
    while (bitPos + codeLen <= total)
    {
        int code = 0;
        for (int k = 0; k < codeLen; k++, bitPos++)
            code = (code << 1) | ((in[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
        if (code == 256) { reset(); codeLen = 9; prev = -1; continue; }
        if (code == 257) break;
        std::string entry;
        if (code < (int)dict.size() && code != 256 && code != 257) entry = dict[code];
        else if (code == (int)dict.size() && prev >= 0) entry = dict[prev] + dict[prev][0];
        else break;
        out += entry;
        if (out.size() > kMaxStream) return false;
        if (prev >= 0 && dict.size() < 4096) dict.push_back(dict[prev] + entry[0]);
        prev = code;
        size_t next = dict.size() + early;
        codeLen = next >= 2048 ? 12 : next >= 1024 ? 11 : next >= 512 ? 10 : 9;
    }
    return true;
}

bool asciiHexDecode(const std::string& in, std::string& out)
{
    out.clear();
    int hi = -1;
    for (unsigned char c : in)
    {
        if (c == '>') break;
        int v = hexVal(c);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { out += (char)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) out += (char)(hi * 16);
    return true;
}

bool ascii85Decode(const std::string& in, std::string& out)
{
    out.clear();
    uint32_t tuple = 0;
    int count = 0;
    size_t i = 0;
    if (in.size() >= 2 && in[0] == '<' && in[1] == '~') i = 2;
    for (; i < in.size(); i++)
    {
        unsigned char c = in[i];
        if (c == '~') break;
        if (isWs(c)) continue;
        if (c == 'z' && count == 0) { out.append(4, '\0'); continue; }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85 + (c - '!');
        if (++count == 5)
        {
            for (int k = 3; k >= 0; k--) out += (char)(tuple >> (8 * k));
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1)
    {
        for (int k = count; k < 5; k++) tuple = tuple * 85 + 84;
        for (int k = 0; k < count - 1; k++) out += (char)(tuple >> (24 - 8 * k));
    }
    return true;
}

bool runLengthDecode(const std::string& in, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < in.size();)
    {
        int len = (unsigned char)in[i++];
        if (len == 128) break;
        if (len < 128)
        {
            size_t n = std::min((size_t)len + 1, in.size() - i);
            out.append(in, i, n);
            i += n;
        }
        else if (i < in.size())
        {
            out.append(257 - len, in[i++]);
        }
    }
    return true;
}

bool applyPredictor(std::string& data, const Obj* parms)
{
    if (!parms || !parms->isDict()) return true;
    const Obj* o = parms->find("Predictor");
    int pred = o ? o->i() : 1;
    if (pred < 2) return true;
    o = parms->find("Colors");           int colors  = o && o->i() > 0 ? o->i() : 1;
    o = parms->find("BitsPerComponent"); int bpc     = o && o->i() > 0 ? o->i() : 8;
    o = parms->find("Columns");          int columns = o && o->i() > 0 ? o->i() : 1;
    size_t bpp = std::max(1, colors * bpc / 8);
    size_t rowLen = ((size_t)colors * bpc * columns + 7) / 8;
    if (rowLen == 0) return false;

    if (pred == 2)
    {
        if (bpc != 8) return true;
        for (size_t r = 0; r + rowLen <= data.size(); r += rowLen)
            for (size_t i = bpp; i < rowLen; i++) data[r + i] = (char)(data[r + i] + data[r + i - bpp]);
        return true;
    }

    std::string out;
    out.reserve(data.size());
    std::vector<uint8_t> prev(rowLen, 0), cur(rowLen);
    for (size_t r = 0; r < data.size(); r += rowLen + 1)
    {
        int type = (uint8_t)data[r];
        size_t n = std::min(rowLen, data.size() - r - 1);
        memset(&cur[0], 0, rowLen);
        memcpy(&cur[0], data.data() + r + 1, n);
        for (size_t i = 0; i < rowLen; i++)
        {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0;
            switch (type)
            {
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + (a + b) / 2); break;
            case 4:
            {
                int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                cur[i] = (uint8_t)(cur[i] + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c)));
                break;
            }
            default: break;
            }
        }
        out.append((const char*)&cur[0], n);
        prev.swap(cur);
    }
    data.swap(out);
    return true;
}

/////////////////////////////////////////////
// text helpers
void appendW(std::wstring& w, char32_t c)
{
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return; // invalid code point (broken ToUnicode)
    if (sizeof(wchar_t) == 2 && c > 0xFFFF)
    {
        c -= 0x10000;
        w += (wchar_t)(0xD800 + (c >> 10));
        w += (wchar_t)(0xDC00 + (c & 0x3FF));
    }
    else w += (wchar_t)c;
}

std::u32string utf16beToU32(const std::string& s, size_t start = 0)
{
    std::u32string r;
    for (size_t i = start; i + 1 < s.size(); i += 2)
    {
        char32_t u = ((uint8_t)s[i] << 8) | (uint8_t)s[i + 1];
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < s.size())
        {
            char32_t l = ((uint8_t)s[i + 2] << 8) | (uint8_t)s[i + 3];
            if (l >= 0xDC00 && l < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); i += 2; }
        }
        r += u;
    }
    return r;
}

std::u32string utf8ToU32(const std::string& s, size_t start = 0)
{
    std::u32string r;
    for (size_t i = start; i < s.size();)
    {
        uint8_t c = s[i];
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        char32_t u = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        i++;
        for (int k = 0; k < n && i < s.size(); k++, i++) u = (u << 6) | (s[i] & 0x3F);
        r += u;
    }
    return r;
}

std::u32string decodeCodepage(int cp, const std::string& s)
{
    std::u32string r;
    if (s.empty()) return r;
#ifdef _WIN32
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return r;
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    for (size_t i = 0; i < w.size(); i++)
    {
        char32_t u = (uint16_t)w[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < w.size()) { u = 0x10000 + ((u - 0xD800) << 10) + ((uint16_t)w[i + 1] - 0xDC00); i++; }
        r += u;
    }
#else
    const char* name = cp == 936 ? "GBK" : cp == 950 ? "BIG5" : cp == 932 ? "SHIFT_JIS" : "CP949";
    iconv_t ic = iconv_open("UTF-32LE", name);
    if (ic == (iconv_t)-1) return r;
    std::string in = s;
    std::string out(s.size() * 4 + 16, 0);
    char* ip = &in[0]; size_t il = in.size();
    char* op = &out[0]; size_t ol = out.size();
    while (il > 0)
    {
        if (iconv(ic, &ip, &il, &op, &ol) == (size_t)-1) { ip++; il--; } // skip bad byte
    }
    iconv_close(ic);
    for (char* q = &out[0]; q + 4 <= op; q += 4)
        r += (char32_t)((uint8_t)q[0] | ((uint8_t)q[1] << 8) | ((uint8_t)q[2] << 16) | ((uint32_t)(uint8_t)q[3] << 24));
#endif
    return r;
}

// PDF "text string" (outline titles, document info)
std::wstring textString(const std::string& s)
{
    std::u32string u;
    if (s.size() >= 2 && (uint8_t)s[0] == 0xFE && (uint8_t)s[1] == 0xFF) u = utf16beToU32(s, 2);
    else if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) u = utf8ToU32(s, 3);
    else for (unsigned char c : s) u += (char32_t)(kPdfDocEncoding[c] ? kPdfDocEncoding[c] : c);
    std::wstring w;
    bool space = false;
    for (char32_t c : u)
    {
        if (c < 0x20 || c == 0x3000 || c == 0xA0) { space = !w.empty(); continue; }
        if (space) { w += L' '; space = false; }
        appendW(w, c);
    }
    return w;
}

std::u32string glyphNameToUnicode(const std::string& full)
{
    std::u32string r;
    std::string name = full.substr(0, full.find('.'));
    if (name.empty()) return r;
    if (name.find('_') != std::string::npos) // ligature "f_f_i"
    {
        size_t s = 0;
        while (s <= name.size())
        {
            size_t e = name.find('_', s);
            if (e == std::string::npos) e = name.size();
            r += glyphNameToUnicode(name.substr(s, e - s));
            s = e + 1;
        }
        return r;
    }
    size_t lo = 0, hi = sizeof(kGlyphNames) / sizeof(kGlyphNames[0]);
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(kGlyphNames[mid].name, name.c_str());
        if (c == 0) { r += (char32_t)kGlyphNames[mid].uni; return r; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    auto hexRun = [&](size_t from, size_t len) -> long {
        long v = 0;
        for (size_t i = from; i < from + len; i++) { int h = hexVal(name[i]); if (h < 0) return -1; v = v * 16 + h; }
        return v;
    };
    if (name.size() >= 7 && name.compare(0, 3, "uni") == 0 && (name.size() - 3) % 4 == 0)
    {
        for (size_t i = 3; i < name.size(); i += 4)
        {
            long v = hexRun(i, 4);
            if (v < 0) return std::u32string();
            r += (char32_t)v;
        }
        return r;
    }
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u')
    {
        long v = hexRun(1, name.size() - 1);
        if (v > 0) r += (char32_t)v;
    }
    return r;
}

bool isCjk(char32_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x2FFFF) ||
           c == 0x2014 || c == 0x2026 || c == 0x201C || c == 0x201D || c == 0x2018 || c == 0x2019;
}
inline bool isSpace(char32_t c) { return c == ' ' || c == '\t' || c == 0x3000 || c == 0xA0; }

/////////////////////////////////////////////
// fonts
struct Font
{
    bool cid = false;
    bool identity = false;
    int  fixedBytes = 1;
    struct Space { int bytes; uint32_t lo, hi; };
    std::vector<Space> spaces;
    std::map<uint32_t, std::u32string> uni;
    struct Range { uint32_t lo, hi; std::u32string base; };
    std::vector<Range> ranges;
    bool hasSimple = false;
    char32_t simple[256];
    int  codepage = 0;          // legacy CJK CMaps without ToUnicode
    bool utf16 = false, utf8 = false;
    float dw = 1000, missing = 500, scale = 1;
    int firstChar = 0;
    std::vector<float> widths;
    std::map<uint32_t, float> cidW;

    float width(uint32_t code) const
    {
        if (cid)
        {
            auto it = cidW.find(code);
            return it != cidW.end() ? it->second : dw;
        }
        int idx = (int)code - firstChar;
        if (idx >= 0 && idx < (int)widths.size() && widths[idx] > 0) return widths[idx] * scale;
        return missing * scale;
    }

    bool lookup(uint32_t code, std::u32string& out) const
    {
        auto it = uni.find(code);
        if (it != uni.end()) { out = it->second; return true; }
        if (!ranges.empty())
        {
            auto r = std::upper_bound(ranges.begin(), ranges.end(), code, [](uint32_t c, const Range& x) { return c < x.lo; });
            if (r != ranges.begin())
            {
                --r;
                if (code <= r->hi && !r->base.empty())
                {
                    out = r->base;
                    out.back() += (char32_t)(code - r->lo);
                    return true;
                }
            }
        }
        if (hasSimple && code < 256 && simple[code]) { out.assign(1, simple[code]); return true; }
        return false;
    }

    // split a string into codes
    void codes(const std::string& s, std::vector<std::pair<uint32_t, int>>& out) const
    {
        out.clear();
        for (size_t i = 0; i < s.size();)
        {
            int n = 0;
            uint32_t code = 0;
            if (!spaces.empty())
            {
                uint32_t v = 0;
                for (int k = 1; k <= 4 && i + k <= s.size(); k++)
                {
                    v = (v << 8) | (uint8_t)s[i + k - 1];
                    for (const auto& sp : spaces)
                        if (sp.bytes == k && v >= sp.lo && v <= sp.hi) { n = k; code = v; break; }
                    if (n) break;
                }
                if (!n) { n = std::min((size_t)fixedBytes, s.size() - i); code = 0; for (int k = 0; k < n; k++) code = (code << 8) | (uint8_t)s[i + k]; }
            }
            else
            {
                n = (int)std::min((size_t)fixedBytes, s.size() - i);
                for (int k = 0; k < n; k++) code = (code << 8) | (uint8_t)s[i + k];
            }
            out.push_back(std::make_pair(code, n));
            i += n;
        }
    }
};

struct Mat
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Mat() {}
    Mat(double a_, double b_, double c_, double d_, double e_, double f_) : a(a_), b(b_), c(c_), d(d_), e(e_), f(f_) {}
    // this × m  (apply this first, then m)
    Mat operator*(const Mat& m) const
    {
        return Mat(a * m.a + b * m.c, a * m.b + b * m.d, c * m.a + d * m.c, c * m.b + d * m.d,
                   e * m.a + f * m.c + m.e, e * m.b + f * m.d + m.f);
    }
    void apply(double x, double y, double& ox, double& oy) const { ox = a * x + c * y + e; oy = b * x + d * y + f; }
};

struct Run
{
    double x0, x1, y, size;
    std::u32string t;
};

struct Line
{
    double x0, x1, y, size;
    bool lead;            // starts with blank characters (typed indent)
    std::u32string t;
};

} // namespace

/////////////////////////////////////////////
// document
struct Document::Impl
{
    struct XEntry { uint8_t type = 0; uint32_t a = 0; uint32_t b = 0; };
    struct ObjStm { std::string data; std::vector<std::pair<int, size_t>> index; };
    struct Page { int num; const Obj* dict; const Obj* res; };
    struct GState { Mat ctm; };

    const uint8_t* data = nullptr;
    size_t size = 0;
    const volatile bool* abortFlag = nullptr;

    std::vector<XEntry> xref;
    Obj trailer;
    bool reconstructed = false;
    std::map<int, Obj> cache;
    std::set<int> loading;
    std::map<int, ObjStm> objstms;
    std::vector<int> pendingObjStms;

    // encryption
    bool encrypted = false;
    int  encNum = -1;
    int  encR = 0;
    std::string fileKey;
    int stmMethod = 0, strMethod = 0; // 0 none, 1 rc4, 2 aes

    std::vector<Page> pages;
    std::map<int, int> pageByNum;
    std::vector<Outline> outlines;
    std::wstring title;
    std::map<std::string, const Obj*> namedDests;
    bool namedDestsLoaded = false;
    std::map<const Obj*, std::shared_ptr<Font>> fonts;

    // text state while interpreting a page
    std::vector<GState> gstack;
    Mat ctm, tm, tlm;
    Font* font = nullptr;
    double fontSize = 0, charSpace = 0, wordSpace = 0, hscale = 1, leading = 0, rise = 0;
    std::vector<Run> runs;
    std::set<int> formStack;

    bool aborted() const { return abortFlag && *abortFlag; }

    /////////////////////////////////////////
    // object access
    const Obj& get(int num)
    {
        auto it = cache.find(num);
        if (it != cache.end()) return it->second;
        if (loading.count(num) || num <= 0) return kNull;
        loading.insert(num);
        Obj o;
        bool ok = loadObj(num, o);
        if (!ok && !reconstructed && num < (int)xref.size() && xref[num].type != 0)
        {
            reconstruct();
            ok = loadObj(num, o);
        }
        if (!ok && !pendingObjStms.empty())
        {
            indexPendingObjStms();
            ok = loadObj(num, o);
        }
        loading.erase(num);
        if (!ok) return kNull;
        return cache.insert(std::make_pair(num, std::move(o))).first->second;
    }

    const Obj& resolve(const Obj& o)
    {
        const Obj* r = &o;
        for (int i = 0; i < 8 && r->t == Obj::REF; i++) r = &get(r->num);
        return r->t == Obj::REF ? kNull : *r;
    }
    const Obj& resolve(const Obj* o) { return o ? resolve(*o) : kNull; }
    const Obj& at(const Obj& dict, const char* key) { return resolve(dict.find(key)); }

    bool parseIndirectAt(size_t off, Obj& out, int& num, int& gen)
    {
        if (off >= size) return false;
        Lexer lx(data, size, off);
        lx.refs = false;
        long long n, g;
        if (!lx.readInt(n) || !lx.readInt(g)) return false;
        lx.skipWs();
        if (!lx.startsWith("obj")) return false;
        lx.p += 3;
        lx.refs = true;
        if (!lx.parse(out)) return false;
        num = (int)n;
        gen = (int)g;
        if (out.t != Obj::DICT) return true;
        lx.skipWs();
        if (!lx.startsWith("stream")) return true;
        lx.p += 6;
        if (lx.p < lx.e && *lx.p == '\r') lx.p++;
        if (lx.p < lx.e && *lx.p == '\n') lx.p++;
        size_t start = lx.pos();
        long long len = -1;
        const Obj* l = out.find("Length");
        if (l && l->t == Obj::NUM) len = (long long)l->n;
        else if (l && l->t == Obj::REF && l->num != n)
        {
            const Obj& lv = get(l->num);
            if (lv.t == Obj::NUM) len = (long long)lv.n;
        }
        bool valid = false;
        if (len >= 0 && start + (size_t)len <= size)
        {
            Lexer chk(data, size, start + (size_t)len);
            chk.skipWs();
            valid = chk.startsWith("endstream");
        }
        if (!valid)
        {
            const uint8_t* hit = nullptr;
            for (const uint8_t* q = data + start; q + 9 <= data + size; q++)
                if (*q == 'e' && memcmp(q, "endstream", 9) == 0) { hit = q; break; }
            size_t end = hit ? (size_t)(hit - data) : size;
            while (end > start && (data[end - 1] == '\n' || data[end - 1] == '\r')) end--;
            len = (long long)(end - start);
        }
        out.t = Obj::STREAM;
        out.soff = start;
        out.slen = (size_t)len;
        out.num = num;
        out.gen = gen;
        return true;
    }

    bool loadObj(int num, Obj& out)
    {
        if (num <= 0 || num >= (int)xref.size()) return false;
        const XEntry x = xref[num];
        if (x.type == 1)
        {
            int n, g;
            if (!parseIndirectAt(x.a, out, n, g) || n != num) return false;
            if (encrypted && num != encNum) decryptStrings(out, num, g);
            return true;
        }
        if (x.type == 2)
        {
            ObjStm* stm = loadObjStm((int)x.a);
            if (!stm) return false;
            for (const auto& e : stm->index)
            {
                if (e.first != num) continue;
                Lexer lx(stm->data.data(), stm->data.size(), e.second);
                return lx.parse(out);
            }
        }
        return false;
    }

    ObjStm* loadObjStm(int num)
    {
        auto it = objstms.find(num);
        if (it != objstms.end()) return &it->second;
        const Obj& s = get(num);
        if (s.t != Obj::STREAM) return nullptr;
        ObjStm stm;
        if (!decodeStream(s, stm.data)) return nullptr;
        int n = at(s, "N").i(), first = at(s, "First").i();
        Lexer lx(stm.data.data(), stm.data.size());
        lx.refs = false;
        for (int i = 0; i < n; i++)
        {
            long long on, off;
            if (!lx.readInt(on) || !lx.readInt(off)) break;
            stm.index.push_back(std::make_pair((int)on, (size_t)(first + off)));
        }
        return &objstms.insert(std::make_pair(num, std::move(stm))).first->second;
    }

    /////////////////////////////////////////
    // streams
    bool decodeStream(const Obj& s, std::string& out)
    {
        out.clear();
        if (s.t != Obj::STREAM || s.soff + s.slen > size) return false;
        out.assign((const char*)data + s.soff, s.slen);

        if (encrypted && stmMethod && s.num != encNum)
        {
            const Obj* type = s.find("Type");
            bool skip = type && type->isName("XRef");
            const Obj& filter0 = at(s, "Filter");
            if (filter0.isName("Crypt") || (filter0.t == Obj::ARR && !filter0.items.empty() && filter0.items[0].isName("Crypt"))) skip = true;
            if (!skip) decryptBytes(out, s.num, s.gen, stmMethod);
        }

        const Obj& filter = at(s, "Filter");
        const Obj& parms = at(s, "DecodeParms");
        std::vector<const Obj*> filters, params;
        if (filter.t == Obj::NAME) { filters.push_back(&filter); params.push_back(&parms); }
        else if (filter.t == Obj::ARR)
        {
            for (size_t i = 0; i < filter.items.size(); i++)
            {
                filters.push_back(&resolve(filter.items[i]));
                params.push_back(parms.t == Obj::ARR && i < parms.items.size() ? &resolve(parms.items[i]) : &kNull);
            }
        }
        std::string tmp;
        for (size_t i = 0; i < filters.size(); i++)
        {
            const std::string& f = filters[i]->s;
            bool ok = true;
            if (f == "FlateDecode" || f == "Fl") { ok = inflateData(out, tmp) && applyPredictor(tmp, params[i]); out.swap(tmp); }
            else if (f == "LZWDecode" || f == "LZW")
            {
                const Obj* ec = params[i]->find("EarlyChange");
                ok = lzwDecode(out, tmp, ec ? ec->i() : 1) && applyPredictor(tmp, params[i]);
                out.swap(tmp);
            }
            else if (f == "ASCIIHexDecode" || f == "AHx") { ok = asciiHexDecode(out, tmp); out.swap(tmp); }
            else if (f == "ASCII85Decode" || f == "A85") { ok = ascii85Decode(out, tmp); out.swap(tmp); }
            else if (f == "RunLengthDecode" || f == "RL") { ok = runLengthDecode(out, tmp); out.swap(tmp); }
            else if (f == "Crypt") {}
            else return false; // image codecs: not needed for text
            if (!ok) return false;
        }
        return true;
    }

    /////////////////////////////////////////
    // encryption
    void decryptBytes(std::string& s, int num, int gen, int method)
    {
        if (!method || s.empty()) return;
        if (encR >= 5)
        {
            std::string out;
            crypt::aesDecryptPdf((const uint8_t*)fileKey.data(), fileKey.size(), s, out);
            s.swap(out);
            return;
        }
        std::string k = fileKey;
        k += (char)(num & 0xff); k += (char)((num >> 8) & 0xff); k += (char)((num >> 16) & 0xff);
        k += (char)(gen & 0xff); k += (char)((gen >> 8) & 0xff);
        if (method == 2) k += "sAlT";
        uint8_t h[16];
        crypt::md5((const uint8_t*)k.data(), k.size(), h);
        size_t klen = std::min(fileKey.size() + 5, (size_t)16);
        if (method == 1) crypt::rc4(h, klen, (uint8_t*)&s[0], s.size());
        else
        {
            std::string out;
            crypt::aesDecryptPdf(h, 16, s, out);
            s.swap(out);
        }
    }

    void decryptStrings(Obj& o, int num, int gen, int depth = 0)
    {
        if (depth > kMaxDepth || !strMethod) return;
        if (o.t == Obj::STR) decryptBytes(o.s, num, gen, strMethod);
        else if (o.t == Obj::ARR || o.isDict())
        {
            if (o.isDict())
            {
                const Obj* type = o.find("Type");
                if (type && type->isName("XRef")) return;
            }
            for (auto& i : o.items) decryptStrings(i, num, gen, depth + 1);
        }
    }

    static std::string padPassword(const std::string& pw)
    {
        static const uint8_t PAD[32] = {
            0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
            0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A };
        std::string r = pw.substr(0, 32);
        r.append((const char*)PAD, 32 - r.size());
        return r;
    }

    // ISO 32000-2 algorithm 2.B
    static std::string hashR6(const std::string& pw, const std::string& salt, const std::string& udata, int r)
    {
        std::string in = pw + salt + udata;
        uint8_t k[64];
        crypt::sha256((const uint8_t*)in.data(), in.size(), k);
        if (r < 6) return std::string((const char*)k, 32);
        size_t klen = 32;
        for (int round = 0;; round++)
        {
            std::string k1;
            std::string unit = pw + std::string((const char*)k, klen) + udata;
            for (int i = 0; i < 64; i++) k1 += unit;
            crypt::aesCbcEncrypt(k, 16, k + 16, (uint8_t*)&k1[0], k1.size());
            int sum = 0;
            for (int i = 0; i < 16; i++) sum += (uint8_t)k1[i];
            switch (sum % 3)
            {
            case 0: crypt::sha256((const uint8_t*)k1.data(), k1.size(), k); klen = 32; break;
            case 1: crypt::sha384((const uint8_t*)k1.data(), k1.size(), k); klen = 48; break;
            default: crypt::sha512((const uint8_t*)k1.data(), k1.size(), k); klen = 64; break;
            }
            if (round >= 63 && (uint8_t)k1.back() <= round - 31) break;
        }
        return std::string((const char*)k, 32);
    }

    static int cryptMethod(const Obj& cfm)
    {
        if (cfm.isName("V2")) return 1;
        if (cfm.isName("AESV2") || cfm.isName("AESV3")) return 2;
        return 0;
    }

    bool setupEncryption()
    {
        const Obj* encRef = trailer.find("Encrypt");
        if (!encRef) return true;
        if (encRef->t == Obj::REF) encNum = encRef->num;
        const Obj& enc = resolve(*encRef);
        if (!enc.isDict()) return true;
        if (!at(enc, "Filter").isName("Standard")) return false;

        int v = at(enc, "V").i();
        int r = at(enc, "R").i();
        int lengthBits = at(enc, "Length").i();
        if (lengthBits <= 0) lengthBits = 40;
        std::string O = at(enc, "O").s, U = at(enc, "U").s;
        int32_t P = (int32_t)(long long)at(enc, "P").n;
        const Obj& em = at(enc, "EncryptMetadata");
        bool encMeta = em.t != Obj::BOOL || em.b;
        std::string id0;
        const Obj& ids = at(trailer, "ID");
        if (ids.t == Obj::ARR && !ids.items.empty()) id0 = resolve(ids.items[0]).s;

        encR = r;
        stmMethod = strMethod = 1;
        if (v >= 4)
        {
            const Obj& cf = at(enc, "CF");
            auto method = [&](const char* key) {
                const Obj& name = at(enc, key);
                if (name.t != Obj::NAME || name.s == "Identity") return 0;
                const Obj& f = at(cf, name.s.c_str());
                return f.isDict() ? cryptMethod(at(f, "CFM")) : 0;
            };
            stmMethod = method("StmF");
            strMethod = method("StrF");
        }

        if (r >= 5)
        {
            if (U.size() < 48 || O.size() < 48) return false;
            std::string empty;
            std::string ue = at(enc, "UE").s, oe = at(enc, "OE").s;
            std::string key;
            if (hashR6(empty, U.substr(32, 8), "", r) == U.substr(0, 32))
                key = hashR6(empty, U.substr(40, 8), "", r), fileKey = ue;
            else if (hashR6(empty, O.substr(32, 8), U.substr(0, 48), r) == O.substr(0, 32))
                key = hashR6(empty, O.substr(40, 8), U.substr(0, 48), r), fileKey = oe;
            else return false;
            if (fileKey.size() < 32) return false;
            fileKey.resize(32);
            uint8_t iv[16] = { 0 };
            crypt::aesCbcDecrypt((const uint8_t*)key.data(), 32, iv, (uint8_t*)&fileKey[0], 32);
            encrypted = true;
            return true;
        }

        size_t n = r == 2 ? 5 : (size_t)std::min(16, std::max(5, lengthBits / 8));
        auto computeKey = [&](const std::string& pw) {
            std::string in = padPassword(pw) + O.substr(0, 32);
            in += (char)(P & 0xff); in += (char)((P >> 8) & 0xff); in += (char)((P >> 16) & 0xff); in += (char)((P >> 24) & 0xff);
            in += id0;
            if (r >= 4 && !encMeta) in.append(4, (char)0xff);
            uint8_t h[16];
            crypt::md5((const uint8_t*)in.data(), in.size(), h);
            if (r >= 3) for (int i = 0; i < 50; i++) crypt::md5(h, n, h);
            return std::string((const char*)h, n);
        };
        auto checkUser = [&](const std::string& key) {
            if (r == 2)
            {
                std::string t = padPassword("");
                crypt::rc4((const uint8_t*)key.data(), key.size(), (uint8_t*)&t[0], 32);
                return U.size() >= 32 && t == U.substr(0, 32);
            }
            std::string in = padPassword("") + id0;
            uint8_t h[16];
            crypt::md5((const uint8_t*)in.data(), in.size(), h);
            for (int i = 0; i < 20; i++)
            {
                std::string k = key;
                for (auto& c : k) c ^= (char)i;
                crypt::rc4((const uint8_t*)k.data(), k.size(), h, 16);
            }
            return U.size() >= 16 && memcmp(h, U.data(), 16) == 0;
        };

        std::string key = computeKey("");
        if (!checkUser(key))
        {
            // empty owner password: recover the user password from /O
            uint8_t h[16];
            std::string pad = padPassword("");
            crypt::md5((const uint8_t*)pad.data(), pad.size(), h);
            if (r >= 3) for (int i = 0; i < 50; i++) crypt::md5(h, 16, h);
            std::string upw = O.substr(0, 32);
            if (r == 2) crypt::rc4(h, n, (uint8_t*)&upw[0], upw.size());
            else
            {
                for (int i = 19; i >= 0; i--)
                {
                    uint8_t k[16];
                    for (size_t j = 0; j < n; j++) k[j] = h[j] ^ (uint8_t)i;
                    crypt::rc4(k, n, (uint8_t*)&upw[0], upw.size());
                }
            }
            key = computeKey(upw);
            if (!checkUser(key)) return false;
        }
        fileKey = key;
        encrypted = true;
        return true;
    }

    /////////////////////////////////////////
    // cross reference
    void setEntry(size_t num, uint8_t type, uint32_t a, uint32_t b)
    {
        if (num >= (1u << 23)) return;
        if (num >= xref.size()) xref.resize(num + 1);
        if (xref[num].type == 0) { xref[num].type = type; xref[num].a = a; xref[num].b = b; }
    }

    void mergeTrailer(const Obj& t)
    {
        if (!t.isDict()) return;
        if (trailer.t == Obj::NONE) { trailer = t; trailer.t = Obj::DICT; return; }
        for (size_t i = 0; i < t.keys.size(); i++)
        {
            if (!trailer.find(t.keys[i].c_str())) { trailer.keys.push_back(t.keys[i]); trailer.items.push_back(t.items[i]); }
        }
    }

    bool readXrefAt(size_t off, Obj& tr)
    {
        Lexer lx(data, size, off);
        lx.refs = false;
        lx.skipWs();
        if (lx.startsWith("xref"))
        {
            lx.p += 4;
            while (true)
            {
                lx.skipWs();
                if (lx.p >= lx.e) return false;
                if (lx.startsWith("trailer"))
                {
                    lx.p += 7;
                    lx.refs = true;
                    return lx.parse(tr) && tr.t == Obj::DICT;
                }
                long long start, count;
                if (!lx.readInt(start) || !lx.readInt(count) || start < 0 || count < 0 || count > (1 << 23)) return false;
                for (long long i = 0; i < count; i++)
                {
                    long long a, b;
                    if (!lx.readInt(a) || !lx.readInt(b)) return false;
                    lx.skipWs();
                    if (lx.p >= lx.e) return false;
                    char kind = (char)*lx.p++;
                    if (i == 0 && start == 1 && a == 0 && b == 65535 && kind == 'f') start = 0; // common off-by-one
                    if (kind == 'n' && a > 0) setEntry((size_t)(start + i), 1, (uint32_t)a, (uint32_t)b);
                }
            }
        }

        int num, gen;
        Obj s;
        if (!parseIndirectAt(off, s, num, gen) || s.t != Obj::STREAM) return false;
        std::string d;
        if (!decodeStream(s, d)) return false;
        const Obj& w = at(s, "W");
        if (w.t != Obj::ARR || w.items.size() < 3) return false;
        int w0 = resolve(w.items[0]).i(), w1 = resolve(w.items[1]).i(), w2 = resolve(w.items[2]).i();
        int rowLen = w0 + w1 + w2;
        if (rowLen <= 0 || w0 > 4 || w1 > 8 || w2 > 8) return false;
        std::vector<long long> index;
        const Obj& idx = at(s, "Index");
        if (idx.t == Obj::ARR) for (const auto& i : idx.items) index.push_back((long long)resolve(i).n);
        else { index.push_back(0); index.push_back(at(s, "Size").i()); }
        size_t pos = 0;
        auto field = [&](int width, size_t def) -> size_t {
            if (!width) return def;
            size_t v = 0;
            for (int k = 0; k < width; k++) v = (v << 8) | (uint8_t)d[pos++];
            return v;
        };
        for (size_t i = 0; i + 1 < index.size(); i += 2)
        {
            for (long long j = 0; j < index[i + 1]; j++)
            {
                if (pos + rowLen > d.size()) break;
                size_t type = field(w0, 1), a = field(w1, 0), b = field(w2, 0);
                if (type == 1 || type == 2) setEntry((size_t)(index[i] + j), (uint8_t)type, (uint32_t)a, (uint32_t)b);
            }
        }
        tr = s;
        tr.t = Obj::DICT;
        return true;
    }

    bool readXref()
    {
        const char* tail = "startxref";
        size_t from = size > 4096 ? size - 4096 : 0;
        long long sx = -1;
        for (size_t i = size >= 9 ? size - 9 : 0; i + 1 > from && i < size; i--)
        {
            if (memcmp(data + i, tail, 9) == 0)
            {
                Lexer lx(data, size, i + 9);
                if (!lx.readInt(sx)) sx = -1;
                break;
            }
            if (i == 0) break;
        }
        if (sx < 0) return false;

        size_t hdr = 0;
        for (size_t i = 0; i + 5 <= std::min(size, (size_t)1024); i++)
            if (memcmp(data + i, "%PDF-", 5) == 0) { hdr = i; break; }

        std::vector<size_t> todo(1, (size_t)sx);
        std::set<size_t> seen;
        bool any = false;
        while (!todo.empty() && seen.size() < 512)
        {
            size_t off = todo.back();
            todo.pop_back();
            if (seen.count(off)) continue;
            seen.insert(off);
            Obj tr;
            if (!readXrefAt(off, tr) && !(hdr && readXrefAt(off + hdr, tr))) continue;
            any = true;
            mergeTrailer(tr);
            const Obj* prev = tr.find("Prev");
            if (prev && prev->t == Obj::NUM && prev->n >= 0) todo.push_back((size_t)prev->n);
            const Obj* stm = tr.find("XRefStm");
            if (stm && stm->t == Obj::NUM && stm->n >= 0) todo.push_back((size_t)stm->n);
        }
        return any && trailer.find("Root");
    }

    void reconstruct()
    {
        reconstructed = true;
        xref.clear();
        std::vector<int> objstmCandidates;
        Obj lastTrailer;
        for (size_t i = 0; i + 3 <= size; i++)
        {
            if (data[i] == 't' && i + 7 <= size && memcmp(data + i, "trailer", 7) == 0)
            {
                Lexer lx(data, size, i + 7);
                Obj t;
                if (lx.parse(t) && t.t == Obj::DICT && t.find("Root")) lastTrailer = t;
                continue;
            }
            if (data[i] != 'o' || memcmp(data + i, "obj", 3) != 0) continue;
            if (i + 3 < size && !isWs(data[i + 3]) && !isDelim(data[i + 3])) continue;
            size_t j = i;
            while (j > 0 && isWs(data[j - 1])) j--;
            size_t ge = j;
            while (j > 0 && data[j - 1] >= '0' && data[j - 1] <= '9') j--;
            if (j == ge) continue;
            size_t gs = j;
            while (j > 0 && isWs(data[j - 1])) j--;
            if (j == gs) continue;
            size_t ne = j;
            while (j > 0 && data[j - 1] >= '0' && data[j - 1] <= '9') j--;
            if (j == ne || (j > 0 && !isWs(data[j - 1]) && !isDelim(data[j - 1]))) continue;
            long num = strtol((const char*)data + j, nullptr, 10);
            long gen = strtol((const char*)data + gs, nullptr, 10);
            if (num <= 0 || num >= (1 << 23)) continue;
            if ((size_t)num >= xref.size()) xref.resize(num + 1);
            xref[num].type = 1;
            xref[num].a = (uint32_t)j;
            xref[num].b = (uint32_t)gen;
        }
        trailer = Obj();
        if (lastTrailer.t == Obj::DICT) mergeTrailer(lastTrailer);
        for (size_t num = 1; num < xref.size(); num++)
        {
            if (xref[num].type != 1) continue;
            Obj o;
            int n, g;
            if (!parseIndirectAt(xref[num].a, o, n, g) || !o.isDict()) continue;
            const Obj* type = o.find("Type");
            if (!type) continue;
            if (type->isName("ObjStm")) pendingObjStms.push_back((int)num);
            else if (type->isName("XRef") && o.find("Root")) { Obj t = o; t.t = Obj::DICT; mergeTrailer(t); }
            else if (type->isName("Catalog") && !trailer.find("Root"))
            {
                Obj ref; ref.t = Obj::REF; ref.num = (int)num; ref.gen = g;
                if (trailer.t == Obj::NONE) trailer.t = Obj::DICT;
                trailer.keys.push_back("Root");
                trailer.items.push_back(ref);
            }
        }
    }

    void indexPendingObjStms()
    {
        std::vector<int> list;
        list.swap(pendingObjStms);
        for (int num : list)
        {
            ObjStm* stm = loadObjStm(num);
            if (!stm) continue;
            for (const auto& e : stm->index) setEntry((size_t)e.first, 2, (uint32_t)num, 0);
        }
    }

    /////////////////////////////////////////
    // pages / outlines
    void collectPages(const Obj& nodeRef, const Obj* res, int depth, std::set<int>& visited)
    {
        if (depth > kMaxDepth || aborted()) return;
        int num = nodeRef.t == Obj::REF ? nodeRef.num : -1;
        if (num > 0) { if (visited.count(num)) return; visited.insert(num); }
        const Obj& node = resolve(nodeRef);
        if (!node.isDict()) return;
        if (const Obj* r = node.find("Resources")) res = r;
        const Obj& kids = at(node, "Kids");
        const Obj& type = at(node, "Type");
        if (kids.t == Obj::ARR && !type.isName("Page"))
        {
            for (const auto& k : kids.items) collectPages(k, res, depth + 1, visited);
            return;
        }
        if (type.isName("Pages")) return;
        Page pg = { num, &node, res };
        if (num > 0) pageByNum[num] = (int)pages.size();
        pages.push_back(pg);
    }

    void loadNamedDests(const Obj& node, int depth)
    {
        if (depth > kMaxDepth || namedDests.size() > 100000) return;
        const Obj& names = at(node, "Names");
        if (names.t == Obj::ARR)
            for (size_t i = 0; i + 1 < names.items.size(); i += 2)
                namedDests[resolve(names.items[i]).s] = &names.items[i + 1];
        const Obj& kids = at(node, "Kids");
        if (kids.t == Obj::ARR)
            for (const auto& k : kids.items) loadNamedDests(resolve(k), depth + 1);
    }

    int destToPage(const Obj& destIn, int depth = 0)
    {
        if (depth > 4) return -1;
        const Obj& dest = resolve(destIn);
        if (dest.t == Obj::ARR && !dest.items.empty())
        {
            const Obj& p = dest.items[0];
            if (p.t == Obj::REF) { auto it = pageByNum.find(p.num); return it != pageByNum.end() ? it->second : -1; }
            if (p.t == Obj::NUM) { int i = p.i(); return i >= 0 && i < (int)pages.size() ? i : -1; }
            return -1;
        }
        if (dest.isDict()) return destToPage(at(dest, "D"), depth + 1);
        if (dest.t == Obj::NAME || dest.t == Obj::STR)
        {
            const Obj& root = at(trailer, "Root");
            if (dest.t == Obj::NAME)
            {
                const Obj& dests = at(root, "Dests");
                if (const Obj* d = dests.find(dest.s.c_str())) return destToPage(*d, depth + 1);
            }
            if (!namedDestsLoaded)
            {
                namedDestsLoaded = true;
                loadNamedDests(at(at(root, "Names"), "Dests"), 0);
            }
            auto it = namedDests.find(dest.s);
            if (it != namedDests.end()) return destToPage(*it->second, depth + 1);
        }
        return -1;
    }

    void readOutlines(const Obj& firstRef, int level, std::set<int>& visited)
    {
        if (level > 16) return;
        const Obj* cur = &firstRef;
        while (cur && cur->t == Obj::REF && outlines.size() < 20000 && !aborted())
        {
            if (visited.count(cur->num)) break;
            visited.insert(cur->num);
            const Obj& item = resolve(*cur);
            if (!item.isDict()) break;
            Outline o;
            o.title = textString(at(item, "Title").s);
            o.level = level;
            o.page = -1;
            if (const Obj* dest = item.find("Dest")) o.page = destToPage(*dest);
            else
            {
                const Obj& a = at(item, "A");
                if (at(a, "S").isName("GoTo")) o.page = destToPage(at(a, "D"));
            }
            if (!o.title.empty()) outlines.push_back(o);
            if (const Obj* first = item.find("First")) readOutlines(*first, level + 1, visited);
            cur = item.find("Next");
        }
    }

    /////////////////////////////////////////
    // fonts
    void parseCMap(const std::string& cmap, Font& f, bool toUnicode)
    {
        Lexer lx(cmap.data(), cmap.size());
        lx.refs = false;
        Obj o;
        std::vector<Obj> ops;
        auto code = [](const std::string& s) { uint32_t v = 0; for (unsigned char c : s) v = (v << 8) | c; return v; };
        auto dst = [](const Obj& d) {
            if (d.t == Obj::NAME) return glyphNameToUnicode(d.s);
            return utf16beToU32(d.s);
        };
        while (lx.parse(o))
        {
            if (o.t != Obj::KW) { if (ops.size() < 30000) ops.push_back(std::move(o)); continue; }
            if (o.s == "endcodespacerange")
            {
                for (size_t i = 0; i + 1 < ops.size(); i += 2)
                    if (ops[i].t == Obj::STR && !ops[i].s.empty() && ops[i].s.size() <= 4)
                        f.spaces.push_back(Font::Space{ (int)ops[i].s.size(), code(ops[i].s), code(ops[i + 1].s) });
            }
            else if (toUnicode && o.s == "endbfchar")
            {
                for (size_t i = 0; i + 1 < ops.size(); i += 2)
                    if (ops[i].t == Obj::STR) f.uni[code(ops[i].s)] = dst(ops[i + 1]);
            }
            else if (toUnicode && o.s == "endbfrange")
            {
                for (size_t i = 0; i + 2 < ops.size(); i += 3)
                {
                    uint32_t lo = code(ops[i].s), hi = code(ops[i + 1].s);
                    if (hi < lo) continue;
                    const Obj& d = ops[i + 2];
                    if (d.t == Obj::ARR)
                    {
                        for (size_t k = 0; k < d.items.size() && lo + k <= hi; k++) f.uni[lo + (uint32_t)k] = dst(d.items[k]);
                    }
                    else
                    {
                        Font::Range r = { lo, hi, dst(d) };
                        if (!r.base.empty()) f.ranges.push_back(r);
                    }
                }
            }
            ops.clear();
            if (aborted()) return;
        }
        std::sort(f.ranges.begin(), f.ranges.end(), [](const Font::Range& a, const Font::Range& b) { return a.lo < b.lo; });
    }

    void simpleEncoding(Font& f, const Obj& fontDict, const std::string& subtype)
    {
        const uint16_t* base = subtype == "TrueType" ? kWinAnsiEncoding : kStandardEncoding;
        const Obj& enc = at(fontDict, "Encoding");
        const Obj* encName = enc.t == Obj::NAME ? &enc : (enc.isDict() ? &at(enc, "BaseEncoding") : nullptr);
        if (encName && encName->t == Obj::NAME)
        {
            if (encName->s == "WinAnsiEncoding") base = kWinAnsiEncoding;
            else if (encName->s == "MacRomanEncoding") base = kMacRomanEncoding;
            else if (encName->s == "StandardEncoding") base = kStandardEncoding;
            else if (encName->s == "PDFDocEncoding") base = kPdfDocEncoding;
        }
        for (int i = 0; i < 256; i++) f.simple[i] = base[i];
        f.hasSimple = true;
        if (enc.isDict())
        {
            const Obj& diff = at(enc, "Differences");
            int c = 0;
            for (const auto& item : diff.items)
            {
                const Obj& v = resolve(item);
                if (v.t == Obj::NUM) c = v.i();
                else if (v.t == Obj::NAME)
                {
                    if (c >= 0 && c < 256)
                    {
                        std::u32string u = glyphNameToUnicode(v.s);
                        f.simple[c] = u.size() == 1 ? u[0] : 0;
                        if (u.size() > 1) f.uni[(uint32_t)c] = u;
                    }
                    c++;
                }
            }
        }
    }

    Font* loadFont(const Obj& fontRef)
    {
        const Obj& fd = resolve(fontRef);
        if (!fd.isDict()) return nullptr;
        auto it = fonts.find(&fd);
        if (it != fonts.end()) return it->second.get();
        std::shared_ptr<Font> fp(new Font());
        Font& f = *fp;
        fonts[&fd] = fp;

        std::string subtype = at(fd, "Subtype").s;
        const Obj& enc = at(fd, "Encoding");
        if (subtype == "Type0")
        {
            f.cid = true;
            f.fixedBytes = 2;
            const Obj& desc = resolve(at(fd, "DescendantFonts").items.empty() ? kNull : at(fd, "DescendantFonts").items[0]);
            const Obj& dw = at(desc, "DW");
            if (dw.t == Obj::NUM) f.dw = (float)dw.n;
            const Obj& w = at(desc, "W");
            for (size_t i = 0; i + 1 < w.items.size() && f.cidW.size() < 200000;)
            {
                uint32_t c0 = (uint32_t)resolve(w.items[i]).i();
                const Obj& nx = resolve(w.items[i + 1]);
                if (nx.t == Obj::ARR)
                {
                    for (size_t k = 0; k < nx.items.size(); k++) f.cidW[c0 + (uint32_t)k] = (float)resolve(nx.items[k]).n;
                    i += 2;
                }
                else if (i + 2 < w.items.size())
                {
                    uint32_t c1 = (uint32_t)nx.i();
                    float wv = (float)resolve(w.items[i + 2]).n;
                    for (uint32_t c = c0; c <= c1 && c - c0 < 65536; c++) f.cidW[c] = wv;
                    i += 3;
                }
                else break;
            }
            if (enc.t == Obj::NAME)
            {
                const std::string& n = enc.s;
                if (n.compare(0, 8, "Identity") == 0) f.identity = true;
                else if (n.find("UCS2") != std::string::npos || n.find("UTF16") != std::string::npos) f.utf16 = true;
                else if (n.find("UTF8") != std::string::npos) f.utf8 = true;
                else if (n.find("GB") != std::string::npos) f.codepage = 936;
                else if (n.find("B5") != std::string::npos || n.find("CNS") != std::string::npos) f.codepage = 950;
                else if (n.find("RKSJ") != std::string::npos || (n.find("EUC") != std::string::npos && n.find("KSC") == std::string::npos)) f.codepage = 932;
                else if (n.find("KSC") != std::string::npos || n.find("UHC") != std::string::npos) f.codepage = 949;
            }
            else if (enc.t == Obj::STREAM)
            {
                std::string cm;
                if (decodeStream(enc, cm)) parseCMap(cm, f, false);
            }
        }
        else
        {
            simpleEncoding(f, fd, subtype);
            const std::string& baseFont = at(fd, "BaseFont").s;
            if (baseFont.find("Dingbats") != std::string::npos && !enc.isDict()) f.hasSimple = false; // pictograms, no text
            f.firstChar = at(fd, "FirstChar").i();
            const Obj& w = at(fd, "Widths");
            for (const auto& i : w.items) f.widths.push_back((float)resolve(i).n);
            const Obj& mw = at(at(fd, "FontDescriptor"), "MissingWidth");
            if (mw.t == Obj::NUM && mw.n > 0) f.missing = (float)mw.n;
            else if (baseFont.find("Courier") != std::string::npos) f.missing = 600;
            if (f.widths.empty())
            {
                // standard 14 fonts may omit /Widths: use the built-in AFM metrics (by Unicode -> WinAnsi code)
                const uint16_t* table = nullptr;
                bool bold = baseFont.find("Bold") != std::string::npos || baseFont.find("Black") != std::string::npos;
                if (baseFont.find("Courier") != std::string::npos) { f.missing = 600; }
                else if (baseFont.find("Times") != std::string::npos || baseFont.find("Serif") != std::string::npos)
                    table = bold ? kWidthTimesBold : kWidthTimes;
                else if (baseFont.find("Helvetica") != std::string::npos || baseFont.find("Arial") != std::string::npos)
                    table = bold ? kWidthHelveticaBold : kWidthHelvetica;
                if (table)
                {
                    f.firstChar = 0;
                    f.widths.assign(256, 0);
                    for (int c = 0; c < 256; c++)
                    {
                        char32_t u = f.hasSimple ? f.simple[c] : (char32_t)kStandardEncoding[c];
                        for (int k = 32; k < 256 && u; k++)
                            if (kWinAnsiEncoding[k] == u) { f.widths[c] = table[k]; break; }
                    }
                }
            }
            if (subtype == "Type3")
            {
                const Obj& fm = at(fd, "FontMatrix");
                if (fm.t == Obj::ARR && !fm.items.empty()) f.scale = (float)(resolve(fm.items[0]).n * 1000.0);
                if (f.scale <= 0) f.scale = 1;
            }
        }

        const Obj& tu = at(fd, "ToUnicode");
        if (tu.t == Obj::STREAM)
        {
            std::string cm;
            if (decodeStream(tu, cm))
            {
                std::vector<Font::Space> keep = f.spaces;
                parseCMap(cm, f, true);
                if (f.identity || !keep.empty()) f.spaces = keep; // the font's own code length wins
                f.utf16 = f.utf8 = false;
                f.codepage = 0;
            }
        }
        if (f.identity) f.spaces.clear();
        return fp.get();
    }

    /////////////////////////////////////////
    // content interpreter
    void emit(const std::u32string& text, double x0, double y0, double x1, double y1)
    {
        if (text.empty()) return;
        Mat trm = Mat(fontSize * hscale, 0, 0, fontSize, 0, rise) * tm * ctm;
        double size = std::sqrt(std::fabs(trm.a * trm.d - trm.b * trm.c));
        if (size < 0.5) size = 0.5;
        // rotated text: measure along the baseline direction
        double ang = std::atan2(trm.b, trm.a);
        double cs = std::cos(-ang), sn = std::sin(-ang);
        Run r;
        r.x0 = x0 * cs - y0 * sn;
        r.y = x0 * sn + y0 * cs;
        r.x1 = x1 * cs - y1 * sn;
        if (r.x1 < r.x0) r.x1 = r.x0;
        r.size = size;
        r.t = text;
        runs.push_back(std::move(r));
    }

    void showText(const std::string& bytes)
    {
        if (!font || bytes.empty()) return;
        Mat m = tm * ctm;
        double x0, y0;
        m.apply(0, rise, x0, y0);
        std::u32string out;

        if (font->utf16 || font->utf8 || font->codepage)
        {
            out = font->utf16 ? utf16beToU32(bytes) : font->utf8 ? utf8ToU32(bytes) : decodeCodepage(font->codepage, bytes);
            double adv = 0;
            for (char32_t c : out)
            {
                adv += ((c < 0x2E80 ? 0.5 : 1.0) * fontSize + charSpace + (c == ' ' ? wordSpace : 0)) * hscale;
            }
            tm = Mat(1, 0, 0, 1, adv, 0) * tm;
        }
        else
        {
            std::vector<std::pair<uint32_t, int>> codes;
            font->codes(bytes, codes);
            std::u32string u;
            for (const auto& c : codes)
            {
                if (font->lookup(c.first, u)) out += u;
                double w = font->width(c.first) / 1000.0;
                double tx = (w * fontSize + charSpace + (c.second == 1 && c.first == 32 ? wordSpace : 0)) * hscale;
                tm = Mat(1, 0, 0, 1, tx, 0) * tm;
            }
        }
        m = tm * ctm;
        double x1, y1;
        m.apply(0, rise, x1, y1);
        emit(out, x0, y0, x1, y1);
    }

    void runContent(const std::string& cs, const Obj& res, int depth)
    {
        Lexer lx(cs.data(), cs.size());
        lx.refs = false;
        std::vector<Obj> ops;
        Obj o;
        int counter = 0;
        auto num = [&](size_t i) { return i < ops.size() && ops[i].t == Obj::NUM ? ops[i].n : 0.0; };
        while (lx.parse(o))
        {
            if (o.t != Obj::KW)
            {
                if (ops.size() < 64) ops.push_back(std::move(o));
                continue;
            }
            if ((++counter & 1023) == 0 && aborted()) return;
            const std::string& op = o.s;
            size_t n = ops.size();
            switch (op[0])
            {
            case 'B':
                if (op == "BT") { tm = tlm = Mat(); }
                else if (op == "BI")
                {
                    while (lx.parse(o) && !(o.t == Obj::KW && o.s == "ID")) {}
                    if (lx.p < lx.e) lx.p++;
                    while (lx.p + 2 <= lx.e)
                    {
                        if (lx.p[0] == 'E' && lx.p[1] == 'I' && isWs(lx.p[-1]) && (lx.p + 2 == lx.e || isWs(lx.p[2]) || isDelim(lx.p[2]))) { lx.p += 2; break; }
                        lx.p++;
                    }
                }
                break;
            case 'T':
                if (op == "Tf" && n >= 2)
                {
                    fontSize = num(n - 1);
                    font = nullptr;
                    if (ops[n - 2].t == Obj::NAME)
                    {
                        const Obj& fdict = at(at(res, "Font"), ops[n - 2].s.c_str());
                        font = loadFont(fdict);
                    }
                }
                else if (op == "Td" && n >= 2) { tlm = Mat(1, 0, 0, 1, num(n - 2), num(n - 1)) * tlm; tm = tlm; }
                else if (op == "TD" && n >= 2) { leading = -num(n - 1); tlm = Mat(1, 0, 0, 1, num(n - 2), num(n - 1)) * tlm; tm = tlm; }
                else if (op == "Tm" && n >= 6) { tlm = tm = Mat(num(n - 6), num(n - 5), num(n - 4), num(n - 3), num(n - 2), num(n - 1)); }
                else if (op == "T*") { tlm = Mat(1, 0, 0, 1, 0, -leading) * tlm; tm = tlm; }
                else if (op == "TL" && n >= 1) leading = num(n - 1);
                else if (op == "Tc" && n >= 1) charSpace = num(n - 1);
                else if (op == "Tw" && n >= 1) wordSpace = num(n - 1);
                else if (op == "Tz" && n >= 1) hscale = num(n - 1) / 100.0;
                else if (op == "Ts" && n >= 1) rise = num(n - 1);
                else if (op == "Tj" && n >= 1 && ops[n - 1].t == Obj::STR) showText(ops[n - 1].s);
                else if (op == "TJ" && n >= 1 && ops[n - 1].t == Obj::ARR)
                {
                    for (const auto& item : ops[n - 1].items)
                    {
                        if (item.t == Obj::STR) showText(item.s);
                        else if (item.t == Obj::NUM) tm = Mat(1, 0, 0, 1, -item.n / 1000.0 * fontSize * hscale, 0) * tm;
                    }
                }
                break;
            case '\'':
            case '"':
                if (op == "\"" && n >= 3) { wordSpace = num(n - 3); charSpace = num(n - 2); }
                tlm = Mat(1, 0, 0, 1, 0, -leading) * tlm;
                tm = tlm;
                if (n >= 1 && ops[n - 1].t == Obj::STR) showText(ops[n - 1].s);
                break;
            case 'c':
                if (op == "cm" && n >= 6) ctm = Mat(num(n - 6), num(n - 5), num(n - 4), num(n - 3), num(n - 2), num(n - 1)) * ctm;
                break;
            case 'q':
                if (op == "q" && gstack.size() < 256) gstack.push_back(GState{ ctm });
                break;
            case 'Q':
                if (op == "Q" && !gstack.empty()) { ctm = gstack.back().ctm; gstack.pop_back(); }
                break;
            case 'D':
                if (op == "Do" && n >= 1 && ops[n - 1].t == Obj::NAME && depth < kMaxXObject)
                {
                    const Obj* ref = at(res, "XObject").find(ops[n - 1].s.c_str());
                    const Obj& xo = resolve(ref);
                    if (xo.t == Obj::STREAM && at(xo, "Subtype").isName("Form") && !formStack.count(xo.num))
                    {
                        std::string sub;
                        if (decodeStream(xo, sub))
                        {
                            formStack.insert(xo.num);
                            Mat saved = ctm;
                            Mat savedTm = tm, savedTlm = tlm;
                            const Obj& mtx = at(xo, "Matrix");
                            if (mtx.t == Obj::ARR && mtx.items.size() == 6)
                            {
                                double v[6];
                                for (int k = 0; k < 6; k++) v[k] = resolve(mtx.items[k]).n;
                                ctm = Mat(v[0], v[1], v[2], v[3], v[4], v[5]) * ctm;
                            }
                            const Obj& subRes = at(xo, "Resources");
                            size_t level = gstack.size();
                            runContent(sub, subRes.isDict() ? subRes : res, depth + 1);
                            gstack.resize(std::min(level, gstack.size()));
                            ctm = saved;
                            tm = savedTm;
                            tlm = savedTlm;
                            formStack.erase(xo.num);
                        }
                    }
                }
                break;
            default:
                break;
            }
            ops.clear();
        }
    }

    /////////////////////////////////////////
    // layout
    static bool isPageNumber(const std::u32string& t)
    {
        std::string a;
        for (char32_t c : t)
        {
            if (isSpace(c) || c == '-' || c == 0x2013 || c == 0x2014 || c == '[' || c == ']' || c == '(' || c == ')' || c == '/' || c == '|') { a += ' '; continue; }
            if (c == 0x7B2C || c == 0x9875 || c == 0x9801) { a += 'p'; continue; } // 第 页 頁
            if (c < 128) a += (char)tolower((int)c);
            else return false;
        }
        if (a.size() > 24) return false;
        bool digit = false;
        for (size_t i = 0; i < a.size(); i++)
        {
            char c = a[i];
            if (c >= '0' && c <= '9') { digit = true; continue; }
            if (c == ' ' || c == 'p') continue;
            if (a.compare(i, 4, "page") == 0) { i += 3; continue; }
            if (a.compare(i, 2, "of") == 0) { i += 1; continue; }
            if (strchr("ivxlcdm", c)) { digit = true; continue; }
            return false;
        }
        return digit;
    }

    void buildParagraphs(std::vector<Paragraph>& out)
    {
        out.clear();
        std::vector<Line> lines;
        for (auto& r : runs)
        {
            bool blank = true;
            for (char32_t c : r.t) if (!isSpace(c)) { blank = false; break; }
            Line* cur = lines.empty() ? nullptr : &lines.back();
            double sz = std::max(r.size, cur ? cur->size : r.size);
            bool same = cur && std::fabs(r.y - cur->y) < 0.6 * sz && r.x0 > cur->x0 - 0.5 * sz && r.x0 > cur->x1 - 2.0 * sz;
            if (cur && !same && std::fabs(r.y - cur->y) < 0.2 * sz && r.x0 < cur->x1)
            {
                // overprinted text (fake bold / shadow): drop duplicates
                if (cur->t.size() >= r.t.size() && cur->t.compare(cur->t.size() - r.t.size(), r.t.size(), r.t) == 0) continue;
            }
            if (blank)
            {
                if (cur && same) { cur->x1 = std::max(cur->x1, r.x1); if (!cur->t.empty() && !isSpace(cur->t.back())) cur->t += ' '; }
                continue;
            }
            if (!same)
            {
                Line l;
                l.x0 = r.x0; l.x1 = r.x1; l.y = r.y; l.size = r.size;
                l.lead = isSpace(r.t[0]);
                l.t = r.t;
                lines.push_back(std::move(l));
                continue;
            }
            double gap = r.x0 - cur->x1;
            char32_t last = cur->t.empty() ? 0 : cur->t.back();
            char32_t first = r.t[0];
            bool cjk = isCjk(last) || isCjk(first);
            if (!isSpace(last) && !isSpace(first) && gap > (cjk ? 1.5 : 0.15) * sz) cur->t += ' ';
            cur->t += r.t;
            cur->x1 = std::max(cur->x1, r.x1);
            cur->size = std::max(cur->size, r.size);
        }
        runs.clear();

        // normalise text, drop empty lines
        for (auto& l : lines)
        {
            std::u32string t;
            for (char32_t c : l.t)
            {
                if (c >= 0xFB00 && c <= 0xFB06)
                {
                    static const char* lig[] = { "ff", "fi", "fl", "ffi", "ffl", "st", "st" };
                    for (const char* q = lig[c - 0xFB00]; *q; q++) t += (char32_t)*q;
                    continue;
                }
                if (c == 0xAD || c == 0xFFFD || c == 0xFEFF || (c >= 0xE000 && c <= 0xF8FF)) continue;
                if (c < 0x20 || c == 0xA0) c = ' ';
                t += c;
            }
            size_t s = 0, e = t.size();
            while (s < e && isSpace(t[s])) s++;
            while (e > s && isSpace(t[e - 1])) e--;
            l.t = t.substr(s, e - s);
        }
        lines.erase(std::remove_if(lines.begin(), lines.end(), [](const Line& l) { return l.t.empty(); }), lines.end());
        if (lines.empty()) return;

        // page numbers at the very top / bottom (never larger than body text, so headings survive)
        if (lines.size() > 1)
        {
            std::vector<double> sz;
            for (const auto& l : lines) sz.push_back(l.size);
            std::nth_element(sz.begin(), sz.begin() + sz.size() / 2, sz.end());
            double body = sz[sz.size() / 2] * 1.05;
            size_t top = 0, bottom = 0;
            for (size_t i = 1; i < lines.size(); i++)
            {
                if (lines[i].y > lines[top].y) top = i;
                if (lines[i].y < lines[bottom].y) bottom = i;
            }
            bool dropBottom = lines[bottom].size <= body && isPageNumber(lines[bottom].t);
            bool dropTop = top != bottom && lines[top].size <= body && isPageNumber(lines[top].t);
            std::vector<Line> kept;
            for (size_t i = 0; i < lines.size(); i++)
                if (!((i == bottom && dropBottom) || (i == top && dropTop))) kept.push_back(std::move(lines[i]));
            lines.swap(kept);
            if (lines.empty()) return;
        }

        // page metrics
        std::vector<double> sizes, gaps;
        for (const auto& l : lines) sizes.push_back(l.size);
        std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
        const double bodySize = sizes[sizes.size() / 2];
        double left = 1e30, right = -1e30;
        int rightSamples = 0;
        // margins ignore the top-most / bottom-most line: running headers and footers sit outside the text block
        size_t topLine = 0, bottomLine = 0;
        for (size_t i = 1; i < lines.size(); i++)
        {
            if (lines[i].y > lines[topLine].y) topLine = i;
            if (lines[i].y < lines[bottomLine].y) bottomLine = i;
        }
        for (size_t i = 0; i < lines.size(); i++)
        {
            bool edge = (lines.size() > 4 && (i == topLine || i == bottomLine)) ||
                        lines[i].size < bodySize * 0.85 || lines[i].size > bodySize * 1.15;
            if (lines[i].t.size() >= 3 && !edge) left = std::min(left, lines[i].x0);
            if (!edge) { right = std::max(right, lines[i].x1); rightSamples++; }
            if (i)
            {
                double g = lines[i - 1].y - lines[i].y;
                if (g > 0 && g < 3 * lines[i].size) gaps.push_back(g);
            }
        }
        if (left > 1e29) left = lines[0].x0;
        if (right < -1e29) right = lines[0].x1;
        auto median = [](std::vector<double>& v) { if (v.empty()) return 0.0; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
        double typGap = median(gaps);

        auto indented = [&](const Line& l) { return l.lead || l.x0 - left > 0.9 * l.size; };
        // with fewer than 3 reference lines the right margin is unknown: assume the paragraph ended
        auto shortLine = [&](const Line& l) { return rightSamples < 3 || l.x1 < right - 3.0 * l.size; };

        Paragraph para;
        std::u32string text;
        const Line* prev = nullptr;
        auto flush = [&] {
            if (text.empty()) return;
            para.text.clear();
            for (char32_t c : text) appendW(para.text, c);
            out.push_back(para);
            text.clear();
        };
        for (const auto& l : lines)
        {
            bool newPara = !prev;
            if (prev)
            {
                double g = prev->y - l.y;
                double sz = std::max(prev->size, l.size);
                if (g < -0.5 * sz) newPara = true;                                              // jumped up: new column / block
                else if (typGap > 0 ? g > typGap * 1.5 + 0.1 * sz : g > 2.2 * sz) newPara = true; // vertical space
                else if (std::fabs(l.size - prev->size) > 0.15 * sz) newPara = true;            // heading
                else if (indented(l) && !indented(*prev)) newPara = true;                       // first-line indent
                else if (shortLine(*prev)) newPara = true;                                      // previous line ended early
            }
            if (newPara)
            {
                flush();
                para.indented = indented(l);
                para.size = (float)l.size;
                text = l.t;
            }
            else
            {
                char32_t last = text.back(), first = l.t[0];
                if (last == '-' && text.size() > 1 && ((text[text.size() - 2] >= 'a' && text[text.size() - 2] <= 'z') || (text[text.size() - 2] >= 'A' && text[text.size() - 2] <= 'Z')) && first >= 'a' && first <= 'z')
                    text.pop_back();
                else if (!isCjk(last) && !isCjk(first)) text += ' ';
                text += l.t;
            }
            para.endsShort = shortLine(l);
            para.lastSize = (float)l.size;
            prev = &l;
        }
        flush();
    }

    bool pageText(int index, std::vector<Paragraph>& out)
    {
        out.clear();
        if (index < 0 || index >= (int)pages.size()) return false;
        const Page& pg = pages[index];
        const Obj& res = resolve(pg.res);
        const Obj& contents = at(*pg.dict, "Contents");
        std::string cs, part;
        if (contents.t == Obj::STREAM) decodeStream(contents, cs);
        else if (contents.t == Obj::ARR)
        {
            for (const auto& c : contents.items)
            {
                if (decodeStream(resolve(c), part)) { cs += part; cs += '\n'; }
                if (aborted()) return false;
            }
        }
        ctm = tm = tlm = Mat();
        gstack.clear();
        font = nullptr;
        fontSize = charSpace = wordSpace = leading = rise = 0;
        hscale = 1;
        runs.clear();
        formStack.clear();
        runContent(cs, res, 0);
        if (aborted()) return false;
        buildParagraphs(out);
        // fonts are cheap to rebuild; keep memory bounded on huge documents
        if (fonts.size() > 512) fonts.clear();
        if (objstms.size() > 64) objstms.clear();
        return true;
    }
};

/////////////////////////////////////////////
Document::Document() : d(new Impl()) {}
Document::~Document() { delete d; }

Status Document::open(const char* data, size_t size, const volatile bool* abort)
{
    d->data = (const uint8_t*)data;
    d->size = size;
    d->abortFlag = abort;

    bool header = false;
    for (size_t i = 0; i + 5 <= std::min(size, (size_t)1024); i++)
        if (memcmp(data + i, "%PDF-", 5) == 0) { header = true; break; }
    if (!header) return Status::NOT_PDF;

    if (!d->readXref())
    {
        d->reconstruct();
        if (!d->trailer.find("Root")) return Status::BROKEN;
    }
    if (d->aborted()) return Status::ABORTED;
    if (!d->setupEncryption()) return Status::ENCRYPTED;

    std::set<int> visited;
    const Obj& root = d->at(d->trailer, "Root");
    const Obj* pagesRef = root.find("Pages");
    if (pagesRef) d->collectPages(*pagesRef, nullptr, 0, visited);
    if (d->pages.empty() && !d->reconstructed)
    {
        d->reconstruct();
        d->indexPendingObjStms();
        const Obj& root2 = d->at(d->trailer, "Root");
        visited.clear();
        if ((pagesRef = root2.find("Pages"))) d->collectPages(*pagesRef, nullptr, 0, visited);
    }
    if (d->pages.empty())
    {
        // last resort: every /Type /Page object in object order
        for (size_t n = 1; n < d->xref.size() && !d->aborted(); n++)
        {
            if (d->xref[n].type == 0) continue;
            const Obj& o = d->get((int)n);
            if (o.isDict() && d->at(o, "Type").isName("Page"))
            {
                Impl::Page pg = { (int)n, &o, o.find("Resources") };
                d->pageByNum[(int)n] = (int)d->pages.size();
                d->pages.push_back(pg);
            }
        }
    }
    if (d->aborted()) return Status::ABORTED;
    if (d->pages.empty()) return Status::BROKEN;

    const Obj& outlines = d->at(d->at(d->trailer, "Root"), "Outlines");
    if (const Obj* first = outlines.find("First"))
    {
        visited.clear();
        d->readOutlines(*first, 0, visited);
    }
    d->title = textString(d->at(d->at(d->trailer, "Info"), "Title").s);
    return d->aborted() ? Status::ABORTED : Status::OK;
}

int Document::pageCount() const { return (int)d->pages.size(); }
bool Document::pageText(int page, std::vector<Paragraph>& out) { return d->pageText(page, out); }
const std::vector<Outline>& Document::outlines() const { return d->outlines; }
const std::wstring& Document::title() const { return d->title; }

bool Document::coverJpeg(std::string& out)
{
    out.clear();
    if (d->pages.empty()) return false;
    const Obj& xobjs = d->at(d->resolve(d->pages[0].res), "XObject");
    double best = 0;
    for (const auto& item : xobjs.items)
    {
        const Obj& img = d->resolve(item);
        if (img.t != Obj::STREAM || !d->at(img, "Subtype").isName("Image")) continue;
        const Obj& f = d->at(img, "Filter");
        bool dct = f.isName("DCTDecode") || (f.t == Obj::ARR && f.items.size() == 1 && d->resolve(f.items[0]).isName("DCTDecode"));
        double area = d->at(img, "Width").n * d->at(img, "Height").n;
        if (!dct || area < 200 * 200 || area <= best) continue;
        best = area;
        out.assign((const char*)d->data + img.soff, std::min(img.slen, d->size - img.soff));
        if (d->encrypted && d->stmMethod) d->decryptBytes(out, img.num, img.gen, d->stmMethod);
    }
    return !out.empty();
}

} // namespace pdf
