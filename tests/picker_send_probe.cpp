// Picks a target at an explicit screen point and types into it, so the send
// path can be verified against a real browser. Intended for the local test
// fixture only: it injects real keystrokes into whatever window is picked.
#include "../src/WebInputPicker.cpp"

#include <cstdio>
#include <thread>

// Reports who owns input around a send. A caption can vanish while every call
// still succeeds, so this shows whether the browser held the foreground and
// which of its child windows held the keyboard.
void ReportInputOwner(const char* label, HWND target) {
    const HWND foreground = ::GetForegroundWindow();
    wchar_t foregroundClass[128] = L"";
    ::GetClassNameW(foreground, foregroundClass, 128);

    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    wchar_t focusClass[128] = L"";
    bool focusInTarget = false;
    const DWORD thread = ::GetWindowThreadProcessId(target, nullptr);
    if (thread && ::GetGUIThreadInfo(thread, &info) && info.hwndFocus) {
        ::GetClassNameW(info.hwndFocus, focusClass, 128);
        focusInTarget = ::GetAncestor(info.hwndFocus, GA_ROOT) == target;
    }
    std::printf("  %s: active=%d foreground='%ls' focus='%ls' focusInTarget=%d\n", label,
                foreground == target || ::GetAncestor(foreground, GA_ROOT) == target,
                foregroundClass, focusClass, focusInTarget);
}

// Stands in for this app's own window sitting over the browser when Send is
// pressed. Raising the browser only starts when Send asks for it, so for a
// moment the pick point still belongs to the covering window; the send has to
// wait that out instead of hit testing once and giving up.
void CoverPointBriefly(POINT point, DWORD durationMs) {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &::DefWindowProcW;
    description.hInstance = ::GetModuleHandleW(nullptr);
    description.lpszClassName = L"PickerSendProbeCover";
    ::RegisterClassExW(&description);

    const HWND cover = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, description.lpszClassName, L"cover", WS_POPUP,
        point.x - 60, point.y - 60, 120, 120, nullptr, nullptr, description.hInstance, nullptr);
    if (!cover) return;
    ::ShowWindow(cover, SW_SHOWNOACTIVATE);

    // Keep pumping so hit tests against this window are answered rather than
    // stalling whoever asks.
    const ULONGLONG until = ::GetTickCount64() + durationMs;
    for (MSG message; ::GetTickCount64() < until;) {
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::Sleep(5);
    }
    ::DestroyWindow(cover);
}

bool SwitchToSecondTab(HWND target) {
    if (!ActivateWindow(target) ||
        !WaitUntil([target] { return WindowIsActive(target); }, kRaiseTimeoutMs)) {
        return false;
    }
    std::vector<INPUT> events;
    AddModifierReleases(events);
    AddVirtualKey(events, VK_CONTROL);
    AddVirtualKey(events, '2');
    AddVirtualKey(events, '2', true);
    AddVirtualKey(events, VK_CONTROL, true);
    if (!InjectEvents(events)) return false;
    WaitForMessagesProcessed(target);
    ::Sleep(200);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::puts("Usage: picker_send_probe.exe <hwnd> <x> <y> <expected state> [text] [enter] "
                  "[repeat] [cover ms] [hold shift] [switch second tab]");
        return 2;
    }
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;

    const HWND hwnd = reinterpret_cast<HWND>(_wcstoui64(argv[1], nullptr, 10));
    const POINT point{_wtol(argv[2]), _wtol(argv[3])};
    const int expected = _wtoi(argv[4]);
    const std::wstring text = argc > 5 ? argv[5] : L"";
    const bool pressEnter = argc > 6 && _wtoi(argv[6]) != 0;
    const int repeat = argc > 7 ? (std::max)(_wtoi(argv[7]), 1) : 1;
    const DWORD coverMs = argc > 8 ? static_cast<DWORD>((std::max)(_wtoi(argv[8]), 0)) : 0;
    const bool holdShift = argc > 9 && _wtoi(argv[9]) != 0;
    const bool switchSecondTab = argc > 10 && _wtoi(argv[10]) != 0;

    int result = 0;
    {
        WebInputPicker picker;
        const HWND hit = ::WindowFromPoint(point);
        if (!hit || ::GetAncestor(hit, GA_ROOT) != hwnd) {
            std::puts("FAIL: the requested point does not belong to the target window");
            ::CoUninitialize();
            return 1;
        }

        const int state = static_cast<int>(picker.Inspect(point, nullptr, true));
        std::printf("state=%d expected=%d\n", state, expected);
        if (state != expected) {
            std::puts("FAIL: unexpected pick state");
            result = 1;
        }

        const bool pickable = IsPickableState(static_cast<WebInputPickState>(state));
        const bool committed = picker.CommitCandidate();
        std::printf("pickable=%d committed=%d window=%llu\n", pickable, committed,
                    reinterpret_cast<unsigned long long>(picker.SelectedWindow()));
        if (committed != pickable || (committed && picker.SelectedWindow() != hwnd)) {
            std::puts("FAIL: commit did not match the reported state");
            result = 1;
        }
        if (committed && switchSecondTab && !SwitchToSecondTab(hwnd)) {
            std::puts("FAIL: could not switch away from the retained browser tab");
            result = 1;
        }

        // One pick, many sends: a target that only works intermittently shows
        // up here, where a single send would look fine.
        for (int attempt = 1; committed && !text.empty() && attempt <= repeat; ++attempt) {
            std::wstring line = text;
            if (repeat > 1) line += L" " + std::to_wstring(attempt);
            std::thread cover;
            if (coverMs) {
                cover = std::thread(CoverPointBriefly, point, coverMs);
                ::Sleep(60);  // Let the cover be up before the send looks.
            }
            // The Send hotkey is typically a Shift chord. Holding Shift here
            // is the state OnSend sees if the target has not processed the
            // key-up yet: without releasing it, Enter becomes a newline.
            if (holdShift) {
                INPUT down{};
                down.type = INPUT_KEYBOARD;
                down.ki.wVk = VK_SHIFT;
                ::SendInput(1, &down, sizeof(INPUT));
            }
            std::wstring error;
            const ULONGLONG started = ::GetTickCount64();
            const auto outcome = picker.SendText(line, pressEnter, error);
            const bool sent = outcome == WebInputSendResult::Inserted ||
                              outcome == WebInputSendResult::Submitted;
            if (holdShift) {
                INPUT up{};
                up.type = INPUT_KEYBOARD;
                up.ki.wVk = VK_SHIFT;
                up.ki.dwFlags = KEYEVENTF_KEYUP;
                ::SendInput(1, &up, sizeof(INPUT));
            }
            const ULONGLONG elapsed = ::GetTickCount64() - started;
            if (cover.joinable()) cover.join();
            std::printf("send %d/%d: outcome=%d sent=%d took=%llums error='%ls'\n", attempt,
                        repeat, static_cast<int>(outcome), sent, elapsed, error.c_str());
            ReportInputOwner("after send", hwnd);
            if (!sent) result = 1;
        }
    }
    ::CoUninitialize();
    return result;
}
