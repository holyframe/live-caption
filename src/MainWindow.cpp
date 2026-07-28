#include "MainWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <set>

#include "Util.h"
#include "resource.h"

namespace {

constexpr const wchar_t* kClassName = L"LiveCaptionViewMain";
constexpr const wchar_t* kBaseTitle = L"Live Caption App";

// Unscaled (96 dpi) toolbar geometry.
constexpr int kBarHeight    = 58;
constexpr int kPad          = 8;
constexpr int kRowHeight    = 23;
constexpr int kGap          = 6;
constexpr int kSendWidth    = 92;

// Throttle for "copy real-time"; the caption updates ~10x per second and we do
// not want to thrash the clipboard that hard.
constexpr ULONGLONG kRealtimeCopyIntervalMs = 600;

int CALLBACK EnumFontProc(const LOGFONTW* logFont, const TEXTMETRICW*, DWORD, LPARAM param) {
    auto* names = reinterpret_cast<std::set<std::wstring>*>(param);
    if (logFont->lfFaceName[0] != L'@') names->insert(logFont->lfFaceName);
    return 1;
}

}  // namespace

bool MainWindow::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kClassName;
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    return ::RegisterClassExW(&wc) != 0;
}

int MainWindow::Scaled(int value) const {
    return ::MulDiv(value, static_cast<int>(m_dpi), 96);
}

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
    m_instance = instance;
    m_settings.Load();

    const int width = std::max(m_settings.windowW, 480);
    const int height = std::max(m_settings.windowH, 320);
    const int x = m_settings.windowX >= 0 ? m_settings.windowX : CW_USEDEFAULT;
    const int y = m_settings.windowY >= 0 ? m_settings.windowY : CW_USEDEFAULT;

    m_hwnd = ::CreateWindowExW(0, kClassName, kBaseTitle, WS_OVERLAPPEDWINDOW, x, y, width, height,
                               nullptr, nullptr, instance, this);
    if (!m_hwnd) return false;

    ::ShowWindow(m_hwnd, showCommand);
    ::UpdateWindow(m_hwnd);
    return true;
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<MainWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = hwnd;
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, message, wParam, lParam);
    return self->WndProc(message, wParam, lParam);
}

LRESULT MainWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            m_dpi = ::GetDpiForWindow(m_hwnd);
            if (m_dpi == 0) m_dpi = 96;

            // Match the dark title bar in the reference design. Ignored on
            // builds that predate the attribute.
            const BOOL darkTitleBar = TRUE;
            ::DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitleBar,
                                    sizeof(darkTitleBar));

            if (!CreateChildren()) return -1;
            UpdateHotkeyRegistration();
            Layout();
            ApplyTypography();
            ApplyViewMode();

            m_engine.Start(m_hwnd, m_settings.ResolvedTranscriptPath(), m_settings.pollIntervalMs);
            SetStatus(std::wstring(L"Saving transcript to ") + m_settings.ResolvedTranscriptPath());
            return 0;
        }

        case WM_SIZE:
            Layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = Scaled(660);
            mmi->ptMinTrackSize.y = Scaled(260);
            return 0;
        }

        case WM_DPICHANGED: {
            m_dpi = HIWORD(wParam);
            if (m_dpi == 0) m_dpi = 96;
            ApplyControlFont();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left, suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }

        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;

        case WM_MOUSEWHEEL:
            // Forward wheel input to the pane even when it does not have focus.
            if (m_view.Handle()) {
                return ::SendMessageW(m_view.Handle(), WM_MOUSEWHEEL, wParam, lParam);
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_RETURN && m_settings.pressEnter) {
                OnSend();
                return 0;
            }
            break;

        case WM_HOTKEY:
            if (wParam == HOTKEY_TOGGLE_VIEW) {
                ToggleVisibility();
                return 0;
            }
            break;

        case WM_APP_CAPTION_UPDATE: {
            auto* update = reinterpret_cast<CaptionUpdate*>(lParam);
            OnCaptionUpdate(update);
            delete update;
            return 0;
        }

        case WM_APP_STATUS: {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                SetStatus(*text);
                delete text;
            }
            return 0;
        }

        case WM_APP_SOURCE_CHANGED:
            m_sourceWindow = reinterpret_cast<HWND>(lParam);
            return 0;

        case WM_SETFOCUS:
            if (m_view.Handle()) ::SetFocus(m_view.Handle());
            return 0;

        case WM_CLOSE: {
            // Stop producing before tearing down so no posted payload leaks.
            m_engine.Stop();
            DrainPendingPayloads();

            RECT rc{};
            if (::GetWindowRect(m_hwnd, &rc) && !::IsIconic(m_hwnd) && !::IsZoomed(m_hwnd)) {
                m_settings.windowX = rc.left;
                m_settings.windowY = rc.top;
                m_settings.windowW = rc.right - rc.left;
                m_settings.windowH = rc.bottom - rc.top;
            }
            m_settings.Save();
            ::DestroyWindow(m_hwnd);
            return 0;
        }

        case WM_DESTROY:
            if (m_hotkeyRegistered) {
                ::UnregisterHotKey(m_hwnd, HOTKEY_TOGGLE_VIEW);
                m_hotkeyRegistered = false;
            }
            if (m_controlFont) {
                ::DeleteObject(m_controlFont);
                m_controlFont = nullptr;
            }
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(m_hwnd, message, wParam, lParam);
}

