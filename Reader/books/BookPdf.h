/**
 * @file BookPdf.h
 * @brief PDF book: re-flowable text extracted with the built-in pdf::Document.
 *
 * Design Concept:
 * The PDF is memory-mapped and its text is extracted page by page, then
 * paragraphs are stitched across page breaks, running headers/footers are
 * dropped and the result is handed to the normal text layout engine.
 * Chapters come from the PDF bookmarks (outlines); when a file has no or only
 * a few bookmarks the user's chapter rule (default / keyword / regex) from
 * BookText is used instead, exactly like a TXT book.
 * PDF is read-only: editing is disabled.
 */

#pragma once
#include "BookText.h"

class BookPdf : public BookText
{
public:
    BookPdf(const std::wstring& fileName);
    virtual ~BookPdf();
    virtual BookType type() const;
    virtual bool isEditSupport() const;

protected:
    virtual BookErrno parseBook();
};
