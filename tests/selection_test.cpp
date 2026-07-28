// Checks CaptionView's text selection against a real pane, with no caption
// source involved: the lines are fed straight into the view and the mouse
// gestures are synthesised.
//
// Build and run with tests\selection_test.bat on Windows, or
// tests/selection_test_wine.sh to cross-build and run it under wine. It leaves
// screenshots of the pane in build\ so the geometry can be eyeballed as well as
// asserted on.
//
// Pass --no-text-hittest to skip the checks that need
// IDWriteTextLayout::HitTestPoint, which wine still stubs out. Everything that
// only needs line metrics — the gutter, the tail-following selection, clamping
// across live updates, the keyboard — runs either way.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CaptionView.h"
#include "resource.h"

namespace {

int g_failures = 0;
int g_skipped = 0;
int g_lastCommand = 0;
bool g_textHitTesting = true;

// Pane geometry the tests need to know: the left padding doubles as the
// line-select gutter, and the transcript starts just below the top padding.
constexpr int kGutterX = 6;       // inside the gutter
constexpr int kTextX = 15;        // just past it, on the first glyph
constexpr int kPastEndY = 20000;  // far below the last line

constexpr size_t kNoLine = static_cast<size_t>(-1);

void Check(bool condition, const std::string& label) {
    std::printf("%-62s %s\n", label.c_str(), condition ? "PASS" : "FAIL");
    if (!condition) ++g_failures;
    std::fflush(stdout);
}

void Skip(const std::string& label) {
    std::printf("%-62s SKIP\n", label.c_str());
    ++g_skipped;
    std::fflush(stdout);
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t ch : text) {
        if (ch == L'\r') { out += "\\r"; continue; }
        if (ch == L'\n') { out += "\\n"; continue; }
        out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return out;
}

void CheckText(const std::wstring& actual, const std::wstring& expected, const std::string& label) {
    Check(actual == expected, label);
    if (actual != expected) {
        std::printf("    expected [%s]\n    actual   [%s]\n", Narrow(expected).c_str(),
                    Narrow(actual).c_str());
        std::fflush(stdout);
    }
}

LRESULT CALLBACK ParentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND) {
        g_lastCommand = LOWORD(wParam);
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// The transcript joined the way a selection over all of it should come back.
std::wstring Joined(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) out += L"\r\n";
        out += lines[i];
    }
    return out;
}

void Down(HWND hwnd, int x, int y, WPARAM flags = 0) {
    ::SendMessageW(hwnd, WM_LBUTTONDOWN, flags, MAKELPARAM(x, y));
}
void Move(HWND hwnd, int x, int y, WPARAM flags = MK_LBUTTON) {
    ::SendMessageW(hwnd, WM_MOUSEMOVE, flags, MAKELPARAM(x, y));
}
void Up(HWND hwnd, int x, int y) {
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}
void Drag(HWND hwnd, int x1, int y1, int x2, int y2, WPARAM downFlags = 0) {
    Down(hwnd, x1, y1, downFlags);
    Move(hwnd, x2, y2);
    Up(hwnd, x2, y2);
}
void DoubleClick(HWND hwnd, int x, int y) {
    Down(hwnd, x, y);
    ::SendMessageW(hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(x, y));
    Up(hwnd, x, y);
}

// Which line a gutter click at this y takes, or kNoLine when the click lands
// outside the transcript. Used to find the rows without depending on font
// metrics, or on the text hit-testing that wine lacks.
size_t GutterLineAtY(CaptionView& view, const std::vector<std::wstring>& lines, int y) {
    view.ClearSelection();
    Drag(view.Handle(), kGutterX, y, kGutterX, y);
    const std::wstring selected = view.SelectedText();
    if (selected.empty()) return kNoLine;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (selected == lines[i] + L"\r\n" || selected == lines[i]) return i;
    }
    return kNoLine;
}

// A y comfortably inside each line.
std::vector<int> MeasureLineRows(CaptionView& view, const std::vector<std::wstring>& lines,
                                 int paneHeight) {
    std::vector<int> top(lines.size(), -1);
    for (int y = 0; y < paneHeight; ++y) {
        const size_t line = GutterLineAtY(view, lines, y);
        if (line != kNoLine && top[line] < 0) top[line] = y;
    }
    for (int& y : top) {
        if (y >= 0) y += 4;  // a few pixels in, so the point is inside the row
    }
    view.ClearSelection();
    return top;
}