bool MainWindow::CreateChildren() {
    if (!CaptionView::RegisterWindowClass(m_instance)) return false;
    if (!m_view.Create(m_hwnd, m_instance, IDC_CAPTION_VIEW)) return false;

    const auto button = [&](const wchar_t* text, int id) {
        return ::CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 0, 0, 10, 10, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };
    const auto check = [&](const wchar_t* text, int id) {
        return ::CreateWindowExW(0, WC_BUTTONW, text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                 m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 m_instance, nullptr);
    };
    const auto combo = [&](int id) {
        return ::CreateWindowExW(0, WC_COMBOBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 10, 200, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };

    m_sendButton        = button(L"Send", IDC_BTN_SEND);
    m_pressEnterCheck   = check(L"Press Enter", IDC_CHK_PRESS_ENTER);
    m_viewModeButton    = button(L"Normal View", IDC_BTN_VIEW_MODE);
    m_frontAllButton    = button(L"Front all", IDC_BTN_FRONT_ALL);
    m_minimizeAllButton = button(L"Minimize all", IDC_BTN_MINIMIZE_ALL);
    m_copyButton        = button(L"Copy", IDC_BTN_COPY);
    m_copyLiveCheck     = check(L"Copy real-time if this window is active", IDC_CHK_COPY_LIVE);

    m_hintLabel = ::CreateWindowExW(0, WC_STATICW, L"Double-click empty area to send",
                                    WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, 0, 10, 10, m_hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LBL_HINT)),
                                    m_instance, nullptr);

    m_fontCombo    = combo(IDC_CBO_FONT);
    m_sizeCombo    = combo(IDC_CBO_SIZE);
    m_spacingCombo = combo(IDC_CBO_SPACING);

    m_statusBar = ::CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                    0, 0, 10, 10, m_hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUSBAR)),
                                    m_instance, nullptr);

    if (!m_sendButton || !m_fontCombo || !m_statusBar) return false;

    ::SendMessageW(m_pressEnterCheck, BM_SETCHECK, m_settings.pressEnter ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_copyLiveCheck, BM_SETCHECK, m_settings.copyRealtime ? BST_CHECKED : BST_UNCHECKED, 0);

    PopulateFontCombo();
    PopulateSizeCombo();
    PopulateSpacingCombo();
    ApplyControlFont();
    return true;
}

void MainWindow::ApplyControlFont() {
    if (m_controlFont) {
        ::DeleteObject(m_controlFont);
        m_controlFont = nullptr;
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, m_dpi)) {
        m_controlFont = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!m_controlFont) {
        m_controlFont = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    const HWND controls[] = {m_sendButton,     m_pressEnterCheck,   m_hintLabel,
                             m_viewModeButton, m_fontCombo,         m_sizeCombo,
                             m_spacingCombo,   m_copyLiveCheck,     m_frontAllButton,
                             m_minimizeAllButton, m_copyButton,     m_statusBar};
    for (HWND control : controls) {
        if (control) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(m_controlFont), TRUE);
    }
}

