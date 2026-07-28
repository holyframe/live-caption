#include "MainWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <set>

#include "Util.h"
#include "resource.h"

namespace {

constexpr const wchar_t* kClassName = L"LiveCaptionViewMain";
constexpr const wchar_t* kBaseTitle = L"Live Caption App";

// Unscaled (96 dpi) toolbar geometry. The bottom panel's minimum height is
// exactly the button row plus the top/bottom insets used when laying it out.
constexpr int kPad          = 8;
constexpr int kRowHeight    = 23;
constexpr int kBarTopPad    = 4;
constexpr int kBarBottomPad = 5;
constexpr int kBarHeight    = kBarTopPad + kRowHeight + kBarBottomPad;
constexpr int kGap          = 6;
constexpr int kSendWidth    = 92;
// Full-height strip along the right edge, reserved for control buttons.
constexpr int kRightPanelWidth = 45;
constexpr int kRightButtonSize = 32;
// Fallback height for the log view when the font metrics are unavailable.
constexpr int kLogHeight    = 22;
// Draggable strip between the caption pane and the bottom panel, and the
// smallest caption pane a drag may leave behind.
constexpr int kSplitterHeight  = 6;
constexpr int kMinViewHeight   = 80;

// Dark UI palette, aligned with the caption pane.
constexpr COLORREF kWindowBg       = RGB(32, 32, 32);
constexpr COLORREF kPanelBg        = RGB(36, 36, 36);
constexpr COLORREF kButtonFace     = RGB(55, 55, 55);
constexpr COLORREF kButtonPressed  = RGB(70, 70, 70);
constexpr COLORREF kButtonBorder   = RGB(90, 90, 90);
constexpr COLORREF kTextPrimary    = RGB(228, 228, 228);
constexpr COLORREF kTextSecondary  = RGB(170, 170, 170);
constexpr COLORREF kStatusBg       = RGB(28, 28, 28);
constexpr COLORREF kSplitterLine   = RGB(72, 72, 72);

// Throttle for "copy real-time"; the caption updates ~10x per second and we do
// not want to thrash the clipboard that hard.
constexpr ULONGLONG kRealtimeCopyIntervalMs = 600;

int CALLBACK EnumFontProc(const LOGFONTW* logFont, const TEXTMETRICW*, DWORD, LPARAM param) {
    auto* names = reinterpret_cast<std::set<std::wstring>*>(param);
    if (logFont->lfFaceName[0] != L'@') names->insert(logFont->lfFaceName);
    return 1;
}

void TryDarkControlTheme(HWND hwnd, const wchar_t* theme) {
    if (!hwnd) return;
    // Prefer the immersive dark control theme when the OS provides it; fall
    // back to the classic unthemed look so WM_CTLCOLOR* can paint dark.
    if (FAILED(::SetWindowTheme(hwnd, theme, nullptr))) {
        ::SetWindowTheme(hwnd, L"", L"");
    }
}

}  // namespace

bool MainWindow::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // painted dark in WM_ERASEBKGND
    wc.lpszClassName = kClassName;
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    return ::RegisterClassExW(&wc) != 0;
}

int MainWindow::Scaled(int value) const {
    return ::MulDiv(value, static_cast<int>(m_dpi), 96);
}

