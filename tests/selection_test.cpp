// Checks CaptionView's text selection against a real pane, with no caption
// source involved: the lines are fed straight into the view and the mouse
// gestures are synthesised.
//
// Build and run with tests\selection_test.bat. It leaves a screenshot of the
// highlighted pane at build\selection_probe.png so the geometry can be eyeballed
// as well as asserted on.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "CaptionView.h"
#include "resource.h"

namespace {

int g_failures = 0;
int g_lastCommand = 0;

void Check(bool condition, const std::string& label) {
    std::printf("%-62s %s\n", label.c_str(), condition ? "PASS" : "FAIL");
    if (!condition) ++g_failures;
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
void Move(HWND hwnd, int x, int y) {
    ::SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
}
void Up(HWND hwnd, int x, int y) {
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}
void DoubleClick(HWND hwnd, int x, int y) {
    Down(hwnd, x, y);
    ::SendMessageW(hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(x, y));
    Up(hwnd, x, y);
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

int main() {
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

    // Visible and topmost so the screenshot at the end captures real pixels.
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
    ::SetWindowPos(view.Handle(), nullptr, 0, 0, kWidth, kHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    view.SetTypography(L"Segoe UI", 14, 1.3);

    const std::vector<std::wstring> lines = {
        L"So we could learn where the system is folding in the real world.",
        L"Monitoring is missing, and the edge cases that exist in real scenarios "
        L"are the ones that wrap onto a second row in this pane.",
        L"All right.",
        L"So I guess that is my question.",
    };
    view.QueueUpdate(0, lines);
    view.Present();
    Pump(60);

    // --- Clear followed by an absolute-index update -----------------------
    // The capture merger retains its absolute transcript position when only
    // the pane is cleared. Revisions of that same line must replace one
    // another, not accumulate as progressive fragments.
    view.Clear();
    view.QueueUpdate(40, {L"Because"});
    view.QueueUpdate(40, {L"Because believing"});
    view.QueueUpdate(40, {L"Because believing that the dots will connect"});
    view.Present();
    CheckText(view.FullText(), L"Because believing that the dots will connect\r\n",
              "post-clear revisions replace the same absolute line");

    // Restore the standard fixture for the interaction checks below.
    view.Clear();
    view.QueueUpdate(0, lines);
    view.Present();

    // Geometry: the line-select gutter is 75 device pixels at 96 dpi, and this
    // process is dpi-unaware, so a point just past it lands on the first glyph.
    constexpr int kGutterX = 10;
    constexpr int kFirstGlyphX = 76;
    constexpr int kFirstGlyphY = 14;
    constexpr int kPastEndY = 20000;
    // Rough Y of later rows at 14pt / 1.3 spacing. Line 1 wraps onto a second
    // row, so "All right" sits near y≈110 and the last line near y≈140.
    constexpr int kLine0Y = 20;
    constexpr int kLine2Y = 115;
    constexpr int kLine3Y = 145;

    // --- Dragging ----------------------------------------------------------
    Down(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Move(view.Handle(), 4000, kPastEndY);
    Up(view.Handle(), 4000, kPastEndY);
    CheckText(view.SelectedText(), Joined(lines), "drag past the last line selects everything");

    view.ClearSelection();
    Check(!view.HasSelection() && view.SelectedText().empty(), "clearing drops the selection");

    view.SelectAll();
    CheckText(view.SelectedText(), Joined(lines), "select all returns the whole transcript");

    // A drag that ends where it started is a plain click, and selects nothing.
    Down(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Up(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Check(!view.HasSelection(), "a plain click collapses the selection");

    // --- Words -------------------------------------------------------------
    DoubleClick(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    CheckText(view.SelectedText(), L"So", "double-click on a word selects that word");

    // Jitter between the double-click and the button coming up must not
    // collapse the word back to a caret.
    DoubleClick(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Move(view.Handle(), kFirstGlyphX + 1, kFirstGlyphY);
    Up(view.Handle(), kFirstGlyphX + 1, kFirstGlyphY);
    CheckText(view.SelectedText(), L"So", "a twitch after double-click keeps the word");

    g_lastCommand = 0;
    DoubleClick(view.Handle(), 400, kPastEndY);
    Check(g_lastCommand == IDC_BTN_SEND, "double-click below the text still sends");

    // --- Shift-click -------------------------------------------------------
    view.ClearSelection();
    Down(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Up(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Down(view.Handle(), 4000, kPastEndY, MK_SHIFT);
    Up(view.Handle(), 4000, kPastEndY);
    CheckText(view.SelectedText(), Joined(lines), "shift-click extends from the last click");

    // --- Live updates: clamp ------------------------------------------------
    view.SelectAll();
    view.QueueUpdate(lines.size() - 1, {L"short"});
    view.Present();
    std::vector<std::wstring> revised = lines;
    revised.back() = L"short";
    CheckText(view.SelectedText(), Joined(revised),
              "selection survives the last line being revised");

    // --- Sticky end: grows with captions -----------------------------------
    view.ClearSelection();
    Down(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Move(view.Handle(), 4000, kPastEndY);
    Up(view.Handle(), 4000, kPastEndY);
    CheckText(view.SelectedText(), Joined(revised), "sticky setup reaches the transcript end");

    revised.back() = L"short and then longer again";
    view.QueueUpdate(revised.size() - 1, {revised.back()});
    view.Present();
    CheckText(view.SelectedText(), Joined(revised),
              "selection at end grows when the last line is rewritten");

    revised.push_back(L"a brand new line");
    view.QueueUpdate(revised.size() - 1, {revised.back()});
    view.Present();
    CheckText(view.SelectedText(), Joined(revised),
              "selection at end grows when a new line is appended");

    // A selection that stops short must not be pulled along.
    view.ClearSelection();
    DoubleClick(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    const std::wstring frozen = view.SelectedText();
    view.QueueUpdate(revised.size(), {L"should not join the selection"});
    view.Present();
    CheckText(view.SelectedText(), frozen, "selection short of the end stays put");

    // Shrinking the pane used to jump to the bottom whenever stick-to-bottom was
    // set (including when the transcript still fitted on screen), which scrolled
    // a mid-pane highlight out of sight and looked like the selection was gone.
    RECT before{};
    ::GetClientRect(view.Handle(), &before);
    const int newHeight = std::max((before.bottom - before.top) / 2, 80L);
    ::SetWindowPos(view.Handle(), nullptr, 0, 0, before.right - before.left, newHeight,
                   SWP_NOMOVE | SWP_NOZORDER);
    Pump(40);
    CheckText(view.SelectedText(), frozen, "selection survives a caption pane height change");

    // --- Gutter line select ------------------------------------------------
    view.QueueUpdate(0, lines);
    view.Present();
    Pump(40);

    Down(view.Handle(), kGutterX, kLine2Y);
    Up(view.Handle(), kGutterX, kLine2Y);
    const std::wstring tailFromLine2 = lines[2] + L"\r\n" + lines[3];
    CheckText(view.SelectedText(), tailFromLine2,
              "gutter click selects from the clicked line through the transcript end");

    const std::wstring appendedCaption = L"A new caption after the gutter click.";
    view.QueueUpdate(lines.size(), {appendedCaption});
    view.Present();
    CheckText(view.SelectedText(), tailFromLine2 + L"\r\n" + appendedCaption,
              "gutter tail selection grows with new caption text");

    view.QueueUpdate(0, lines);
    view.Present();

    Down(view.Handle(), kGutterX, kLine0Y);
    Move(view.Handle(), kGutterX, kLine2Y);
    Up(view.Handle(), kGutterX, kLine2Y);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "gutter drag down takes whole lines");

    Down(view.Handle(), kGutterX, kLine2Y);
    Move(view.Handle(), kGutterX, kLine0Y);
    Up(view.Handle(), kGutterX, kLine0Y);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "gutter drag up flips the anchor so the origin row stays whole");

    view.ClearSelection();
    Down(view.Handle(), kGutterX, kLine0Y);
    Up(view.Handle(), kGutterX, kLine0Y);
    Down(view.Handle(), kGutterX, kLine3Y, MK_SHIFT);
    Up(view.Handle(), kGutterX, kLine3Y);
    CheckText(view.SelectedText(), Joined(lines), "shift-click in the gutter extends by lines");

    // Gutter drag stays line-granular after the pointer wanders over the text.
    Down(view.Handle(), kGutterX, kLine0Y);
    Move(view.Handle(), kFirstGlyphX + 80, kLine2Y);
    Up(view.Handle(), kFirstGlyphX + 80, kLine2Y);
    CheckText(view.SelectedText(), lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "gutter drag stays line-granular over the text");

    // Text drag never turns into a line selection just by crossing the gutter.
    Down(view.Handle(), kFirstGlyphX, kFirstGlyphY);
    Move(view.Handle(), kGutterX, kLine2Y);
    Up(view.Handle(), kGutterX, kLine2Y);
    {
        const std::wstring selected = view.SelectedText();
        Check(view.HasSelection() && selected.find(L"So we could learn") == 0 &&
                  selected != lines[0] + L"\r\n" + lines[1] + L"\r\n" + lines[2] + L"\r\n",
              "text drag through the gutter stays character-granular");
    }

    g_lastCommand = 0;
    DoubleClick(view.Handle(), kGutterX, kLine0Y);
    Check(g_lastCommand != IDC_BTN_SEND && view.SelectedText() == Joined(lines),
          "double-click in the gutter selects the transcript tail rather than sending");

    // --- Trimming ----------------------------------------------------------
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
        Check(selected.find(L"one more line") != std::wstring::npos,
              "select-all at the end grows across the trim append");
    }

    // --- Keyboard ----------------------------------------------------------
    view.QueueUpdate(0, lines);
    view.Present();

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
        ::SendMessageW(view.Handle(), WM_KEYDOWN, 'A', 0);
        CheckText(view.SelectedText(), Joined(lines), "Ctrl+A selects the whole transcript");

        ::SendMessageW(view.Handle(), WM_KEYDOWN, 'C', 0);
        Check(g_lastCommand == IDC_BTN_COPY, "Ctrl+C asks the parent to copy");

        keyboard[VK_CONTROL] &= static_cast<BYTE>(~0x80);
        ::SetKeyboardState(keyboard);

        // Unmodified, the same keys must fall through to scrolling.
        g_lastCommand = 0;
        ::SendMessageW(view.Handle(), WM_KEYDOWN, 'C', 0);
        Check(g_lastCommand == 0 && view.HasSelection(), "a bare C neither copies nor deselects");

        ::SendMessageW(view.Handle(), WM_KEYDOWN, VK_ESCAPE, 0);
        Check(!view.HasSelection(), "Escape clears the selection");
    }

    // --- Screenshot --------------------------------------------------------
    // A partial selection running from inside line 1 into line 3, which is the
    // interesting case for the highlight: a wrapped row, a full row, and the
    // stub that stands in for each line break.
    view.ClearSelection();
    Down(view.Handle(), 200, 45);
    Move(view.Handle(), 300, 120);
    Up(view.Handle(), 300, 120);
    ::InvalidateRect(view.Handle(), nullptr, FALSE);
    ::UpdateWindow(view.Handle());
    Pump(150);

    if (SaveScreenshot(parent, L"build\\selection_probe.bmp")) {
        std::printf("\nScreenshot written to build\\selection_probe.bmp\n");
    } else {
        std::printf("\nCould not capture the screenshot.\n");
    }
    std::printf("Selection under the drag: [%s]\n", Narrow(view.SelectedText()).c_str());

    ::DestroyWindow(parent);
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "All checks passed" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