void MainWindow::PopulateFontCombo() {
    std::set<std::wstring> families;
    const HDC dc = ::GetDC(m_hwnd);
    LOGFONTW request{};
    request.lfCharSet = DEFAULT_CHARSET;
    ::EnumFontFamiliesExW(dc, &request, &EnumFontProc, reinterpret_cast<LPARAM>(&families), 0);
    ::ReleaseDC(m_hwnd, dc);

    for (const std::wstring& family : families) {
        ::SendMessageW(m_fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(family.c_str()));
    }

    int index = static_cast<int>(::SendMessageW(m_fontCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                               reinterpret_cast<LPARAM>(m_settings.fontFamily.c_str())));
    if (index == CB_ERR) {
        index = static_cast<int>(::SendMessageW(m_fontCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                               reinterpret_cast<LPARAM>(L"Segoe UI")));
    }
    ::SendMessageW(m_fontCombo, CB_SETCURSEL, static_cast<WPARAM>(index == CB_ERR ? 0 : index), 0);
}

void MainWindow::PopulateSizeCombo() {
    static constexpr int kSizes[] = {8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28, 32, 36, 42, 48};
    for (int size : kSizes) {
        const std::wstring text = std::to_wstring(size);
        ::SendMessageW(m_sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    const std::wstring current = std::to_wstring(m_settings.fontSizePt);
    int index = static_cast<int>(::SendMessageW(m_sizeCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                               reinterpret_cast<LPARAM>(current.c_str())));
    if (index == CB_ERR) index = 4;  // 12 pt
    ::SendMessageW(m_sizeCombo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

void MainWindow::PopulateSpacingCombo() {
    static constexpr double kSpacings[] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.75, 2.0, 2.5, 3.0};
    int selected = 3;
    for (int i = 0; i < static_cast<int>(std::size(kSpacings)); ++i) {
        wchar_t text[16];
        ::swprintf_s(text, L"%.2g", kSpacings[i]);
        ::SendMessageW(m_spacingCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        if (std::abs(kSpacings[i] - m_settings.lineSpacing) < 0.001) selected = i;
    }
    ::SendMessageW(m_spacingCombo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

void MainWindow::Layout() {
    if (!m_hwnd || !m_statusBar) return;

    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;

    // Status bar sizes itself; ask it how tall it ended up.
    ::SendMessageW(m_statusBar, WM_SIZE, 0, 0);
    RECT statusRect{};
    ::GetWindowRect(m_statusBar, &statusRect);
    const int statusHeight = statusRect.bottom - statusRect.top;

    const int pad = Scaled(kPad);
    const int gap = Scaled(kGap);
    const int rowHeight = Scaled(kRowHeight);
    const int barHeight = Scaled(kBarHeight);

    const int barTop = std::max(clientHeight - statusHeight - barHeight, 0);
    const int viewHeight = std::max(barTop, 0);

    if (m_view.Handle()) {
        ::SetWindowPos(m_view.Handle(), nullptr, 0, 0, clientWidth, viewHeight,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const int row1Top = barTop + Scaled(4);
    const int row2Top = row1Top + rowHeight + Scaled(3);

    // Left block: a tall Send button with the Press Enter option beside it.
    int left = pad;
    const int sendHeight = rowHeight * 2 + Scaled(3);
    ::SetWindowPos(m_sendButton, nullptr, left, row1Top, Scaled(kSendWidth), sendHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    left += Scaled(kSendWidth) + gap;
    ::SetWindowPos(m_pressEnterCheck, nullptr, left, row1Top + (sendHeight - rowHeight) / 2,
                   Scaled(96), rowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    const int leftBlockEnd = left + Scaled(96) + gap;

    // Right blocks are laid out from the right edge inwards, mirroring the
    // reference design's ordering.
    auto placeRight = [&](HWND control, int& cursor, int top, int width, int height) {
        cursor -= width;
        ::SetWindowPos(control, nullptr, cursor, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        cursor -= gap;
    };

    int cursor = clientWidth - pad;
    placeRight(m_spacingCombo, cursor, row1Top, Scaled(62), rowHeight);
    placeRight(m_sizeCombo, cursor, row1Top, Scaled(62), rowHeight);
    placeRight(m_fontCombo, cursor, row1Top, Scaled(140), rowHeight);
    placeRight(m_viewModeButton, cursor, row1Top, Scaled(96), rowHeight);
    const int hintWidth = std::max(cursor - leftBlockEnd, 0);
    ::SetWindowPos(m_hintLabel, nullptr, leftBlockEnd, row1Top + Scaled(4), hintWidth, rowHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    cursor = clientWidth - pad;
    placeRight(m_copyButton, cursor, row2Top, Scaled(70), rowHeight);
    placeRight(m_minimizeAllButton, cursor, row2Top, Scaled(90), rowHeight);
    placeRight(m_frontAllButton, cursor, row2Top, Scaled(76), rowHeight);
    const int checkWidth = std::max(cursor - leftBlockEnd, Scaled(120));
    ::SetWindowPos(m_copyLiveCheck, nullptr, leftBlockEnd, row2Top, checkWidth, rowHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

void MainWindow::UpdateHotkeyRegistration() {
    if (m_hotkeyRegistered) {
        ::UnregisterHotKey(m_hwnd, HOTKEY_TOGGLE_VIEW);
        m_hotkeyRegistered = false;
    }

    const unsigned key = m_settings.hotkeyVirtualKey;
    // Fall back through progressively more heavily modified combinations: the
    // preferred one may be rejected outright (bare Shift+letter) or already be
    // owned by another process.
    const unsigned candidates[] = {
        m_settings.hotkeyModifiers,
        MOD_CONTROL | MOD_SHIFT,
        MOD_CONTROL | MOD_ALT,
        MOD_CONTROL | MOD_ALT | MOD_SHIFT,
    };

    unsigned active = 0;
    for (unsigned modifiers : candidates) {
        if (modifiers == 0) continue;
        if (::RegisterHotKey(m_hwnd, HOTKEY_TOGGLE_VIEW, modifiers | MOD_NOREPEAT, key)) {
            active = modifiers;
            m_hotkeyRegistered = true;
            break;
        }
    }

    std::wstring title = kBaseTitle;
    if (m_hotkeyRegistered) {
        m_settings.hotkeyModifiers = active;
        std::wstring combo;
        if (active & MOD_CONTROL) combo += L"Ctrl+";
        if (active & MOD_ALT) combo += L"Alt+";
        if (active & MOD_SHIFT) combo += L"Shift+";
        if (active & MOD_WIN) combo += L"Win+";
        combo.push_back(static_cast<wchar_t>(key));
        title += L" (Hotkey - " + combo + L")";
    } else {
        title += L" (hotkey unavailable)";
    }
    ::SetWindowTextW(m_hwnd, title.c_str());
}

void MainWindow::SetStatus(const std::wstring& text) {
    if (m_statusBar) ::SendMessageW(m_statusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void MainWindow::ApplyTypography() {
    m_view.SetTypography(m_settings.fontFamily, m_settings.fontSizePt, m_settings.lineSpacing);
}

void MainWindow::ApplyViewMode() {
    // Compact view drops the title bar and pins the window above other windows,
    // but keeps the toolbar so the mode can be switched back.
    const LONG_PTR style = ::GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    const LONG_PTR wanted = m_settings.compactView
                                ? ((style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) |
                                   WS_POPUP | WS_THICKFRAME)
                                : ((style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_OVERLAPPEDWINDOW);
    if (style != wanted) {
        ::SetWindowLongPtrW(m_hwnd, GWL_STYLE, wanted);
        ::SetWindowPos(m_hwnd, m_settings.compactView ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
    }
    ::SetWindowTextW(m_viewModeButton, m_settings.compactView ? L"Compact View" : L"Normal View");
    Layout();
}

void MainWindow::ToggleVisibility() {
    if (::IsWindowVisible(m_hwnd) && !::IsIconic(m_hwnd)) {
        ::ShowWindow(m_hwnd, SW_HIDE);
    } else {
        ::ShowWindow(m_hwnd, SW_SHOW);
        ::SetForegroundWindow(m_hwnd);
    }
}

void MainWindow::OnCommand(int controlId, int notifyCode) {
    switch (controlId) {
        case IDC_BTN_SEND:
            OnSend();
            return;

        case IDC_CHK_PRESS_ENTER:
            m_settings.pressEnter =
                ::SendMessageW(m_pressEnterCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return;

        case IDC_CHK_COPY_LIVE:
            m_settings.copyRealtime =
                ::SendMessageW(m_copyLiveCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return;

        case IDC_BTN_VIEW_MODE:
            m_settings.compactView = !m_settings.compactView;
            ApplyViewMode();
            return;

        case IDC_BTN_FRONT_ALL:
            OnFrontAll();
            return;

        case IDC_BTN_MINIMIZE_ALL:
            OnMinimizeAll();
            return;

        case IDC_BTN_COPY:
            OnCopy();
            return;

        case IDC_CBO_FONT: {
            if (notifyCode != CBN_SELCHANGE) return;
            const int index = static_cast<int>(::SendMessageW(m_fontCombo, CB_GETCURSEL, 0, 0));
            if (index == CB_ERR) return;
            const int length = static_cast<int>(::SendMessageW(m_fontCombo, CB_GETLBTEXTLEN, index, 0));
            if (length <= 0) return;
            std::wstring family(static_cast<size_t>(length) + 1, L'\0');
            ::SendMessageW(m_fontCombo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(family.data()));
            family.resize(static_cast<size_t>(length));
            m_settings.fontFamily = family;
            ApplyTypography();
            return;
        }

        case IDC_CBO_SIZE: {
            if (notifyCode != CBN_SELCHANGE) return;
            const int index = static_cast<int>(::SendMessageW(m_sizeCombo, CB_GETCURSEL, 0, 0));
            if (index == CB_ERR) return;
            wchar_t text[16]{};
            ::SendMessageW(m_sizeCombo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
            m_settings.fontSizePt = _wtoi(text);
            ApplyTypography();
            return;
        }

        case IDC_CBO_SPACING: {
            if (notifyCode != CBN_SELCHANGE) return;
            const int index = static_cast<int>(::SendMessageW(m_spacingCombo, CB_GETCURSEL, 0, 0));
            if (index == CB_ERR) return;
            wchar_t text[16]{};
            ::SendMessageW(m_spacingCombo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
            m_settings.lineSpacing = _wtof(text);
            ApplyTypography();
            return;
        }

        default:
            return;
    }
}

void MainWindow::OnSend() {
    // Intentionally inert for now: the destination for "Send" is still to be
    // specified. Wire the real behaviour in here.
    SetStatus(L"Send is not wired up yet.");
}

void MainWindow::OnCopy() {
    if (m_view.Empty()) {
        SetStatus(L"Nothing to copy yet.");
        return;
    }
    if (CopyTextToClipboard(m_view.FullText())) {
        SetStatus(L"Transcript copied to the clipboard.");
    } else {
        SetStatus(L"Could not open the clipboard.");
    }
}

void MainWindow::OnFrontAll() {
    if (m_sourceWindow && ::IsWindow(m_sourceWindow)) {
        if (::IsIconic(m_sourceWindow)) ::ShowWindow(m_sourceWindow, SW_RESTORE);
        ::SetWindowPos(m_sourceWindow, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (::IsIconic(m_hwnd)) ::ShowWindow(m_hwnd, SW_RESTORE);
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd);
}

void MainWindow::OnMinimizeAll() {
    if (m_sourceWindow && ::IsWindow(m_sourceWindow) && !::IsIconic(m_sourceWindow)) {
        ::ShowWindow(m_sourceWindow, SW_MINIMIZE);
    }
    ::ShowWindow(m_hwnd, SW_MINIMIZE);
}

void MainWindow::OnCaptionUpdate(CaptionUpdate* update) {
    if (!update) return;
    m_view.ApplyUpdate(update->firstDirtyLine, std::move(update->lines));
    MaybeCopyRealtime();
}

void MainWindow::MaybeCopyRealtime() {
    if (!m_settings.copyRealtime) return;
    if (::GetForegroundWindow() != m_hwnd) return;

    const ULONGLONG now = ::GetTickCount64();
    if (now - m_lastRealtimeCopyTick < kRealtimeCopyIntervalMs) return;
    m_lastRealtimeCopyTick = now;

    CopyTextToClipboard(m_view.FullText());
}

bool MainWindow::CopyTextToClipboard(const std::wstring& text) {
    if (!::OpenClipboard(m_hwnd)) return false;

    bool ok = false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (const HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (auto* buffer = static_cast<wchar_t*>(::GlobalLock(handle))) {
            std::memcpy(buffer, text.c_str(), bytes);
            ::GlobalUnlock(handle);
            ::EmptyClipboard();
            if (::SetClipboardData(CF_UNICODETEXT, handle)) {
                ok = true;  // clipboard owns the handle now
            } else {
                ::GlobalFree(handle);
            }
        } else {
            ::GlobalFree(handle);
        }
    }
    ::CloseClipboard();
    return ok;
}

void MainWindow::DrainPendingPayloads() {
    MSG msg;
    while (::PeekMessageW(&msg, m_hwnd, WM_APP_CAPTION_UPDATE, WM_APP_CAPTION_UPDATE, PM_REMOVE)) {
        delete reinterpret_cast<CaptionUpdate*>(msg.lParam);
    }
    while (::PeekMessageW(&msg, m_hwnd, WM_APP_STATUS, WM_APP_STATUS, PM_REMOVE)) {
        delete reinterpret_cast<std::wstring*>(msg.lParam);
    }
}
