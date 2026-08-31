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

    Check(webinput::IsBrowserWindowClass(L"Chrome_WidgetWin_1"),
          "Chromium browser shell recognised for the click fallback");
    Check(webinput::IsBrowserWindowClass(L"MozillaWindowClass"),
          "Firefox browser shell recognised for the click fallback");
    Check(!webinput::IsBrowserWindowClass(L"Notepad"),
          "ordinary application windows never get a blind click fallback");
    Check(!webinput::IsBrowserWindowClass(L""), "missing class name is not a browser");
    Check(webinput::IsWebContentWindowClass(L"Chrome_RenderWidgetHostHWND"),
          "Chromium page surface recognised");
    Check(!webinput::IsWebContentWindowClass(L"Chrome_WidgetWin_1"),
          "browser shell is not mistaken for the page surface");

    // A chat composer sits just above the bottom of the page viewport.
    const RECT viewport{100, 100, 900, 700};
    const POINT composer{500, 660};
    const auto bottomAnchor = webinput::MakePickAnchor(viewport, composer);
    Check(bottomAnchor.fromBottom && !bottomAnchor.fromRight && bottomAnchor.offsetY == 40,
          "a point low in the viewport is remembered as an offset from the bottom");
    POINT resolved = webinput::ResolvePickAnchor(viewport, bottomAnchor);
    Check(resolved.x == composer.x && resolved.y == composer.y,
          "an unchanged viewport resolves back to the picked point");
    resolved = webinput::ResolvePickAnchor(RECT{300, 200, 1100, 800}, bottomAnchor);
    Check(resolved.x == 700 && resolved.y == 760, "moving the window carries the point along");
    resolved = webinput::ResolvePickAnchor(RECT{100, 100, 900, 900}, bottomAnchor);
    Check(resolved.y == 860, "a taller window keeps the composer's distance from the bottom");
    resolved = webinput::ResolvePickAnchor(RECT{100, 100, 300, 200}, bottomAnchor);
    Check(resolved.x == 299 && resolved.y == 160,
          "a viewport smaller than the offset still resolves inside itself");

    const auto topAnchor = webinput::MakePickAnchor(viewport, POINT{150, 140});
    Check(!topAnchor.fromBottom && !topAnchor.fromRight && topAnchor.offsetX == 50 &&
              topAnchor.offsetY == 40,
          "a point high in the viewport is remembered as an offset from the top left");
    resolved = webinput::ResolvePickAnchor(RECT{100, 100, 1500, 1500}, topAnchor);
    Check(resolved.x == 150 && resolved.y == 140,
          "a growing window keeps a top-anchored point where it was");

    const auto rightAnchor = webinput::MakePickAnchor(viewport, POINT{880, 660});
    Check(rightAnchor.fromRight && rightAnchor.offsetX == 20,
          "a point near the right edge is remembered as an offset from the right");
    resolved = webinput::ResolvePickAnchor(RECT{100, 100, 1200, 700}, rightAnchor);
    Check(resolved.x == 1180, "a wider window keeps a right-anchored point near the right edge");

    std::printf("\nPicker validation: %d failures\n", failures);
    return failures ? 1 : 0;
}