int MainWindow::Unscaled(int value) const {
    return ::MulDiv(value, 96, static_cast<int>(m_dpi));
}

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
    m_instance = instance;
    m_settings.Load();

    const int width = std::max(m_settings.windowW, 480);
    const int height = std::max(m_settings.windowH, 320);
    const int x = m_settings.windowX >= 0 ? m_settings.windowX : CW_USEDEFAULT;
    const int y = m_settings.windowY >= 0 ? m_settings.windowY : CW_USEDEFAULT;

    // WS_CLIPCHILDREN keeps the background erase out of the panes, which would
    // otherwise flicker through them on every splitter drag step.
    m_hwnd = ::CreateWindowExW(0, kClassName, kBaseTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x,
                               y, width, height, nullptr, nullptr, instance, this);
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

            EnsureThemeBrushes();

            // Match the dark title bar in the reference design. Ignored on
            // builds that predate the attribute.
            const BOOL darkTitleBar = TRUE;
            ::DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitleBar,
                                    sizeof(darkTitleBar));

            if (!CreateChildren()) return -1;
            ApplyDarkTheme();
            UpdateHotkeyRegistration();
            Layout();
            ApplyTypography();
            ApplyViewMode();

            m_engine.Start(m_hwnd, m_settings.ResolvedTranscriptPath(), m_settings.pollIntervalMs);
            SetStatus(std::wstring(L"Saving transcript to ") + m_settings.ResolvedTranscriptPath());
            return 0;
        }

        case WM_ERASEBKGND: {
            if (!m_windowBrush) EnsureThemeBrushes();
            const HDC dc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            ::GetClientRect(m_hwnd, &rc);
            ::FillRect(dc, &rc, m_windowBrush);
            DrawSplitter(dc);
            return 1;
        }

        case WM_SETCURSOR: {
            POINT pt{};
            if (LOWORD(lParam) == HTCLIENT && ::GetCursorPos(&pt) &&
                ::ScreenToClient(m_hwnd, &pt) &&
                (m_splitterDrag || ::PtInRect(&m_splitterRect, pt))) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (::PtInRect(&m_splitterRect, pt)) {
                m_splitterDrag = true;
                m_splitterGrab = pt.y - m_splitterRect.top;
                ::SetCapture(m_hwnd);
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE:
            if (m_splitterDrag) {
                DragSplitterTo(GET_Y_LPARAM(lParam) - m_splitterGrab);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (m_splitterDrag) {
                m_splitterDrag = false;
                ::ReleaseCapture();
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            m_splitterDrag = false;
            return 0;

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

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item && item->CtlType == ODT_BUTTON) {
                DrawDarkButton(*item);
                return TRUE;
            }
            break;
        }

        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr && hdr->hwndFrom == m_statusBar && hdr->code == NM_CUSTOMDRAW) {
                const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
                if (draw->dwDrawStage == CDDS_PREPAINT) {
                    ::SetTextColor(draw->hdc, kTextSecondary);
                    ::SetBkColor(draw->hdc, kStatusBg);
                    return CDRF_NEWFONT;
                }
            }
            break;
        }

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
            return 0;

        case WM_SETFOCUS:
            if (m_view.Handle()) ::SetFocus(m_view.Handle());
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return OnCtlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));

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
            DiscardThemeBrushes();
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

    // Both panels are plain backdrops. The bottom one sits under the toolbar
    // controls, so every control below carries WS_CLIPSIBLINGS and Layout()
    // keeps the panels at the bottom of the z-order.
    const auto panel = [&](int id) {
        return ::CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 m_instance, nullptr);
    };
    m_rightPanel = panel(IDC_RIGHT_PANEL);
    m_bottomPanel = panel(IDC_BOTTOM_PANEL);
    if (!m_rightPanel || !m_bottomPanel) return false;

    const auto button = [&](const wchar_t* text, int id) {
        return ::CreateWindowExW(0, WC_BUTTONW, text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                                     BS_OWNERDRAW,
                                 0, 0, 10, 10, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };
    const auto check = [&](const wchar_t* text, int id) {
        return ::CreateWindowExW(0, WC_BUTTONW, text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                                     BS_AUTOCHECKBOX,
                                 0, 0, 10, 10, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };
    const auto combo = [&](int id) {
        return ::CreateWindowExW(0, WC_COMBOBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                                     CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 10, 200, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance,
                                 nullptr);
    };

    m_sendButton      = button(L"Send", IDC_BTN_SEND);
    m_pressEnterCheck = check(L"Press Enter", IDC_CHK_PRESS_ENTER);
    // Caption text doubles as the accessible name; DrawDarkButton paints icons.
    m_settingsButton    = button(L"Settings", IDC_BTN_SETTINGS);
    m_pickWindowButton  = button(L"Pick up window", IDC_BTN_PICK_WINDOW);

    m_hintLabel = ::CreateWindowExW(0, WC_STATICW, L"Double-click empty area to send",
                                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_RIGHT, 0, 0, 10,
                                    10, m_hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LBL_HINT)),
                                    m_instance, nullptr);

    m_fontCombo    = combo(IDC_CBO_FONT);
    m_sizeCombo    = combo(IDC_CBO_SIZE);
    m_spacingCombo = combo(IDC_CBO_SPACING);

    // The log view only spans the left column, so the status bar must not keep
    // stretching itself across the parent; CCS_NORESIZE hands placement to
    // Layout(). No size grip either, since the corner belongs to the right panel.
    m_statusBar = ::CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                    WS_CHILD | WS_VISIBLE | CCS_NOPARENTALIGN | CCS_NORESIZE, 0, 0,
                                    10, 10, m_hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUSBAR)),
                                    m_instance, nullptr);

    if (!m_sendButton || !m_settingsButton || !m_pickWindowButton || !m_fontCombo ||
        !m_statusBar) {
        return false;
    }

    ::SendMessageW(m_pressEnterCheck, BM_SETCHECK, m_settings.pressEnter ? BST_CHECKED : BST_UNCHECKED, 0);

    PopulateFontCombo();
    PopulateSizeCombo();
    PopulateSpacingCombo();
    ApplyControlFont();
    return true;
}

