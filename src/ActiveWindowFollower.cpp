#include "ActiveWindowFollower.h"

namespace {

bool IsOnlyLeftShiftDown() {
    if ((::GetAsyncKeyState(VK_LSHIFT) & 0x8000) == 0) return false;

    // Mouse buttons occupy virtual-key values below VK_BACK and are
    // intentionally ignored: they are pointer input, not an additional key.
    // VK_SHIFT mirrors the state of VK_LSHIFT, so it must also be skipped.
    for (int virtualKey = VK_BACK; virtualKey <= 0xFE; ++virtualKey) {
        if (virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT) continue;
        if ((::GetAsyncKeyState(virtualKey) & 0x8000) != 0) return false;
    }
    return true;
}

bool TryGetFollowableWindowBounds(HWND window, RECT& bounds) {
    if (!window || !::IsWindow(window) || !::IsWindowVisible(window) || ::IsIconic(window) ||
        ::IsZoomed(window) || window == ::GetDesktopWindow() || window == ::GetShellWindow()) {
        return false;
    }

    if ((::GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0) return false;
    if (!::GetWindowRect(window, &bounds)) return false;
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

}  // namespace

void ActiveWindowFollower::Poll() {
    if (!IsOnlyLeftShiftDown()) {
        Stop();
        return;
    }

    POINT cursor{};
    if (!::GetCursorPos(&cursor)) return;

    HWND activeWindow = ::GetForegroundWindow();
    if (activeWindow) activeWindow = ::GetAncestor(activeWindow, GA_ROOT);
    Update(true, activeWindow, cursor);
}

void ActiveWindowFollower::Update(bool leftShiftAloneDown, HWND activeWindow,
                                  POINT cursorPosition) {
    if (!leftShiftAloneDown) {
        Stop();
        return;
    }

    RECT bounds{};
    if (!TryGetFollowableWindowBounds(activeWindow, bounds)) {
        Stop();
        return;
    }

    // A newly active window gets a fresh anchor. It must not jump to an offset
    // captured from a previously active window while Shift remains held.
    if (activeWindow != m_window) {
        m_window = activeWindow;
        m_cursorOffset.x = cursorPosition.x - bounds.left;
        m_cursorOffset.y = cursorPosition.y - bounds.top;
        return;
    }

    const int left = cursorPosition.x - m_cursorOffset.x;
    const int top = cursorPosition.y - m_cursorOffset.y;
    if (left == bounds.left && top == bounds.top) return;

    // Cross-process windows can own a different input queue. Posting the move
    // keeps a hung foreground application from blocking Live Caption View.
    ::SetWindowPos(activeWindow, nullptr, left, top, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void ActiveWindowFollower::Stop() {
    m_window = nullptr;
    m_cursorOffset = {};
}
