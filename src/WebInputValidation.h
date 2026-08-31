#pragma once

#include <windows.h>
#include <objbase.h>
#include <oleacc.h>
#include <uiautomation.h>

#include <algorithm>
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

// Top-level classes of browsers that render pages into a child surface. Such a
// window may expose its own chrome (tabs, address bar) through UI Automation
// while exposing nothing at all for the page, which is what happens when the
// renderer is started with --disable-renderer-accessibility.
inline bool IsBrowserWindowClass(std::wstring_view className) {
    return className == L"Chrome_WidgetWin_1" ||   // Chrome, Edge, Chromium forks
           className == L"Chrome_WidgetWin_0" ||
           className == L"MozillaWindowClass";     // Firefox
}

// The child window Chromium draws page content into. Its rectangle excludes the
// tab strip and toolbar, so it tells us whether a pick landed on the page.
inline bool IsWebContentWindowClass(std::wstring_view className) {
    return className == L"Chrome_RenderWidgetHostHWND";
}

// A pick point remembered relative to the nearest edges of the page viewport.
// Chat composers sit at the bottom of the page, so measuring from the closest
// edge keeps the point on the composer when the window is later resized.
struct PickAnchor {
    LONG offsetX = 0;
    LONG offsetY = 0;
    bool fromRight = false;
    bool fromBottom = false;
};

inline PickAnchor MakePickAnchor(const RECT& frame, POINT point) {
    PickAnchor anchor;
    const LONG width = frame.right - frame.left;
    const LONG height = frame.bottom - frame.top;
    anchor.fromRight = width > 0 && (point.x - frame.left) * 2 > width;
    anchor.fromBottom = height > 0 && (point.y - frame.top) * 2 > height;
    anchor.offsetX = anchor.fromRight ? frame.right - point.x : point.x - frame.left;
    anchor.offsetY = anchor.fromBottom ? frame.bottom - point.y : point.y - frame.top;
    return anchor;
}

inline POINT ResolvePickAnchor(const RECT& frame, const PickAnchor& anchor) {
    POINT point{anchor.fromRight ? frame.right - anchor.offsetX : frame.left + anchor.offsetX,
                anchor.fromBottom ? frame.bottom - anchor.offsetY : frame.top + anchor.offsetY};
    // An anchored offset can fall outside a viewport that has since shrunk.
    point.x = std::clamp(point.x, frame.left, (std::max)(frame.right - 1, frame.left));
    point.y = std::clamp(point.y, frame.top, (std::max)(frame.bottom - 1, frame.top));
    return point;
}

}  // namespace webinput
