#include "Settings.h"

#include <windows.h>

#include <cstdlib>

#include "Util.h"

namespace {

constexpr const wchar_t* kSection = L"LiveCaptionView";

std::wstring ReadString(const wchar_t* key, const std::wstring& fallback,
                        const std::wstring& file) {
    wchar_t buffer[1024]{};
    ::GetPrivateProfileStringW(kSection, key, fallback.c_str(), buffer,
                               static_cast<DWORD>(std::size(buffer)), file.c_str());
    return buffer;
}

int ReadInt(const wchar_t* key, int fallback, const std::wstring& file) {
    return static_cast<int>(::GetPrivateProfileIntW(kSection, key, fallback, file.c_str()));
}

void WriteString(const wchar_t* key, const std::wstring& value, const std::wstring& file) {
    ::WritePrivateProfileStringW(kSection, key, value.c_str(), file.c_str());
}

void WriteInt(const wchar_t* key, int value, const std::wstring& file) {
    WriteString(key, std::to_wstring(value), file);
}

}  // namespace

std::wstring Settings::FilePath() {
    return util::ExecutableDirectory() + L"\\LiveCaptionView.ini";
}

std::wstring Settings::ResolvedTranscriptPath() const {
    if (!transcriptPath.empty()) return transcriptPath;
    return util::ExecutableDirectory() + L"\\captions.txt";
}

void Settings::Load() {
    const std::wstring file = FilePath();
    if (::GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    fontFamily  = ReadString(L"FontFamily", fontFamily, file);
    fontSizePt  = ReadInt(L"FontSizePt", fontSizePt, file);
    lineSpacing = _wtof(ReadString(L"LineSpacing", L"1.3", file).c_str());
    if (lineSpacing < 0.8 || lineSpacing > 4.0) lineSpacing = 1.3;
    if (fontSizePt < 6 || fontSizePt > 96) fontSizePt = 12;

    pressEnter   = ReadInt(L"PressEnter", pressEnter ? 1 : 0, file) != 0;
    copyRealtime = ReadInt(L"CopyRealtime", copyRealtime ? 1 : 0, file) != 0;
    compactView  = ReadInt(L"CompactView", compactView ? 1 : 0, file) != 0;
    alwaysOnTop  = ReadInt(L"AlwaysOnTop", alwaysOnTop ? 1 : 0, file) != 0;

    transcriptPath = ReadString(L"TranscriptPath", L"", file);

    hotkeyModifiers  = static_cast<unsigned>(ReadInt(L"HotkeyModifiers", static_cast<int>(hotkeyModifiers), file));
    hotkeyVirtualKey = static_cast<unsigned>(ReadInt(L"HotkeyVirtualKey", static_cast<int>(hotkeyVirtualKey), file));

    windowX = ReadInt(L"WindowX", windowX, file);
    windowY = ReadInt(L"WindowY", windowY, file);
    windowW = ReadInt(L"WindowW", windowW, file);
    windowH = ReadInt(L"WindowH", windowH, file);
}

void Settings::Save() const {
    const std::wstring file = FilePath();

    WriteString(L"FontFamily", fontFamily, file);
    WriteInt(L"FontSizePt", fontSizePt, file);

    wchar_t spacing[32];
    ::swprintf_s(spacing, L"%.2f", lineSpacing);
    WriteString(L"LineSpacing", spacing, file);

    WriteInt(L"PressEnter", pressEnter ? 1 : 0, file);
    WriteInt(L"CopyRealtime", copyRealtime ? 1 : 0, file);
    WriteInt(L"CompactView", compactView ? 1 : 0, file);
    WriteInt(L"AlwaysOnTop", alwaysOnTop ? 1 : 0, file);
    WriteString(L"TranscriptPath", transcriptPath, file);

    WriteInt(L"HotkeyModifiers", static_cast<int>(hotkeyModifiers), file);
    WriteInt(L"HotkeyVirtualKey", static_cast<int>(hotkeyVirtualKey), file);

    WriteInt(L"WindowX", windowX, file);
    WriteInt(L"WindowY", windowY, file);
    WriteInt(L"WindowW", windowW, file);
    WriteInt(L"WindowH", windowH, file);
}
