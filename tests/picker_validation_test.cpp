#include "WebInputValidation.h"

#include <cstdio>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    std::printf("%-72s %s\n", name, condition ? "PASS" : "FAIL");
    if (!condition) ++failures;
}
}  // namespace

int main() {
    using webinput::WriteAccess;
    webinput::InputCapabilities input;
    input.type = UIA_EditControlTypeId;
    input.enabled = input.focusable = true;
    input.offscreen = input.password = false;
    input.value = WriteAccess::Writable;
    Check(webinput::CanEdit(input), "normal writable HTML input accepted");
    input.type = UIA_DocumentControlTypeId;
    Check(webinput::CanEdit(input), "writable Document composer accepted (not just Edit)");
    input.value = WriteAccess::Unknown;
    input.text = WriteAccess::Writable;
    Check(webinput::CanEdit(input), "TextPattern-only writable Document accepted");
    input.type = UIA_CustomControlTypeId;
    Check(webinput::CanEdit(input), "writable custom web composer accepted");
    input.type = UIA_PaneControlTypeId;
    Check(webinput::CanEdit(input), "writable pane web composer accepted");
    input.type = UIA_GroupControlTypeId;
    Check(webinput::CanEdit(input), "writable group web composer accepted");
    input.type = UIA_ButtonControlTypeId;
    Check(!webinput::CanEdit(input), "non-text control rejected even with text capability");
    input.type = UIA_DocumentControlTypeId;
    input.text = WriteAccess::ReadOnly;
    Check(!webinput::CanEdit(input), "ordinary read-only web document rejected");
    input.text = WriteAccess::Unknown;
    Check(!webinput::CanEdit(input), "missing/mixed/unsupported read-only attribute rejected");
    input.type = UIA_EditControlTypeId;
    Check(!webinput::CanEdit(input), "Edit with unproven write support rejected");
    input.value = WriteAccess::ReadOnly;
    input.text = WriteAccess::Writable;
    Check(!webinput::CanEdit(input), "read-only ValuePattern cannot fall through to TextPattern");
    input.value = WriteAccess::Writable;
    input.enabled = false;
    Check(!webinput::CanEdit(input), "disabled input rejected");
    input.enabled = true;
    input.focusable = false;
    Check(!webinput::CanEdit(input), "non-focusable input rejected");
    input.focusable = true;
    input.offscreen = true;
    Check(!webinput::CanEdit(input), "hidden/background input rejected");
    input.offscreen = false;
    input.password = true;
    Check(!webinput::CanEdit(input), "password input rejected");

    const HWND window = reinterpret_cast<HWND>(1);
    const HWND other = reinterpret_cast<HWND>(2);
    const ULONGLONG checkedAt = 1000;
    Check(webinput::CanReuseInspection(window, window,
                                     checkedAt + webinput::kInspectionCacheMs - 1, checkedAt, false),
          "mouse movement reuses a recent result briefly");
    Check(!webinput::CanReuseInspection(window, window,
                                      checkedAt + webinput::kInspectionCacheMs, checkedAt, false),
          "negative detection expires at retry boundary");
    Check(!webinput::CanReuseInspection(window, window, 3000, checkedAt, false),
          "hover retries even when the top-level HWND has not changed");
    Check(!webinput::CanReuseInspection(window, window, 1001, checkedAt, true),
          "drop bypasses cached successes AND failures");
    Check(!webinput::CanReuseInspection(other, window, 1001, checkedAt, false),
          "entering a different window never reuses the prior result");
    Check(!webinput::CanReuseInspection(window, nullptr, 1001, checkedAt, false),
          "a new drag starts with no reusable result");
    Check(!webinput::CanReuseInspection(nullptr, nullptr, 1001, checkedAt, false),
          "no-window hit never reuses a result");

    Check(webinput::TextMatches(L"first\r\nsecond", L"first\nsecond"),
          "read-back verification treats CRLF and LF as the same text");
    Check(webinput::TextMatches(L"first\rsecond", L"first\nsecond"),
          "read-back verification normalizes lone carriage returns");
    Check(!webinput::TextMatches(L"caption", L"caption "),
          "read-back verification does not ignore meaningful whitespace");
    Check(!webinput::TextMatches(L"caption", L"different"),
          "read-back verification rejects different input contents");
    Check(webinput::TextMatches(L"", L""),
          "an empty composer can confirm submission");
    Check(webinput::ComposerIsEmpty(L"\r\n\t\u200B"),
          "structural whitespace in an empty rich composer confirms submission");
    Check(!webinput::ComposerIsEmpty(L"caption"),
          "visible composer text does not confirm submission");

    std::printf("\nPicker validation: %d failures\n", failures);
    return failures ? 1 : 0;
}
