/**
 * @file PdfParser.h
 * @brief Lightweight PDF text extractor.
 *
 * Design Concept:
 * Reader only needs the *text* of a PDF (it re-flows text with its own layout
 * engine), so instead of linking a full renderer such as MuPDF or PDFium
 * (several MB), this module implements just enough of ISO 32000 to pull the
 * text out:
 *   - classic xref tables, xref streams, object streams, incremental updates
 *     and a recovery scan for damaged files
 *   - Flate / LZW / ASCIIHex / ASCII85 / RunLength filters + PNG/TIFF predictors
 *     (zlib comes from the bundled miniz)
 *   - standard security handler with an empty user password
 *     (RC4 40-128, AES-128, AES-256 R5/R6)
 *   - simple fonts (Standard/WinAnsi/MacRoman/Differences), Type0/CID fonts
 *     (ToUnicode, Identity-H, UCS2/UTF16 and legacy CJK code pages)
 *   - form XObjects, outlines (bookmarks) with named destinations
 *   - line / paragraph reconstruction suitable for re-flowing
 * Scanned (image-only) PDFs contain no text and yield empty pages.
 *
 * The module has no Windows dependency except for legacy CJK code pages,
 * which go through MultiByteToWideChar on Windows and iconv elsewhere.
 */

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pdf {

enum class Status
{
    OK,
    NOT_PDF,
    BROKEN,
    ENCRYPTED,  // needs a password we do not have
    ABORTED,
};

struct Outline
{
    std::wstring title;
    int page;   // 0-based page index, -1 if unresolved
    int level;  // 0 = top level
};

struct Paragraph
{
    std::wstring text;
    bool indented;   // first line is indented relative to the page's left margin
    bool endsShort;  // last line stops well before the right margin
    float size;      // font size of the first line
    float lastSize;  // font size of the last line
};

class Document
{
public:
    Document();
    ~Document();

    // data must stay valid for the lifetime of the Document (no copy is made)
    Status open(const char* data, size_t size, const volatile bool* abort = nullptr);

    int  pageCount() const;
    bool pageText(int page, std::vector<Paragraph>& out);
    const std::vector<Outline>& outlines() const;
    const std::wstring& title() const;

    // Raw JPEG bytes of the largest image on the first page, if any (used as cover).
    bool coverJpeg(std::string& out);

private:
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    struct Impl;
    Impl* d;
};

} // namespace pdf
