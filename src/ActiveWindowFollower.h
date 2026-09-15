#pragma once

#include <windows.h>

// Moves the foreground window with the pointer while Left Shift is held.
// Poll() supplies the live keyboard/window/cursor state; Update() is public so
// the state machine can also be exercised without synthesising global input.
class ActiveWindowFollower {
public:
    void Poll();
    void Update(bool leftShiftAloneDown, HWND activeWindow, POINT cursorPosition);
    void Stop();

private:
    HWND  m_window = nullptr;
    POINT m_cursorOffset{};
};
