/**
 * @file PdfText.h
 * @brief Turns the pages of a pdf::Document into one re-flowable text.
 *
 * - paragraphs split by a page break are joined again
 * - running headers / footers that repeat on many pages are removed
 * - each bookmark is pinned to its heading line when it can be found,
 *   otherwise to the start of its page
 * Platform independent so it can be unit tested anywhere.
 */

#pragma once
#include "PdfParser.h"

namespace pdf {

struct TextChapter
{
    int index;          // offset in text
    int titleLen;       // length of the heading line at index, 0 if not found
    int level;
    std::wstring title;
};

struct BookTextResult
{
    std::wstring text;                  // paragraphs separated by '\n'
    std::vector<TextChapter> chapters;  // from bookmarks, sorted by index
    int firstPageChars = 0;             // amount of text on page 1 (cover detection)
};

// Returns false if aborted.
bool buildBookText(Document& doc, BookTextResult& out, const volatile bool* abort = nullptr);

} // namespace pdf
