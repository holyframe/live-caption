#pragma once

#include <windows.h>
#include <objbase.h>
#include <oleacc.h>
#include <uiautomation.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace webinput {

enum class WriteAccess { Unknown, ReadOnly, Writable };

struct InputCapabilities {
    CONTROLTYPEID type = 0;
    bool enabled = false;
    bool focusable = false;
    bool offscreen = true;
    bool password = true;
    WriteAccess value = WriteAccess::Unknown;
    WriteAccess text = WriteAccess::Unknown;
};

inline bool CanEdit(const InputCapabilities& input) {
    if (!input.enabled || !input.focusable || input.offscreen || input.password) return false;
    switch (input.type) {
        case UIA_EditControlTypeId:
        case UIA_DocumentControlTypeId:
        case UIA_CustomControlTypeId:
        case UIA_PaneControlTypeId:
        case UIA_GroupControlTypeId:
            break;
        default:
            return false;
    }
    // TextPattern alone also exists on read-only pages. Require affirmative
    // write support, and never override an explicit read-only ValuePattern.
    if (input.value == WriteAccess::ReadOnly) return false;
    return input.value == WriteAccess::Writable || input.text == WriteAccess::Writable;
}

constexpr ULONGLONG kInspectionCacheMs = 750;

inline bool CanReuseInspection(HWND window, HWND previous, ULONGLONG now,
                               ULONGLONG inspectedAt, bool forceRefresh) {
    return !forceRefresh && window && window == previous &&
           now - inspectedAt < kInspectionCacheMs;
}

inline std::wstring NormalizeComparableText(std::wstring_view text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (character == L'\r') {
            normalized.push_back(L'\n');
            if (index + 1 < text.size() && text[index + 1] == L'\n') ++index;
            continue;
        }
        normalized.push_back(character);
    }
    return normalized;
}

inline bool TextMatches(std::wstring_view actual, std::wstring_view expected) {
    return NormalizeComparableText(actual) == NormalizeComparableText(expected);
}

inline bool ComposerIsEmpty(std::wstring_view text) {
    for (const wchar_t character : text) {
        if (!std::iswspace(static_cast<wint_t>(character)) &&
            character != L'\u200B' && character != L'\uFEFF') {
            return false;
        }
    }
    return true;
}

}  // namespace webinput