void MainWindow::EnsureThemeBrushes() {
    if (!m_windowBrush) m_windowBrush = ::CreateSolidBrush(kWindowBg);
    if (!m_panelBrush) m_panelBrush = ::CreateSolidBrush(kPanelBg);
    if (!m_buttonBrush) m_buttonBrush = ::CreateSolidBrush(kButtonFace);
    if (!m_buttonPressedBrush) m_buttonPressedBrush = ::CreateSolidBrush(kButtonPressed);
    if (!m_splitterBrush) m_splitterBrush = ::CreateSolidBrush(kSplitterLine);
}

void MainWindow::DiscardThemeBrushes() {
    if (m_windowBrush) {
        ::DeleteObject(m_windowBrush);
        m_windowBrush = nullptr;
    }
    if (m_panelBrush) {
        ::DeleteObject(m_panelBrush);
        m_panelBrush = nullptr;
    }
    if (m_buttonBrush) {
        ::DeleteObject(m_buttonBrush);
        m_buttonBrush = nullptr;
    }
    if (m_buttonPressedBrush) {
        ::DeleteObject(m_buttonPressedBrush);
        m_buttonPressedBrush = nullptr;
    }
    if (m_splitterBrush) {
        ::DeleteObject(m_splitterBrush);
        m_splitterBrush = nullptr;
    }
}

