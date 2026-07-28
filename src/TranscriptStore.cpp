#include "TranscriptStore.h"

#include "Util.h"

TranscriptStore::~TranscriptStore() { Close(); }

bool TranscriptStore::Open(const std::wstring& path) {
    Close();

    m_file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_file == INVALID_HANDLE_VALUE) return false;

    m_path = path;

    LARGE_INTEGER size{};
    const bool isNew = ::GetFileSizeEx(m_file, &size) && size.QuadPart == 0;
    if (isNew) {
        // UTF-8 BOM so Notepad and Excel detect the encoding correctly.
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        ::WriteFile(m_file, bom, sizeof(bom), &written, nullptr);
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t header[128];
    ::swprintf_s(header, L"%s----- session %04u-%02u-%02u %02u:%02u:%02u -----\r\n",
                 isNew ? L"" : L"\r\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                 now.wSecond);
    WriteUtf8(header);
    return true;
}

void TranscriptStore::Close() {
    if (m_file != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(m_file);
        ::CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }
    m_path.clear();
}

void TranscriptStore::AppendLines(const std::vector<std::wstring>& lines, size_t begin, size_t end) {
    if (!IsOpen() || begin >= end) return;
    end = std::min(end, lines.size());

    std::wstring batch;
    for (size_t i = begin; i < end; ++i) {
        batch += lines[i];
        batch += L"\r\n";
    }
    WriteUtf8(batch);
}

void TranscriptStore::WriteUtf8(std::wstring_view text) {
    if (!IsOpen() || text.empty()) return;
    const std::string utf8 = util::ToUtf8(text);
    DWORD written = 0;
    ::WriteFile(m_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void TranscriptStore::Flush() {
    if (IsOpen()) ::FlushFileBuffers(m_file);
}
