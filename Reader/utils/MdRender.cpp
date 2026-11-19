#include "MdRender.h"
#include <algorithm>

namespace md {
namespace {

inline bool isBlank(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == 0x3000 || c == 0xA0 || c == 0xFEFF; }

inline bool isCjk(wchar_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0xD800 && c <= 0xDBFF) || (sizeof(wchar_t) > 2 && (unsigned)c >= 0x20000);
}

std::wstring trim(const wchar_t* s, size_t n)
{
    size_t b = 0, e = n;
    while (b < e && isBlank(s[b])) b++;
    while (e > b && isBlank(s[e - 1])) e--;
    return std::wstring(s + b, e - b);
}

// whitespace-free, lower-case form used to compare titles with text lines
std::wstring norm(const std::wstring& s)
{
    std::wstring r;
    for (wchar_t c : s)
    {
        if (isBlank(c) || c == L'\n') continue;
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        r += c;
    }
    return r;
}

std::wstring headingText(const std::wstring& t)
{
    std::wstring r;
    bool space = false;
    for (wchar_t c : t)
    {
        if (isBlank(c) || c == L'\n') { space = !r.empty(); continue; }
        if (space) { r += L' '; space = false; }
        r += c;
    }
    while (!r.empty() && (r.back() == L'#' || r.back() == L' ')) r.pop_back(); // closing hashes would be eaten
    return r;
}

std::wstring escapeLine(const std::wstring& p)
{
    if (p.empty()) return p;
    wchar_t c = p[0];
    if (c == L'#' || c == L'>' || c == L'<' || c == L'|' || c == L'=' ||
        ((c == L'-' || c == L'*' || c == L'+') && p.size() > 1 && p[1] == L' ') ||
        p.compare(0, 3, L"```") == 0 || p.compare(0, 3, L"~~~") == 0)
    {
        // keep scene breaks ("* * *", "---") as thematic breaks
        std::wstring n = norm(p);
        if (n.size() >= 3 && n.find_first_not_of(n[0]) == std::wstring::npos && (n[0] == L'*' || n[0] == L'-')) return L"* * *";
        return L"\\" + p;
    }
    size_t i = 0;
    while (i < p.size() && p[i] >= L'0' && p[i] <= L'9') i++;
    if (i > 0 && i < 10 && i + 1 < p.size() && (p[i] == L'.' || p[i] == L')') && p[i + 1] == L' ')
        return p.substr(0, i) + L"\\" + p.substr(i);
    return p;
}

size_t countWords(const std::wstring& s)
{
    size_t n = 0;
    bool inWord = false;
    for (wchar_t c : s)
    {
        if (isCjk(c)) { n++; inWord = false; }
        else if (isBlank(c) || c == L'\n') inWord = false;
        else if (!inWord) { n++; inWord = true; }
    }
    return n;
}

struct Unit
{
    std::wstring heading;           // empty for front matter
    int level = 0;
    std::vector<std::wstring> paras;
    size_t words = 0;
};

} // namespace

std::string toUtf8(const std::wstring& w)
{
    std::string out;
    out.reserve(w.size() * 3);
    for (size_t i = 0; i < w.size(); i++)
    {
        unsigned long c = (unsigned long)w[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size())
        {
            unsigned long l = (unsigned long)w[i + 1];
            if (l >= 0xDC00 && l <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (l - 0xDC00); i++; }
        }
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c <= 0x10FFFF)
        {
            out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

RenderResult render(const wchar_t* text, size_t length, std::vector<Heading> headings,
                    const std::wstring& title, const std::wstring& sourceName, size_t maxWords)
{
    RenderResult result;
    if (!text) length = 0;
    if (maxWords < 1000) maxWords = 1000;

    // chapters in text order; those without content are reported, not rendered
    std::vector<Heading> hs;
    for (auto& h : headings)
    {
        if (h.index < 0 || (size_t)h.index > length) { result.skippedChapters++; continue; }
        hs.push_back(h);
    }
    std::stable_sort(hs.begin(), hs.end(), [](const Heading& a, const Heading& b) { return a.index < b.index; });

    // split the text into units: front matter + one unit per chapter
    std::vector<Unit> units;
    auto addParas = [&](Unit& u, size_t from, size_t to, const std::wstring& skipTitle) {
        bool first = true;
        std::wstring t = norm(skipTitle);
        size_t pos = from;
        while (pos < to)
        {
            size_t end = pos;
            while (end < to && text[end] != L'\n') end++;
            std::wstring p = trim(text + pos, end - pos);
            pos = end + 1;
            if (p.empty()) continue;
            if (first)
            {
                first = false;
                // the chapter's title line is already the heading
                std::wstring n = norm(p);
                if (!t.empty() && n.size() <= t.size() + 20 && (n.find(t) != std::wstring::npos || (n.size() >= 2 && t.compare(0, n.size(), n) == 0)))
                    continue;
            }
            u.words += countWords(p);
            u.paras.push_back(escapeLine(p));
        }
    };

    size_t firstStart = hs.empty() ? length : (size_t)hs[0].index;
    Unit front;
    addParas(front, 0, firstStart, std::wstring());
    if (!front.paras.empty()) units.push_back(std::move(front));
    for (size_t i = 0; i < hs.size(); i++)
    {
        size_t from = (size_t)hs[i].index;
        size_t to = i + 1 < hs.size() ? (size_t)hs[i + 1].index : length;
        Unit u;
        u.heading = headingText(hs[i].title);
        u.level = hs[i].level < 0 ? 0 : hs[i].level;
        u.words = countWords(u.heading);
        addParas(u, from, std::max(from, to), hs[i].title);
        if (u.heading.empty() && u.paras.empty()) continue;
        units.push_back(std::move(u));
    }

    // pack units into parts below the word limit
    std::wstring bookTitle = headingText(title);
    if (bookTitle.empty()) bookTitle = L"Untitled";
    std::vector<std::wstring> parts;
    std::wstring cur;
    size_t curWords = 0;
    auto newPart = [&] {
        if (!cur.empty()) parts.push_back(cur);
        cur.clear();
        curWords = 0;
    };
    auto heading = [](int level, const std::wstring& t) {
        return std::wstring((size_t)std::min(6, 2 + level), L'#') + L" " + t + L"\n\n";
    };
    for (const auto& u : units)
    {
        if (curWords > 0 && curWords + u.words > maxWords) newPart();
        if (!u.heading.empty()) cur += heading(u.level, u.heading);
        for (const auto& p : u.paras)
        {
            size_t w = countWords(p);
            if (curWords > 0 && curWords + w > maxWords)
            {
                newPart();
                if (!u.heading.empty()) cur += heading(u.level, u.heading); // continue the chapter in the next part
            }
            cur += p;
            cur += L"\n\n";
            curWords += w;
        }
        curWords += countWords(u.heading);
    }
    newPart();
    if (parts.empty()) parts.push_back(std::wstring());

    for (size_t i = 0; i < parts.size(); i++)
    {
        std::wstring head = L"# " + bookTitle;
        if (parts.size() > 1) head += L" (" + std::to_wstring(i + 1) + L"/" + std::to_wstring(parts.size()) + L")";
        head += L"\n\n";
        if (!sourceName.empty()) head += L"> " + headingText(sourceName) + L"\n\n";
        std::wstring doc = head + parts[i];
        while (doc.size() >= 2 && doc[doc.size() - 1] == L'\n' && doc[doc.size() - 2] == L'\n') doc.pop_back();
        result.parts.push_back(toUtf8(doc));
    }
    return result;
}

} // namespace md
