#include "CaptionSave.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace caption_save {
namespace {

constexpr size_t kMaxTabNameLength = 120;

bool IsInvalidFileNameCharacter(wchar_t character) {
    if (character < 32) return true;
    static constexpr std::wstring_view kInvalid = L"<>:\"/\\|?*";
    return kInvalid.find(character) != std::wstring_view::npos;
}

}  // namespace

std::wstring SanitizeTabName(std::wstring_view tabName) {
    std::wstring safe;
    safe.reserve(std::min(tabName.size(), kMaxTabNameLength));

    bool previousReplacement = false;
    for (wchar_t character : tabName) {
        if (safe.size() >= kMaxTabNameLength) break;

        const bool replace = IsInvalidFileNameCharacter(character);
        if (replace) {
            if (!previousReplacement) safe.push_back(L'_');
            previousReplacement = true;
        } else {
            safe.push_back(character);
            previousReplacement = false;
        }
    }

    // Windows ignores trailing spaces and periods in a path component.
    while (!safe.empty() && (std::iswspace(static_cast<wint_t>(safe.back())) ||
                             safe.back() == L'.')) {
        safe.pop_back();
    }
    size_t first = 0;
    while (first < safe.size() &&
           std::iswspace(static_cast<wint_t>(safe[first]))) {
        ++first;
    }
    if (first != 0) safe.erase(0, first);

    return safe.empty() ? L"Captions" : safe;
}

std::wstring BuildFileName(const SYSTEMTIME& saveTime, std::wstring_view tabName) {
    wchar_t timestamp[32]{};
    ::swprintf_s(timestamp, L"%04u-%02u-%02u %02u-%02u-%02u-%03u", saveTime.wYear,
                 saveTime.wMonth, saveTime.wDay, saveTime.wHour, saveTime.wMinute,
                 saveTime.wSecond, saveTime.wMilliseconds);
    return std::wstring(timestamp) + L" - " + SanitizeTabName(tabName) + L".txt";
}

}  // namespace caption_save