void MainWindow::ApplyDarkTheme() {
    EnsureThemeBrushes();

    const HWND explorer[] = {m_pressEnterCheck, m_hintLabel, m_rightPanel,
                             m_bottomPanel,     m_statusBar, m_view.Handle()};
    for (HWND control : explorer) TryDarkControlTheme(control, L"DarkMode_Explorer");

    const HWND combos[] = {m_fontCombo, m_sizeCombo, m_spacingCombo};
    for (HWND control : combos) TryDarkControlTheme(control, L"DarkMode_CFD");

    if (m_statusBar) {
        ::SendMessageW(m_statusBar, SB_SETBKCOLOR, 0, static_cast<LPARAM>(kStatusBg));
    }

    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

LRESULT MainWindow::OnCtlColor(HDC dc, HWND control) {
    EnsureThemeBrushes();

    const bool isHint = control == m_hintLabel;
    // The panels, and the controls that sit on the bottom panel, share its fill.
    const bool onPanel = control == m_rightPanel || control == m_bottomPanel ||
                         control == m_hintLabel || control == m_pressEnterCheck;
    const COLORREF text = isHint ? kTextSecondary : kTextPrimary;
    const COLORREF background = onPanel ? kPanelBg : kWindowBg;
    const HBRUSH brush = onPanel ? m_panelBrush : m_windowBrush;

    ::SetTextColor(dc, text);
    ::SetBkColor(dc, background);
    ::SetBkMode(dc, OPAQUE);
    return reinterpret_cast<LRESULT>(brush);
}

void MainWindow::DrawDarkButton(const DRAWITEMSTRUCT& item) const {
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    const HBRUSH fill = pressed ? m_buttonPressedBrush : m_buttonBrush;
    ::FillRect(item.hDC, &item.rcItem, fill ? fill : reinterpret_cast<HBRUSH>(::GetStockObject(DKGRAY_BRUSH)));

    const HPEN border = ::CreatePen(PS_SOLID, 1, kButtonBorder);
    const HGDIOBJ oldPen = ::SelectObject(item.hDC, border);
    const HGDIOBJ oldBrush = ::SelectObject(item.hDC, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    ::SelectObject(item.hDC, oldBrush);
    ::SelectObject(item.hDC, oldPen);
    ::DeleteObject(border);

    ::SetBkMode(item.hDC, TRANSPARENT);
    ::SetTextColor(item.hDC, disabled ? kTextSecondary : kTextPrimary);

    RECT contentRc = item.rcItem;
    if (pressed) ::OffsetRect(&contentRc, 1, 1);

    // Right-panel icon buttons use Segoe MDL2 Assets glyphs.
    const wchar_t* iconGlyph = nullptr;
    if (item.hwndItem == m_settingsButton) {
        iconGlyph = L"\uE713";  // Settings (gear)
    } else if (item.hwndItem == m_pickWindowButton) {
        iconGlyph = L"\uE78B";  // Window with up-arrow (pick up / raise window)
    }

    if (iconGlyph) {
        const int iconPx = std::max(Scaled(16), 12);
        HFONT iconFont = ::CreateFontW(-iconPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                       L"Segoe MDL2 Assets");
        const HGDIOBJ oldFont = iconFont ? ::SelectObject(item.hDC, iconFont) : nullptr;
        ::DrawTextW(item.hDC, iconGlyph, 1, &contentRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (oldFont) ::SelectObject(item.hDC, oldFont);
        if (iconFont) ::DeleteObject(iconFont);
    } else {
        wchar_t text[128]{};
        ::GetWindowTextW(item.hwndItem, text, 128);
        if (m_controlFont) ::SelectObject(item.hDC, m_controlFont);
        ::DrawTextW(item.hDC, text, -1, &contentRc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (focused && !pressed) {
        RECT focusRc = item.rcItem;
        ::InflateRect(&focusRc, -3, -3);
        ::DrawFocusRect(item.hDC, &focusRc);
    }
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

    const HWND controls[] = {m_sendButton, m_pressEnterCheck, m_hintLabel, m_fontCombo,
                             m_sizeCombo,  m_spacingCombo,    m_statusBar};
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

void MainWindow::DrawSplitter(HDC dc) const {
    if (!m_splitterBrush || ::IsRectEmpty(&m_splitterRect)) return;
    // A hairline down the middle of the grab strip, so the strip reads as a
    // divider rather than as a gap.
    const int thickness = std::max(Scaled(1), 1);
    RECT line = m_splitterRect;
    line.top += (line.bottom - line.top - thickness) / 2;
    line.bottom = line.top + thickness;
    ::FillRect(dc, &line, m_splitterBrush);
}

// Keeps the bottom panel tall enough for its toolbar row and short enough to
// leave a usable caption pane, whatever the window has been resized to.
int MainWindow::ClampBottomPanelHeight(int wanted, int clientHeight) const {
    const int minimum = Scaled(kBarHeight);
    const int available =
        clientHeight - LogBarHeight() - Scaled(kSplitterHeight) - Scaled(kMinViewHeight);
    return std::clamp(wanted, minimum, std::max(available, minimum));
}

void MainWindow::DragSplitterTo(int splitterTop) {
    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    const int clientHeight = client.bottom - client.top;

    const int logTop = std::max(clientHeight - LogBarHeight(), 0);
    const int wanted = logTop - splitterTop - Scaled(kSplitterHeight);
    const int height = Unscaled(ClampBottomPanelHeight(wanted, clientHeight));
    if (height == m_settings.bottomPanelHeight) return;

    m_settings.bottomPanelHeight = height;
    Layout();
}

int MainWindow::LogBarHeight() const {
    int height = Scaled(kLogHeight);
    if (const HDC dc = ::GetDC(m_hwnd)) {
        const HGDIOBJ previous = m_controlFont ? ::SelectObject(dc, m_controlFont) : nullptr;
        TEXTMETRICW metrics{};
        if (::GetTextMetricsW(dc, &metrics)) height = metrics.tmHeight + Scaled(8);
        if (previous) ::SelectObject(dc, previous);
        ::ReleaseDC(m_hwnd, dc);
    }
    return height;
}

void MainWindow::Layout() {
    if (!m_hwnd || !m_statusBar) return;

    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;

    const int pad = Scaled(kPad);
    const int gap = Scaled(kGap);
    const int rowHeight = Scaled(kRowHeight);
    const int logHeight = LogBarHeight();
    const int splitterHeight = Scaled(kSplitterHeight);
    const int barHeight = ClampBottomPanelHeight(Scaled(m_settings.bottomPanelHeight), clientHeight);

    // The right panel runs the full height of the window. Everything else
    // stacks inside the column left of it: caption pane, splitter, bottom
    // panel, log view.
    const int rightPanelWidth = Scaled(kRightPanelWidth);
    const int columnWidth = std::max(clientWidth - rightPanelWidth, 0);
    const int logTop = std::max(clientHeight - logHeight, 0);
    const int barTop = std::max(logTop - barHeight, 0);
    const int viewHeight = std::max(barTop - splitterHeight, 0);

    m_splitterRect = RECT{0, viewHeight, columnWidth, barTop};

    if (m_view.Handle()) {
        ::SetWindowPos(m_view.Handle(), nullptr, 0, 0, columnWidth, viewHeight,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Both panels are backdrops, so keep them under their contents.
    if (m_bottomPanel) {
        ::SetWindowPos(m_bottomPanel, HWND_BOTTOM, 0, barTop, columnWidth,
                       std::max(logTop - barTop, 0), SWP_NOACTIVATE);
    }
    ::SetWindowPos(m_statusBar, nullptr, 0, logTop, columnWidth,
                   std::max(clientHeight - logTop, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_rightPanel) {
        ::SetWindowPos(m_rightPanel, HWND_BOTTOM, columnWidth, 0, rightPanelWidth, clientHeight,
                       SWP_NOACTIVATE);
    }

    // Right-panel icon buttons: window picker at the top, settings gear at the foot.
    const int buttonSize = Scaled(kRightButtonSize);
    const int buttonLeft = columnWidth + std::max((rightPanelWidth - buttonSize) / 2, 0);
    if (m_pickWindowButton) {
        ::SetWindowPos(m_pickWindowButton, nullptr, buttonLeft, pad, buttonSize, buttonSize,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (m_settingsButton) {
        const int buttonTop = std::max(clientHeight - pad - buttonSize, 0);
        ::SetWindowPos(m_settingsButton, nullptr, buttonLeft, buttonTop, buttonSize, buttonSize,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // The toolbar hugs the foot of the bottom panel. A taller panel then opens
    // space under the splitter rather than carrying the controls up with it, so
    // they stay under the pointer while the caption pane is being resized.
    const int rowTop =
        std::max(logTop - Scaled(kBarBottomPad) - rowHeight, barTop + Scaled(kBarTopPad));

    // Left block: Send with the Press Enter option beside it.
    int left = pad;
    ::SetWindowPos(m_sendButton, nullptr, left, rowTop, Scaled(kSendWidth), rowHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    left += Scaled(kSendWidth) + gap;
    ::SetWindowPos(m_pressEnterCheck, nullptr, left, rowTop, Scaled(96), rowHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    const int leftBlockEnd = left + Scaled(96) + gap;

    // Right blocks are laid out from the right edge inwards.
    auto placeRight = [&](HWND control, int& cursor, int top, int width, int height) {
        cursor -= width;
        ::SetWindowPos(control, nullptr, cursor, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        cursor -= gap;
    };

    int cursor = columnWidth - pad;
    placeRight(m_spacingCombo, cursor, rowTop, Scaled(62), rowHeight);
    placeRight(m_sizeCombo, cursor, rowTop, Scaled(62), rowHeight);
    placeRight(m_fontCombo, cursor, rowTop, Scaled(140), rowHeight);
    const int hintWidth = std::max(cursor - leftBlockEnd, 0);
    ::SetWindowPos(m_hintLabel, nullptr, leftBlockEnd, rowTop + Scaled(4), hintWidth, rowHeight,
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

        case IDC_BTN_SETTINGS:
            OnSettings();
            return;

        case IDC_BTN_PICK_WINDOW:
            OnPickWindow();
            return;

        case IDC_CHK_PRESS_ENTER:
            m_settings.pressEnter =
                ::SendMessageW(m_pressEnterCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            return;

        case IDC_BTN_COPY:
            // Still used by the caption pane (Ctrl+C / context menu).
            OnCopy();
            return;

        case IDCANCEL:
            // IsDialogMessage turns Escape into this before the caption pane can
            // see the key, so the pane's own handler never runs.
            m_view.ClearSelection();
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

void MainWindow::OnSettings() {
    // Placeholder until a settings surface exists.
    SetStatus(L"Settings is not wired up yet.");
}

void MainWindow::OnPickWindow() {
    // Placeholder until window picking is implemented.
    SetStatus(L"Pick up window is not wired up yet.");
}

void MainWindow::OnCopy() {
    if (m_view.Empty()) {
        SetStatus(L"Nothing to copy yet.");
        return;
    }
    const bool selectionOnly = m_view.HasSelection();
    const std::wstring text = selectionOnly ? m_view.SelectedText() : m_view.FullText();
    if (CopyTextToClipboard(text)) {
        SetStatus(selectionOnly ? L"Selection copied to the clipboard."
                                : L"Whole transcript copied to the clipboard.");
    } else {
        SetStatus(L"Could not open the clipboard.");
    }
}

void MainWindow::OnCaptionUpdate(CaptionUpdate* update) {
    if (!update) return;
    m_view.QueueUpdate(update->firstDirtyLine, std::move(update->lines));

    // The capture thread reads far faster than a frame lasts, so by the time we
    // get here there may already be newer updates waiting. Folding them into
    // this same repaint keeps the queue from growing without bound, which would
    // otherwise leave the view drifting steadily further behind the source and
    // only catching up when the speaker pauses.
    MSG queued;
    while (::PeekMessageW(&queued, m_hwnd, WM_APP_CAPTION_UPDATE, WM_APP_CAPTION_UPDATE,
                          PM_REMOVE)) {
        auto* pending = reinterpret_cast<CaptionUpdate*>(queued.lParam);
        if (!pending) continue;
        m_view.QueueUpdate(pending->firstDirtyLine, std::move(pending->lines));
        delete pending;
    }

    m_view.Present();
    MaybeCopyRealtime();
}

void MainWindow::MaybeCopyRealtime() {
    if (!m_settings.copyRealtime) return;
    if (::GetForegroundWindow() != m_hwnd) return;
    // Whatever the user just picked out by hand outranks the running transcript.
    if (m_view.HasSelection()) return;

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
