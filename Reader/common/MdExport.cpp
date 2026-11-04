#include "MdExport.h"
#include "MdRender.h"
#include "BookSession.h"
#include "JsonSerialize.h"
#include "Utils.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <cstdio>

namespace
{

std::wstring stripExt(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) return path.substr(0, dot);
    return path;
}

std::wstring baseName(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    wchar_t last = dir[dir.size() - 1];
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

bool isDirectory(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool writeFile(const std::wstring& path, const std::string& data)
{
    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), fp) == data.size();
    ok = (fclose(fp) == 0) && ok;
    return ok;
}

// The command line must not touch Settings::instance(): it writes the settings file back
// on exit and would overwrite the reading progress of a running Reader. Read-only instead.
ChapterRule loadChapterRule()
{
    ChapterRule rule;
    rule.rule = ChapterRule::DEFAULT;
    FILE* fp = _wfopen((Utils::appPath() + SETTINGS_FILE_NAME).c_str(), L"rb");
    if (!fp) return rule;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size > 0 ? (size_t)size : 0);
    bool ok = size > 0 && fread(bytes.data(), 1, bytes.size(), fp) == bytes.size();
    fclose(fp);
    Data data;
    data.chapterRule.rule = ChapterRule::DEFAULT;
    if (ok && JsonSerialize::deserialize(bytes, data)) rule = data.chapterRule;
    return rule;
}

// Console output for a GUI-subsystem program started from cmd / PowerShell / a pipe.
class Console
{
public:
    Console() : m_out(NULL), m_isConsole(false)
    {
        m_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!m_out || m_out == INVALID_HANDLE_VALUE)
        {
            typedef BOOL(WINAPI* AttachConsoleProc)(DWORD);
            AttachConsoleProc attach = (AttachConsoleProc)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AttachConsole");
            if (attach && attach((DWORD)-1 /*ATTACH_PARENT_PROCESS*/)) m_out = GetStdHandle(STD_OUTPUT_HANDLE);
        }
        DWORD mode;
        m_isConsole = m_out && m_out != INVALID_HANDLE_VALUE && GetConsoleMode(m_out, &mode);
        print(L"\r\n");
    }

    void print(const std::wstring& s)
    {
        if (!m_out || m_out == INVALID_HANDLE_VALUE) return;
        DWORD n;
        if (m_isConsole) WriteConsoleW(m_out, s.c_str(), (DWORD)s.size(), &n, NULL);
        else
        {
            std::string u8 = md::toUtf8(s);
            WriteFile(m_out, u8.data(), (DWORD)u8.size(), &n, NULL);
        }
    }

private:
    HANDLE m_out;
    bool   m_isConsole;
};

const wchar_t* errorText(BookErrno err)
{
    switch (err)
    {
    case BookErrno::NOT_EXIST:      return L"file not found";
    case BookErrno::EMPTY:          return L"empty file";
    case BookErrno::UNKNOWN_FORMAT: return L"unsupported format";
    case BookErrno::TOO_LARGE:      return L"file too large";
    case BookErrno::ENCRYPTED:      return L"encrypted, password required";
    case BookErrno::NO_TEXT:        return L"no extractable text (scanned PDF?)";
    case BookErrno::OPEN_W_FAIL:
    case BookErrno::WRITE_FAIL:     return L"cannot write output";
    default:                        return L"cannot parse file";
    }
}

} // namespace

