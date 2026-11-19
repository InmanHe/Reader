/**
 * @file MdRender.h
 * @brief Renders a book (text + chapter list) as Markdown for NotebookLM.
 *
 * Output rules (chosen for clean ingestion by NotebookLM and other RAG tools):
 *   - UTF-8 without BOM, LF line endings
 *   - "# <book title>" once, then one heading per chapter ("##", deeper
 *     bookmark levels become "###"...), so citations point at chapters
 *   - one paragraph per source line, separated by a blank line; leading
 *     indentation removed; characters that would turn into Markdown syntax at
 *     the start of a line are escaped
 *   - the chapter's own title line is not repeated under its heading
 *   - a book larger than the NotebookLM per-source limit (500,000 words) is
 *     split at chapter boundaries into several documents
 * Platform independent (no Windows headers) so it can be unit tested.
 */

#pragma once
#include <string>
#include <vector>

namespace md {

struct Heading
{
    int index;          // offset of the chapter in the text, < 0 = content not available
    int titleLineLen;   // length of the title line at index (0 if unknown)
    int level;          // 0 = top level
    std::wstring title;
};

struct RenderResult
{
    std::vector<std::string> parts;   // UTF-8 Markdown documents
    int skippedChapters = 0;          // chapters without content (e.g. not yet downloaded)
};

// NotebookLM: 500,000 words per source. CJK characters are counted as one word each.
const size_t kDefaultMaxWords = 450000;

RenderResult render(const wchar_t* text, size_t length, std::vector<Heading> headings,
                    const std::wstring& title, const std::wstring& sourceName,
                    size_t maxWordsPerPart = kDefaultMaxWords);

std::string toUtf8(const std::wstring& w);

} // namespace md
