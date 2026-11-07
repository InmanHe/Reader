#include "BookPdf.h"
#include "PdfText.h"
#include "Logger.h"

namespace {

// Read-only memory mapping: large PDFs are mostly images, no need to copy them into the heap.
class MappedFile
{
public:
    explicit MappedFile(const std::wstring& path)
        : m_file(INVALID_HANDLE_VALUE), m_map(NULL), m_view(NULL), m_size(0)
    {
        m_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (m_file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(m_file, &size) || size.QuadPart <= 0 || (unsigned long long)size.QuadPart > (size_t)-1) return;
        m_map = CreateFileMappingW(m_file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!m_map) return;
        m_view = MapViewOfFile(m_map, FILE_MAP_READ, 0, 0, 0);
        if (m_view) m_size = (size_t)size.QuadPart;
    }
    ~MappedFile()
    {
        if (m_view) UnmapViewOfFile(m_view);
        if (m_map) CloseHandle(m_map);
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
    }
    bool opened() const { return m_file != INVALID_HANDLE_VALUE; }
    const char* data() const { return (const char*)m_view; }
    size_t size() const { return m_size; }

private:
    HANDLE m_file;
    HANDLE m_map;
    void*  m_view;
    size_t m_size;
};

bool isBlankText(const std::wstring& t)
{
    for (wchar_t c : t)
        if (c != L'\n' && c != L' ' && c != L'\t' && c != 0x3000) return false;
    return true;
}

} // namespace

BookPdf::BookPdf(const std::wstring& fileName)
    : BookText(fileName)
{
}

BookPdf::~BookPdf()
{
}

BookType BookPdf::type() const
{
    return BookType::PDF;
}

bool BookPdf::isEditSupport() const
{
    return false; // never overwrite the PDF with plain text
}

BookErrno BookPdf::parseBook()
{
    MappedFile file(m_fileName);
    if (!file.opened())
    {
        lgerror("Failed to open pdf file. Error code: %lu", GetLastError());
        return BookErrno::OPEN_R_FAIL;
    }
    if (!file.data()) return file.size() == 0 ? BookErrno::EMPTY : BookErrno::TOO_LARGE;

    pdf::Document doc;
    switch (doc.open(file.data(), file.size(), &m_abort))
    {
    case pdf::Status::OK:        break;
    case pdf::Status::NOT_PDF:   return BookErrno::UNKNOWN_FORMAT;
    case pdf::Status::ENCRYPTED: return BookErrno::ENCRYPTED;
    case pdf::Status::ABORTED:   return BookErrno::FAIL;
    default:                     return BookErrno::PARSE_FAIL;
    }

    pdf::BookTextResult result;
    if (!pdf::buildBookText(doc, result, &m_abort)) return BookErrno::FAIL;
    if (isBlankText(result.text))
    {
        lgwarn("PDF has no extractable text (scanned?).");
        return BookErrno::NO_TEXT;
    }

    // A first page that is mostly a picture is the cover; shown like EPUB covers.
    std::string jpeg;
    int offset = 0;
    freeCover();
    if (result.firstPageChars < 200 && doc.coverJpeg(jpeg))
    {
        m_cover = (Cover*)malloc(sizeof(Cover));
        if (m_cover)
        {
            m_cover->data = (char*)malloc(jpeg.size());
            m_cover->size = (int)jpeg.size();
            if (m_cover->data) { memcpy(m_cover->data, jpeg.data(), jpeg.size()); offset = 1; }
            else freeCover();
        }
    }
    std::string().swap(jpeg);

    if (result.text.size() + offset >= 0x7fffffff) return BookErrno::TOO_LARGE;
    m_length = (int)result.text.size() + offset;
    m_text = (wchar_t*)malloc(sizeof(wchar_t) * (m_length + 1));
    if (!m_text)
    {
        m_length = 0;
        freeCover();
        return BookErrno::TOO_LARGE;
    }
    if (offset) m_text[0] = 0x000A; // placeholder character for the cover image
    memcpy(m_text + offset, result.text.data(), sizeof(wchar_t) * result.text.size());
    m_text[m_length] = 0;
    std::wstring().swap(result.text);

    // chapters: bookmarks first ...
    Chapters outline;
    for (const auto& c : result.chapters)
    {
        Chapter chapter;
        chapter.index = c.index + offset;
        chapter.titleLineLen = c.titleLen;
        chapter.title = c.title;
        chapter.level = c.level;
        outline.push_back(std::move(chapter));
    }

    // ... unless there are hardly any, then the chapter rule (like TXT) may do better
    m_chapters.clear();
    if (outline.size() < 3)
    {
        if (!parserChapters()) m_chapters.clear();
        if (m_abort) return BookErrno::FAIL;
        if (m_chapters.size() <= outline.size()) m_chapters.swap(outline);
    }
    else
    {
        m_chapters.swap(outline);
    }
    return BookErrno::SUCC;
}
