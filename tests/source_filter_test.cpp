#include <cstdio>
#include <string_view>

#include "CaptionSourceMatcher.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%-66s %s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++g_failures;
}

bool Matches(CaptionSourceChoice choice, std::wstring_view process,
             std::wstring_view title) {
    return caption_source_detail::IsWindowForChoice(choice, process, title);
}

}  // namespace

int main() {
    Check(Matches(CaptionSourceChoice::WindowsLiveCaptions, L"livecaptions.exe",
                  L"live captions"),
          "Windows selection accepts the Windows Live Captions process");
    Check(!Matches(CaptionSourceChoice::WindowsLiveCaptions, L"chrome.exe",
                   L"live caption"),
          "Windows selection rejects Chrome Live Caption");
    Check(Matches(CaptionSourceChoice::Chrome, L"chrome.exe", L"live caption"),
          "Chrome selection accepts a Chrome Live Caption window");
    Check(!Matches(CaptionSourceChoice::Chrome, L"livecaptions.exe",
                   L"live captions"),
          "Chrome selection rejects the Windows Live Captions window");
    Check(!Matches(CaptionSourceChoice::Chrome, L"chrome.exe", L"new tab"),
          "Chrome selection rejects ordinary Chrome windows");
    Check(!Matches(CaptionSourceChoice::Chrome, L"msedge.exe", L"live caption"),
          "Chrome selection rejects non-Chrome browser caption windows");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "All checks passed" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
