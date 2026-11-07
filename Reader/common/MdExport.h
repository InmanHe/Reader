/**
 * @file MdExport.h
 * @brief Export an opened book (TXT/EPUB/MOBI/AZW3/PDF/cached online book)
 *        to Markdown that NotebookLM can import as a source.
 *
 * GUI:  File -> Export Markdown
 * CLI:  Reader.exe --to-md <file|folder>... [-o <file|folder>]
 *       (use "start /wait" in batch files: Reader is a GUI program)
 */

#pragma once
#include <string>
#include <vector>
#include "IBook.h"

namespace MdExport
{
    // Writes <outFile>, or <stem>.part1.md, <stem>.part2.md... when the book exceeds
    // the NotebookLM per-source limit. skipped = chapters with no downloaded content.
    BookErrno exportBook(IBook* book, const std::wstring& outFile, std::vector<std::wstring>& written, int* skipped = nullptr);

    // <dir>\<book file name>.md
    std::wstring defaultOutputPath(const std::wstring& bookFile);

    // Handles "--to-md". Returns false when the command line does not ask for it.
    bool runCommandLine(int& exitCode);
}
