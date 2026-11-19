#include "PdfText.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>

namespace pdf {
namespace {

bool isCjkChar(wchar_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0xD800 && c <= 0xDFFF) ||
           c == 0x2014 || c == 0x2026 || c == 0x201C || c == 0x201D || c == 0x2018 || c == 0x2019;
}

bool isLatinLetter(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

void joinInto(std::wstring& out, const std::wstring& next)
{
    if (next.empty()) return;
    wchar_t last = out.empty() ? 0 : out.back();
    wchar_t first = next[0];
    if (last == L'-' && out.size() > 1 && isLatinLetter(out[out.size() - 2]) && first >= L'a' && first <= L'z')
        out.pop_back();
    else if (last && !isCjkChar(last) && !isCjkChar(first))
        out += L' ';
    out += next;
}

// header/footer key: digits collapsed so "Page 12" and "Page 13" match
std::wstring repeatKey(const std::wstring& t)
{
    std::wstring k;
    for (wchar_t c : t)
    {
        if (c == L' ' || c == L'\t' || c == 0x3000) continue;
        if (c >= L'0' && c <= L'9') { if (k.empty() || k.back() != L'#') k += L'#'; continue; }
        k += c;
    }
    return k.size() <= 80 ? k : std::wstring();
}

std::wstring normTitle(const wchar_t* s, size_t n)
{
    std::wstring r;
    for (size_t i = 0; i < n; i++)
    {
        wchar_t c = s[i];
        if (c == L' ' || c == L'\t' || c == 0x3000 || c == 0xA0) continue;
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        r += c;
    }
    return r;
}

} // namespace

bool buildBookText(Document& doc, BookTextResult& out, const volatile bool* abort)
{
    out = BookTextResult();
    const int n = doc.pageCount();
    std::vector<std::vector<Paragraph>> pages(n);
    for (int i = 0; i < n; i++)
    {
        if (abort && *abort) return false;
        doc.pageText(i, pages[i]);
    }
    if (n > 0)
        for (const auto& p : pages[0]) out.firstPageChars += (int)p.text.size();

    // running headers / footers
    if (n >= 6)
    {
        std::map<std::wstring, int> freq;
        for (const auto& pg : pages)
        {
            if (pg.empty()) continue;
            std::wstring a = repeatKey(pg.front().text);
            if (!a.empty()) freq[a]++;
            if (pg.size() > 1)
            {
                std::wstring b = repeatKey(pg.back().text);
                if (!b.empty() && b != a) freq[b]++;
            }
        }
        const int threshold = std::max(3, n * 3 / 10);
        for (auto& pg : pages)
        {
            if (!pg.empty()) { auto it = freq.find(repeatKey(pg.front().text)); if (it != freq.end() && it->second >= threshold) pg.erase(pg.begin()); }
            if (!pg.empty()) { auto it = freq.find(repeatKey(pg.back().text));  if (it != freq.end() && it->second >= threshold) pg.pop_back(); }
        }
    }

    // Does the document mark paragraphs with a first-line indent (typical for novels)?
    // Then a page that starts without indent continues the previous paragraph. Otherwise
    // (block style) a paragraph whose last line on the page is full width continues.
    size_t multi = 0, indentedCount = 0;
    for (const auto& pg : pages)
        for (const auto& p : pg)
            if (p.text.size() > 40) { multi++; if (p.indented) indentedCount++; }
    const bool indentStyle = multi > 0 && indentedCount * 2 >= multi;

    // stitch pages
    std::wstring& text = out.text;
    std::vector<size_t> pageStart(n + 1, 0);
    bool open = false;
    float openSize = 0;
    for (int i = 0; i < n; i++)
    {
        pageStart[i] = text.size();
        const auto& pg = pages[i];
        for (size_t j = 0; j < pg.size(); j++)
        {
            const Paragraph& para = pg[j];
            bool sameFont = std::fabs(para.size - openSize) <= 0.15f * std::max(para.size, openSize);
            if (j == 0 && open && sameFont && !(indentStyle && para.indented) && !text.empty())
            {
                text.pop_back(); // '\n'
                size_t ls = text.rfind(L'\n');
                pageStart[i] = ls == std::wstring::npos ? 0 : ls + 1;
                joinInto(text, para.text);
            }
            else text += para.text;
            text += L'\n';
            open = j + 1 == pg.size() && !para.endsShort;
            openSize = para.lastSize;
        }
        if (pg.empty()) open = false;
        std::vector<Paragraph>().swap(pages[i]);
    }
    pageStart[n] = text.size();

    // bookmarks -> chapters
    for (const auto& o : doc.outlines())
    {
        if (o.page < 0 || o.page >= n) continue;
        TextChapter c;
        c.index = (int)pageStart[o.page];
        c.titleLen = 0;
        c.level = o.level;
        c.title = o.title;
        std::wstring t = normTitle(o.title.c_str(), o.title.size());
        size_t from = pageStart[o.page], to = pageStart[std::min(o.page + 2, n)];
        for (size_t pos = from; pos < to && !t.empty();)
        {
            size_t end = text.find(L'\n', pos);
            if (end == std::wstring::npos) end = text.size();
            size_t len = end - pos;
            if (len > 0 && len < 300)
            {
                std::wstring l = normTitle(text.c_str() + pos, len);
                bool hit = (l.compare(0, t.size(), t) == 0 && l.size() <= t.size() + 40) ||
                           (l.size() >= 2 && l.size() * 2 >= t.size() && t.compare(0, l.size(), l) == 0);
                if (hit)
                {
                    c.index = (int)pos;
                    c.titleLen = (int)len;
                    break;
                }
            }
            pos = end + 1;
        }
        out.chapters.push_back(c);
    }
    std::stable_sort(out.chapters.begin(), out.chapters.end(), [](const TextChapter& a, const TextChapter& b) { return a.index < b.index; });
    return true;
}

} // namespace pdf
