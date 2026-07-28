#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace util {

// Locale-aware iswspace/towlower are out-of-line calls that dominate any
// per-character loop. Caption text is overwhelmingly ASCII, so short-circuit
// that range and only fall back to the locale for the rest.
bool IsSpaceNonAscii(wchar_t c);

inline bool IsSpace(wchar_t c) {
    if (c < 0x80) return c == L' ' || (c >= 0x09 && c <= 0x0D);
    return IsSpaceNonAscii(c);
}

std::string  ToUtf8(std::wstring_view text);
std::wstring FromUtf8(std::string_view text);

// Collapses runs of whitespace and trims the ends.
std::wstring CollapseWhitespace(std::wstring_view text);

// Lowercased, with surrounding punctuation removed. Used to compare words
// across snapshots, because the caption engines revise casing and punctuation
// as their confidence improves ("the java" becomes "The Java.").
std::wstring NormalizeWord(std::wstring_view word);

std::vector<std::wstring> SplitWords(std::wstring_view text);

// Counts words without allocating. Used to map a character offset back onto a
// word index on the hot path.
size_t CountWords(std::wstring_view text);

// Length of the longest common prefix of two strings.
size_t CommonPrefixLength(std::wstring_view a, std::wstring_view b);

// True when the word terminates a sentence, allowing for common abbreviations
// and initials so "Dr. Evans" and "e.g." do not create spurious line breaks.
bool IsSentenceEnd(std::wstring_view word);

std::wstring DirectoryOf(const std::wstring& path);
std::wstring ExecutablePath();
std::wstring ExecutableDirectory();

}  // namespace util