void Pump(int milliseconds) {
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    MSG msg;
    while (::GetTickCount64() < deadline) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(5);
    }
}

// Screen capture rather than a window DC: an ID2D1HwndRenderTarget presents
// through DirectX, so its pixels are not in the window's GDI surface.
bool SaveScreenshot(HWND hwnd, const wchar_t* path) {
    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc)) return false;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return false;

    const HDC screen = ::GetDC(nullptr);
    const HDC mem = ::CreateCompatibleDC(screen);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down while we blit
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HBITMAP dib = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        const HGDIOBJ previous = ::SelectObject(mem, dib);
        if (::BitBlt(mem, 0, 0, width, height, screen, rc.left, rc.top, SRCCOPY)) {
            const DWORD stride = static_cast<DWORD>(width) * 4;
            const DWORD imageBytes = stride * static_cast<DWORD>(height);

            BITMAPFILEHEADER file{};
            file.bfType = 0x4D42;  // "BM"
            file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            file.bfSize = file.bfOffBits + imageBytes;

            BITMAPINFOHEADER header = info.bmiHeader;
            header.biHeight = height;  // rows are written bottom-up below
            header.biSizeImage = imageBytes;

            FILE* out = nullptr;
            if (::_wfopen_s(&out, path, L"wb") == 0 && out) {
                std::fwrite(&file, sizeof(file), 1, out);
                std::fwrite(&header, sizeof(header), 1, out);
                const auto* pixels = static_cast<const unsigned char*>(bits);
                for (int row = height - 1; row >= 0; --row) {
                    std::fwrite(pixels + static_cast<size_t>(row) * stride, stride, 1, out);
                }
                std::fclose(out);
                ok = true;
            }
        }
        ::SelectObject(mem, previous);
    }

    if (dib) ::DeleteObject(dib);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-text-hittest") == 0) g_textHitTesting = false;
    }

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ParentProc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"SelectionProbeHost";
    ::RegisterClassExW(&wc);

    constexpr int kWidth = 760;
    constexpr int kHeight = 300;

    // Visible and topmost so the screenshots capture real pixels.
    const HWND parent = ::CreateWindowExW(WS_EX_TOPMOST, L"SelectionProbeHost", L"Selection probe",
                                          WS_POPUP | WS_VISIBLE | WS_BORDER, 120, 120, kWidth,
                                          kHeight, nullptr, nullptr, instance, nullptr);
    if (!parent) {
        std::printf("Could not create the host window.\n");
        return 1;
    }

    CaptionView view;
    if (!CaptionView::RegisterWindowClass(instance) ||
        !view.Create(parent, instance, IDC_CAPTION_VIEW)) {
        std::printf("Could not create the caption pane.\n");
        return 1;
    }
    const HWND pane = view.Handle();
    ::SetWindowPos(pane, nullptr, 0, 0, kWidth, kHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    view.SetTypography(L"Segoe UI", 14, 1.3);

    const std::vector<std::wstring> lines = {
        L"So we could learn where the system is folding in the real world.",
        L"Monitoring is missing, and the edge cases that exist in real scenarios "
        L"are the ones that wrap onto a second row in this pane.",
        L"All right.",
        L"So I guess that is my question.",
    };
    const auto reset = [&] {
        view.ClearSelection();
        view.QueueUpdate(0, lines);
        view.Present();
    };
    reset();
    Pump(60);

    const std::vector<int> row = MeasureLineRows(view, lines, kHeight);
    std::printf("Line rows:");
    for (const int y : row) std::printf(" %d", y);
    std::printf("%s\n\n", g_textHitTesting ? "" : "   (text hit-testing disabled)");
    for (size_t i = 0; i < row.size(); ++i) {
        if (row[i] < 0) {
            std::printf("Could not locate line %zu; the pane is too small.\n", i);
            return 1;
        }
    }

    // --- The gutter ---------------------------------------------------------
    // Clicking the left padding takes the whole line, line break and all, the
    // way clicking a line number does in an editor.
    reset();
    Check(GutterLineAtY(view, lines, 0) == kNoLine, "the padding above the text selects nothing");

    Drag(pane, kGutterX, row[0], kGutterX, row[0]);
    CheckText(view.SelectedText(), lines[0] + L"\r\n", "a gutter click takes the whole line");

    Drag(pane, kGutterX, row[2], kGutterX, row[2]);
    CheckText(view.SelectedText(), lines[2] + L"\r\n", "a gutter click takes the line it is on");

    Drag(pane, kGutterX, row[3], kGutterX, row[3]);
    CheckText(view.SelectedText(), lines[3], "the last line has no break to take");

    Drag(pane, kGutterX, row[0], kGutterX, row[2]);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "dragging down the gutter takes whole lines");

    Drag(pane, kGutterX, row[2], kGutterX, row[0]);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "dragging up the gutter keeps the started-on line whole");

    // A gutter drag stays line-granular even when the pointer wanders over the
    // text, as it does in VS Code.
    Drag(pane, kGutterX, row[0], 300, row[1]);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n",
              "a gutter drag stays line-granular over the text");

    view.ClearSelection();
    Drag(pane, kGutterX, row[1], kGutterX, row[1]);
    Drag(pane, kGutterX, row[3], kGutterX, row[3], MK_SHIFT);
    CheckText(view.SelectedText(), lines[1] + L"\r\n" + lines[2] + L"\r\n" + lines[3],
              "shift-clicking the gutter extends by whole lines");

    // Below the last line the gutter is just empty pane space, so the
    // double-click-to-send gesture still works there.
    view.ClearSelection();
    g_lastCommand = 0;
    DoubleClick(pane, kGutterX, kPastEndY);
    Check(g_lastCommand == IDC_BTN_SEND && !view.HasSelection(),
          "the gutter below the transcript is still empty space");

    // --- A selection that reaches the tail follows it -----------------------
    reset();
    view.SelectAll();
    Check(view.SelectionFollowsTail(), "a selection taken to the end follows the tail");
    {
        std::vector<std::wstring> grown = lines;

        // A new caption line arrives.
        grown.push_back(L"And this sentence arrived afterwards.");
        view.QueueUpdate(lines.size(), {grown.back()});
        view.Present();
        CheckText(view.SelectedText(), Joined(grown), "a new caption line joins a tail selection");

        // The recogniser revises the last line in place, as it does constantly.
        grown.back() = L"And this sentence arrived afterwards, revised.";
        view.QueueUpdate(grown.size() - 1, {grown.back()});
        view.Present();
        CheckText(view.SelectedText(), Joined(grown), "a revision of the tail line stays selected");
        Check(view.SelectionFollowsTail(), "the selection still follows the tail");
    }

    // The same, for a selection that starts partway in rather than at the top.
    reset();
    Drag(pane, kGutterX, row[2], kGutterX, row[3]);
    {
        const std::wstring before = view.SelectedText();
        Check(view.SelectionFollowsTail(), "a gutter selection of the last line follows the tail");
        view.QueueUpdate(lines.size(), {L"Newly recognised words."});
        view.Present();
        CheckText(view.SelectedText(), before + L"\r\nNewly recognised words.",
                  "the tail keeps growing from a gutter selection");
    }

    // A selection that stops short of the tail must stay exactly where it is.
    reset();
    Drag(pane, kGutterX, row[0], kGutterX, row[0]);
    {
        const std::wstring before = view.SelectedText();
        Check(!view.SelectionFollowsTail(), "a selection short of the end does not follow");
        view.QueueUpdate(lines.size(), {L"Another sentence entirely."});
        view.Present();
        CheckText(view.SelectedText(), before, "new text leaves an earlier selection alone");
    }

    // --- Live updates and trimming ------------------------------------------
    reset();
    view.SelectAll();
    view.QueueUpdate(lines.size() - 1, {L"short"});
    view.Present();
    {
        std::vector<std::wstring> revised = lines;
        revised.back() = L"short";
        CheckText(view.SelectedText(), Joined(revised),
                  "selection survives the last line being revised");
    }

    // The pane keeps a bounded number of lines; a selection anchored at the top
    // has to follow the window rather than point at a line that is gone.
    {
        std::vector<std::wstring> many;
        for (int i = 0; i < 3000; ++i) {
            wchar_t text[32];
            ::swprintf_s(text, L"line %04d", i);
            many.emplace_back(text);
        }
        view.QueueUpdate(0, many);
        view.SelectAll();
        view.QueueUpdate(many.size(), {L"one more line"});
        view.Present();

        const std::wstring selected = view.SelectedText();
        const size_t firstBreak = selected.find(L"\r\n");
        const std::wstring firstLine =
            firstBreak == std::wstring::npos ? selected : selected.substr(0, firstBreak);
        Check(view.HasSelection(), "selection survives lines being trimmed away");
        CheckText(firstLine, L"line 0001", "a trimmed-away anchor moves to the oldest kept line");
    }

    // --- Selecting through the text -----------------------------------------
    reset();
    if (g_textHitTesting) {
        Drag(pane, 0, 0, 4000, kPastEndY);
        CheckText(view.SelectedText(), Joined(lines), "drag past the last line selects everything");

        DoubleClick(pane, kTextX, row[0]);
        CheckText(view.SelectedText(), L"So", "double-click on a word selects that word");

        // Jitter between the double-click and the button coming up must not
        // collapse the word back to a caret.
        DoubleClick(pane, kTextX, row[0]);
        Move(pane, kTextX + 1, row[0]);
        Up(pane, kTextX + 1, row[0]);
        CheckText(view.SelectedText(), L"So", "a twitch after double-click keeps the word");

        view.ClearSelection();
        Drag(pane, kTextX, row[0], kTextX, row[0]);
        Drag(pane, 4000, kPastEndY, 4000, kPastEndY, MK_SHIFT);
        CheckText(view.SelectedText(), Joined(lines), "shift-click extends from the last click");

        // A text drag never turns into a line selection just by passing through
        // the gutter.
        Drag(pane, kTextX, row[0], kGutterX, row[0]);
        Check(view.SelectedText() != lines[0] + L"\r\n", "a text drag does not take whole lines");
    } else {
        Skip("drag past the last line selects everything");
        Skip("double-click on a word selects that word");
        Skip("a twitch after double-click keeps the word");
        Skip("shift-click extends from the last click");
        Skip("a text drag does not take whole lines");
    }

    view.ClearSelection();
    Drag(pane, kTextX, row[0], kTextX, row[0]);
    Check(!view.HasSelection(), "a plain click collapses the selection");

    g_lastCommand = 0;
    DoubleClick(pane, 400, kPastEndY);
    Check(g_lastCommand == IDC_BTN_SEND, "double-click below the text still sends");

    // --- Keyboard -----------------------------------------------------------
    reset();

    // GetKeyState reads the calling thread's own copy of the keyboard state, and
    // SetKeyboardState writes it, so the modifier can be held down here without
    // injecting anything into the rest of the desktop.
    {
        BYTE keyboard[256]{};
        ::GetKeyboardState(keyboard);
        keyboard[VK_CONTROL] |= 0x80;
        ::SetKeyboardState(keyboard);

        view.ClearSelection();
        g_lastCommand = 0;
        ::SendMessageW(pane, WM_KEYDOWN, 'A', 0);
        CheckText(view.SelectedText(), Joined(lines), "Ctrl+A selects the whole transcript");

        ::SendMessageW(pane, WM_KEYDOWN, 'C', 0);
        Check(g_lastCommand == IDC_BTN_COPY, "Ctrl+C asks the parent to copy");

        keyboard[VK_CONTROL] &= static_cast<BYTE>(~0x80);
        ::SetKeyboardState(keyboard);

        g_lastCommand = 0;
        ::SendMessageW(pane, WM_KEYDOWN, 'C', 0);
        Check(g_lastCommand == 0 && view.HasSelection(), "a bare C neither copies nor deselects");

        ::SendMessageW(pane, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(!view.HasSelection(), "Escape clears the selection");
    }

    // --- Screenshots --------------------------------------------------------
    // Paint synchronously, then wait without pumping: the present has to reach
    // the screen before the capture, but dispatching messages here would
    // deliver the WM_MOUSELEAVE that ends the gutter hover.
    const auto capture = [&](const wchar_t* path) {
        ::InvalidateRect(pane, nullptr, FALSE);
        ::UpdateWindow(pane);
        ::Sleep(250);
        if (!SaveScreenshot(parent, path)) std::printf("Could not capture %ls\n", path);
    };

    reset();
    Pump(120);
    if (g_textHitTesting) {
        // A partial text selection running from inside line 1 into line 3: a
        // wrapped row, a full row, and the stub that stands in for each break.
        Drag(pane, 200, row[1], 300, row[2]);
        capture(L"build\\selection_text.bmp");
        reset();
        Pump(120);
    }

    // Two whole lines taken from the gutter, with the gutter itself lit under
    // the pointer on a third.
    Drag(pane, kGutterX, row[1], kGutterX, row[2]);
    Move(pane, kGutterX, row[3], 0);  // hover, no button held
    capture(L"build\\selection_gutter.bmp");

    ::DestroyWindow(parent);
    std::printf("\n%s (%d failure%s, %d skipped)\n",
                g_failures == 0 ? "All checks passed" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s", g_skipped);
    return g_failures == 0 ? 0 : 1;
}
