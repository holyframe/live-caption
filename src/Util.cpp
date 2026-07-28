#include "Util.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>

namespace util {

bool IsSpaceNonAscii(wchar_t c) { return std::iswspace(static_cast<wint_t>(c)) != 0; }

namespace {

inline bool IsWordChar(wchar_t c) { return !IsSpace(c); }

inline wchar_t ToLowerFast(wchar_t c) {
    if (c < 0x80) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
}

// Punctuation that can wrap a word without being part of it.
bool IsTrimmablePunct(wchar_t c) {
    static constexpr std::wstring_view kPunct = L".,!?;:\"'()[]{}<>\u2014\u2013-\u2018\u2019\u201C\u201D";
    return kPunct.find(c) != std::wstring_view::npos;
}

}  // namespace

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed,
                          nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::wstring CollapseWhitespace(std::wstring_view text) {
    // Caption snapshots are usually already trimmed and single-spaced. Detect
    // that with one cheap scan and copy straight through, rather than rebuilding
    // the whole buffer character by character on every update.
    bool needsRewrite = false;
    if (!text.empty()) {
        if (IsSpace(text.front()) || IsSpace(text.back())) {
            needsRewrite = true;
        } else {
            for (size_t i = 0; i < text.size(); ++i) {
                const wchar_t c = text[i];
                if (!IsSpace(c)) continue;
                // Anything other than a lone plain space needs normalising.
                if (c != L' ' || (i + 1 < text.size() && IsSpace(text[i + 1]))) {
                    needsRewrite = true;
                    break;
                }
            }
        }
    }
    if (!needsRewrite) return std::wstring(text);

    std::wstring out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (wchar_t c : text) {
        if (IsSpace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

std::wstring NormalizeWord(std::wstring_view word) {
    size_t begin = 0;
    size_t end = word.size();
    while (begin < end && IsTrimmablePunct(word[begin])) ++begin;
    while (end > begin && IsTrimmablePunct(word[end - 1])) --end;

    std::wstring out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        out.push_back(ToLowerFast(word[i]));
    }
    return out;
}

std::vector<std::wstring> SplitWords(std::wstring_view text) {
    std::vector<std::wstring> words;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !IsWordChar(text[i])) ++i;
        const size_t start = i;
        while (i < text.size() && IsWordChar(text[i])) ++i;
        if (i > start) words.emplace_back(text.substr(start, i - start));
    }
    return words;
}

size_t CountWords(std::wstring_view text) {
    size_t count = 0;
    bool inWord = false;
    for (wchar_t c : text) {
        if (IsWordChar(c)) {
            if (!inWord) {
                ++count;
                inWord = true;
            }
        } else {
            inWord = false;
        }
    }
    return count;
}

size_t CommonPrefixLength(std::wstring_view a, std::wstring_view b) {
    const size_t limit = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < limit && a[i] == b[i]) ++i;
    return i;
}

bool IsSentenceEnd(std::wstring_view word) {
    // Ignore trailing quotes and brackets so `said."` still terminates.
    size_t end = word.size();
    while (end > 0) {
        const wchar_t c = word[end - 1];
        if (c == L'"' || c == L'\'' || c == L')' || c == L']' || c == L'}' || c == L'\u201D' ||
            c == L'\u2019') {
            --end;
            continue;
        }
        break;
    }
    if (end == 0) return false;

    const wchar_t last = word[end - 1];
    if (last == L'!' || last == L'?') return true;
    if (last != L'.') return false;

    const std::wstring_view stem = word.substr(0, end - 1);
    if (stem.empty()) return false;

    // "A." / "J." are initials, not sentence ends. A lone digit is not treated
    // the same way: speech captions end sentences on numbers ("...is 42.") far
    // more often than they emit list markers.
    if (stem.size() == 1 && std::iswalpha(static_cast<wint_t>(stem[0]))) return false;

    // "e.g." / "U.S." keep internal dots. This also covers decimals such as
    // "3.5", whose final character is not a period at all.
    if (stem.find(L'.') != std::wstring_view::npos) return false;

    // Deliberately short: this transcribes speech, so print abbreviations like
    // "No." or "Fig." are far rarer than someone simply ending a sentence with
    // "no." Only titles that are reliably followed by a name are listed.
    static constexpr std::array<std::wstring_view, 11> kAbbreviations = {
        L"mr", L"mrs", L"ms", L"dr", L"prof", L"jr", L"sr", L"st", L"vs", L"inc", L"ltd"};
    const std::wstring lowered = NormalizeWord(stem);
    for (std::wstring_view abbr : kAbbreviations) {
        if (lowered == abbr) return false;
    }
    return true;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring ExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring ExecutableDirectory() { return DirectoryOf(ExecutablePath()); }

}  // namespace util
