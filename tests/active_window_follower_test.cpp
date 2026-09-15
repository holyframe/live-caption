#include <windows.h>

#include <iostream>

#include "ActiveWindowFollower.h"

namespace {

bool ExpectBounds(HWND window, int left, int top, int width, int height, const wchar_t* step) {
    RECT bounds{};
    if (!::GetWindowRect(window, &bounds)) {
        std::wcerr << step << L": GetWindowRect failed\n";
        return false;
    }

    if (bounds.left != left || bounds.top != top || bounds.right - bounds.left != width ||
        bounds.bottom - bounds.top != height) {
        std::wcerr << step << L": expected (" << left << L", " << top << L", " << width << L", "
                   << height << L"), got (" << bounds.left << L", " << bounds.top << L", "
                   << bounds.right - bounds.left << L", " << bounds.bottom - bounds.top << L")\n";
        return false;
    }
    return true;
}

}  // namespace

int wmain() {
    constexpr int kFirstWidth = 300;
    constexpr int kFirstHeight = 200;
    constexpr int kSecondWidth = 240;
    constexpr int kSecondHeight = 180;

    HWND first = ::CreateWindowExW(0, L"STATIC", L"Follower test 1",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 120, kFirstWidth,
                                    kFirstHeight, nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                    nullptr);
    HWND second = ::CreateWindowExW(0, L"STATIC", L"Follower test 2",
                                     WS_OVERLAPPEDWINDOW | WS_VISIBLE, 500, 160, kSecondWidth,
                                     kSecondHeight, nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                     nullptr);
    if (!first || !second) {
        std::wcerr << L"Could not create test windows\n";
        if (first) ::DestroyWindow(first);
        if (second) ::DestroyWindow(second);
        return 1;
    }

    ActiveWindowFollower follower;
    bool ok = true;

    // Pressing Left Shift establishes the grab offset without moving the window.
    follower.Update(true, first, POINT{150, 170});
    ok = ExpectBounds(first, 100, 120, kFirstWidth, kFirstHeight, L"initial anchor") && ok;

    // Cursor deltas are applied to the window while its size is preserved.
    follower.Update(true, first, POINT{225, 260});
    ok = ExpectBounds(first, 175, 210, kFirstWidth, kFirstHeight, L"follow movement") && ok;

    // Changing the active window while Shift remains down creates a new anchor.
    follower.Update(true, second, POINT{560, 220});
    ok = ExpectBounds(second, 500, 160, kSecondWidth, kSecondHeight, L"replacement anchor") && ok;
    follower.Update(true, second, POINT{530, 270});
    ok = ExpectBounds(second, 470, 210, kSecondWidth, kSecondHeight, L"replacement movement") && ok;
    ok = ExpectBounds(first, 175, 210, kFirstWidth, kFirstHeight, L"old window stopped") && ok;

    // Releasing Shift clears the grab, so the next press anchors at the then-current position.
    follower.Update(false, second, POINT{});
    follower.Update(true, second, POINT{600, 300});
    follower.Update(true, second, POINT{610, 285});
    ok = ExpectBounds(second, 480, 195, kSecondWidth, kSecondHeight, L"release and re-anchor") && ok;

    ::DestroyWindow(second);
    ::DestroyWindow(first);

    if (!ok) return 1;
    std::wcout << L"ActiveWindowFollower checks passed.\n";
    return 0;
}
