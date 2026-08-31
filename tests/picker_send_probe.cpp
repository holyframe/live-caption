// Picks a target at an explicit screen point and types into it, so the send
// path can be verified against a real browser. Intended for the local test
// fixture only: it injects real keystrokes into whatever window is picked.
#include "../src/WebInputPicker.cpp"

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::puts("Usage: picker_send_probe.exe <hwnd> <x> <y> <expected state> [text] [enter]");
        return 2;
    }
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;

    const HWND hwnd = reinterpret_cast<HWND>(_wcstoui64(argv[1], nullptr, 10));
    const POINT point{_wtol(argv[2]), _wtol(argv[3])};
    const int expected = _wtoi(argv[4]);
    const std::wstring text = argc > 5 ? argv[5] : L"";
    const bool pressEnter = argc > 6 && _wtoi(argv[6]) != 0;

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
        std::printf("pickable=%d committed=%d clicksPoint=%d window=%llu\n", pickable, committed,
                    picker.SelectedClicksPoint(),
                    reinterpret_cast<unsigned long long>(picker.SelectedWindow()));
        if (committed != pickable || (committed && picker.SelectedWindow() != hwnd)) {
            std::puts("FAIL: commit did not match the reported state");
            result = 1;
        }

        if (committed && !text.empty()) {
            std::wstring error;
            const bool sent = picker.SendText(text, pressEnter, error);
            std::printf("sent=%d error='%ls'\n", sent, error.c_str());
            if (!sent) result = 1;
        }
    }
    ::CoUninitialize();
    return result;
}