namespace MdExport
{

std::wstring defaultOutputPath(const std::wstring& bookFile)
{
    return stripExt(bookFile) + L".md";
}

BookErrno exportBook(IBook* book, const std::wstring& outFile, std::vector<std::wstring>& written, int* skipped)
{
    written.clear();
    if (skipped) *skipped = 0;
    if (!book || !book->text()) return BookErrno::EMPTY;

    std::vector<md::Heading> headings;
    for (int i = 0; i < book->chapterCount(); i++)
    {
        const Chapter* c = book->chapter(i);
        if (!c) continue;
        md::Heading h;
        h.index = c->index;
        h.titleLineLen = c->titleLineLen;
        h.level = c->level;
        h.title = c->title;
        headings.push_back(h);
    }

    md::RenderResult r = md::render(book->text(), (size_t)book->length(), headings, book->bookName(), baseName(book->fileName()));
    if (skipped) *skipped = r.skippedChapters;

    std::wstring stem = stripExt(outFile);
    for (size_t i = 0; i < r.parts.size(); i++)
    {
        std::wstring path = r.parts.size() == 1 ? outFile : stem + L".part" + std::to_wstring(i + 1) + L".md";
        if (!writeFile(path, r.parts[i])) return BookErrno::WRITE_FAIL;
        written.push_back(path);
    }
    return BookErrno::SUCC;
}

bool runCommandLine(int& exitCode)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    bool requested = false;
    std::vector<std::wstring> inputs;
    std::wstring output;
    for (int i = 1; i < argc; i++)
    {
        std::wstring a = argv[i];
        if (a == L"--to-md" || a == L"-md" || a == L"/md") requested = true;
        else if ((a == L"-o" || a == L"--output") && i + 1 < argc) output = argv[++i];
        else inputs.push_back(a);
    }
    LocalFree(argv);
    if (!requested) return false;

    Console con;
    if (inputs.empty())
    {
        con.print(L"Usage: Reader.exe --to-md <book file|folder>... [-o <output file|folder>]\r\n"
                  L"Formats: txt, epub, mobi, azw3, azw, prc, pdf\r\n");
        exitCode = 2;
        return true;
    }

    // expand folders (non-recursive) into the supported books they contain
    std::vector<std::wstring> files;
    for (const auto& in : inputs)
    {
        wchar_t full[MAX_PATH * 4];
        DWORD n = GetFullPathNameW(in.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, NULL);
        std::wstring path = (n > 0 && n < sizeof(full) / sizeof(full[0])) ? std::wstring(full) : in;
        if (!isDirectory(path)) { files.push_back(path); continue; }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(joinPath(path, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring f = joinPath(path, fd.cFileName);
            if (BookSession::isSupportedFile(f)) files.push_back(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    bool outIsDir = !output.empty() && (isDirectory(output) || files.size() > 1 ||
                    output[output.size() - 1] == L'\\' || output[output.size() - 1] == L'/');
    if (outIsDir && !isDirectory(output)) CreateDirectoryW(output.c_str(), NULL);

    ChapterRule rule = loadChapterRule();
    int failed = 0;
    for (const auto& file : files)
    {
        con.print(file + L"\r\n");
        BookErrno err = BookErrno::UNKNOWN_FORMAT;
        IBook* book = Utils::isFileExist(file) ? BookSession::createBook(file, NULL) : nullptr;
        if (!Utils::isFileExist(file)) err = BookErrno::NOT_EXIST;
        if (book)
        {
            book->setChapterRule(&rule);
            err = book->open();
            if (err != BookErrno::SUCC && book->length() > 0) err = BookErrno::SUCC; // partially readable
        }
        if (err == BookErrno::SUCC)
        {
            std::wstring out = output.empty() ? defaultOutputPath(file)
                             : outIsDir ? joinPath(output, baseName(stripExt(file)) + L".md")
                             : output;
            std::vector<std::wstring> written;
            int skipped = 0;
            err = exportBook(book, out, written, &skipped);
            for (const auto& w : written) con.print(L"  -> " + w + L"\r\n");
            if (skipped > 0) con.print(L"  (" + std::to_wstring(skipped) + L" chapters without content skipped)\r\n");
        }
        if (err != BookErrno::SUCC)
        {
            con.print(std::wstring(L"  failed: ") + errorText(err) + L"\r\n");
            failed++;
        }
        delete book;
    }
    con.print(std::to_wstring(files.size() - failed) + L"/" + std::to_wstring(files.size()) + L" converted.\r\n");
    exitCode = failed ? 1 : 0;
    return true;
}

} // namespace MdExport
