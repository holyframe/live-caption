#include <cstdio>
#include <string>

#include "CaptionSave.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%-66s %s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++g_failures;
}

}  // namespace

int main() {
    SYSTEMTIME time{};
    time.wYear = 2026;
    time.wMonth = 7;
    time.wDay = 29;
    time.wHour = 8;
    time.wMinute = 9;
    time.wSecond = 10;
    time.wMilliseconds = 123;

    Check(caption_save::BuildFileName(time, L"Project planning") ==
              L"2026-07-29 08-09-10-123 - Project planning.txt",
          "filename contains save time and selected tab name");
    Check(caption_save::SanitizeTabName(L"Roadmap: Q3/Q4?") ==
              L"Roadmap_ Q3_Q4_",
          "invalid Windows filename characters are replaced");
    Check(caption_save::SanitizeTabName(L"A<>:\"/\\|?*B") == L"A_B",
          "runs of invalid filename characters collapse");
    Check(caption_save::SanitizeTabName(L"   ...") == L"Captions",
          "missing or unusable tab names use the Captions fallback");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "All checks passed" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
