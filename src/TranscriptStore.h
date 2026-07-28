#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Appends finalised caption lines to a UTF-8 transcript file.
//
// The Python original rewrote the entire file on every 100 ms tick, so the I/O
// cost grew with the length of the session. Here only newly stabilised lines
// are appended, which keeps each write proportional to the new text.
class TranscriptStore {
public:
    ~TranscriptStore();

    TranscriptStore() = default;
    TranscriptStore(const TranscriptStore&) = delete;
    TranscriptStore& operator=(const TranscriptStore&) = delete;

    // Opens (or creates) the file for appending and writes a session header.
    bool Open(const std::wstring& path);
    void Close();

    bool IsOpen() const { return m_file != INVALID_HANDLE_VALUE; }
    const std::wstring& Path() const { return m_path; }

    void AppendLines(const std::vector<std::wstring>& lines, size_t begin, size_t end);
    void Flush();

private:
    void WriteUtf8(std::wstring_view text);

    HANDLE       m_file = INVALID_HANDLE_VALUE;
    std::wstring m_path;
};
