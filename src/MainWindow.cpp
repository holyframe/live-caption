#include "MainWindow.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <set>

#include "Util.h"
#include "SettingsDialog.h"
#include "resource.h"

namespace {

constexpr const wchar_t* kClassName = L"LiveCaptionViewMain";
constexpr const wchar_t* kPickOutlineClassName = L"LiveCaptionViewPickOutline";
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
constexpr COLORREF kPickOutline    = RGB(255, 0, 0);
constexpr COLORREF kLightWindowBg      = RGB(255, 255, 255);
constexpr COLORREF kLightPanelBg       = RGB(246, 246, 246);
constexpr COLORREF kLightButtonFace    = RGB(240, 240, 240);
constexpr COLORREF kLightButtonPressed = RGB(220, 220, 220);
constexpr COLORREF kLightButtonBorder  = RGB(150, 150, 150);
constexpr COLORREF kLightTextPrimary   = RGB(32, 32, 32);
constexpr COLORREF kLightTextSecondary = RGB(96, 96, 96);
constexpr COLORREF kLightStatusBg      = RGB(246, 246, 246);
constexpr COLORREF kLightSplitterLine  = RGB(205, 205, 205);

// Throttle for "copy real-time"; the caption updates ~10x per second and we do
// not want to thrash the clipboard that hard.
constexpr ULONGLONG kRealtimeCopyIntervalMs = 600;
constexpr UINT_PTR kHotkeySendTimerId = 0xCA51;
constexpr UINT kHotkeySendPollMs = 10;

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

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD bytes = sizeof(value);
    return ::RegGetValueW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &bytes) ==
               ERROR_SUCCESS &&
           value == 0;
}

bool EffectiveDarkTheme(UiTheme theme) {
    return theme == UiTheme::Dark || (theme == UiTheme::System && SystemUsesDarkTheme());
}

std::wstring HotkeyName(unsigned modifiers, unsigned key) {
    std::wstring name;
    auto append = [&name](const wchar_t* part) {
        if (!name.empty()) name += L"+";
        name += part;
    };

    if (modifiers & MOD_CONTROL) append(L"Ctrl");
    if (modifiers & MOD_ALT) append(L"Alt");
    if (modifiers & MOD_SHIFT) append(L"Shift");
    if (modifiers & MOD_WIN) append(L"Win");

    if (key >= 'A' && key <= 'Z') {
        const wchar_t keyName[] = {static_cast<wchar_t>(key), L'\0'};
        append(keyName);
    } else if (key >= '0' && key <= '9') {
        const wchar_t keyName[] = {static_cast<wchar_t>(key), L'\0'};
        append(keyName);
    } else if (key != 0) {
        wchar_t keyName[64]{};
        UINT scanCode = ::MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        switch (key) {
            case VK_INSERT:
            case VK_DELETE:
            case VK_HOME:
            case VK_END:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
            case VK_DIVIDE:
            case VK_NUMLOCK:
                scanCode |= 0x100;
                break;
        }
        if (::GetKeyNameTextW(static_cast<LONG>(scanCode << 16), keyName,
                              static_cast<int>(std::size(keyName))) > 0) {
            append(keyName);
        } else {
            append(L"Unknown");
        }
    }

    return name.empty() ? L"None" : name;
}

CaptionSourceChoice ToSourceChoice(CaptionSourcePreference source) {
    return source == CaptionSourcePreference::ChromeLiveCaption
               ? CaptionSourceChoice::Chrome
               : CaptionSourceChoice::WindowsLiveCaptions;
}

HICON CopyWindowIcon(HWND window) {
    HICON icon = nullptr;
    const WPARAM sizes[] = {ICON_SMALL2, ICON_SMALL, ICON_BIG};
    for (WPARAM size : sizes) {
        DWORD_PTR result = 0;
        if (::SendMessageTimeoutW(window, WM_GETICON, size, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100,
                                  &result) &&
            result != 0) {
            icon = reinterpret_cast<HICON>(result);
            break;
        }
    }
    if (!icon) {
        icon = reinterpret_cast<HICON>(::GetClassLongPtrW(window, GCLP_HICONSM));
    }
    if (!icon) {
        icon = reinterpret_cast<HICON>(::GetClassLongPtrW(window, GCLP_HICON));
    }
    if (!icon) icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    return icon ? ::CopyIcon(icon) : nullptr;
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
    if (::RegisterClassExW(&wc) == 0) return false;

    WNDCLASSEXW outline{};
    outline.cbSize = sizeof(outline);
    outline.lpfnWndProc = &MainWindow::PickOutlineWndProc;
    outline.hInstance = instance;
    outline.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
    outline.hbrBackground = nullptr;
    outline.lpszClassName = kPickOutlineClassName;
    return ::RegisterClassExW(&outline) != 0;
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

LRESULT CALLBACK MainWindow::PickButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                                    LPARAM lParam, UINT_PTR subclassId,
                                                    DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (!self) return ::DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message) {
        case WM_LBUTTONDOWN:
            ::SetFocus(hwnd);
            self->BeginWindowPick();
            return 0;

        case WM_MOUSEMOVE:
            if (self->m_windowPickDrag) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ClientToScreen(hwnd, &point);
                self->UpdateWindowPick(point);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (self->m_windowPickDrag) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ClientToScreen(hwnd, &point);
                self->UpdateWindowPick(point);
                self->FinishWindowPick(true);
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (self->m_windowPickDrag) {
                const LPCWSTR cursor =
                    self->m_windowPickState == WebInputPickState::Valid ? IDC_CROSS : IDC_NO;
                ::SetCursor(::LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;

        case WM_KEYDOWN:
            if (self->m_windowPickDrag && wParam == VK_ESCAPE) {
                self->FinishWindowPick(false);
                return 0;
            }
            break;

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            if (self->m_windowPickDrag) {
                self->FinishWindowPick(false);
                return 0;
            }
            break;

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, &MainWindow::PickButtonSubclassProc, subclassId);
            break;

        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SelectedIconSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                                      LPARAM lParam, UINT_PTR subclassId,
                                                      DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (!self) return ::DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message) {
        case WM_RBUTTONDOWN:
            if (self->m_webInputPicker.SelectedWindow()) {
                ::SetFocus(hwnd);
                self->BeginSelectedIconRemoval();
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (self->m_selectedIconRemoveDrag) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ClientToScreen(hwnd, &point);
                self->UpdateSelectedIconRemoval(point);
                return 0;
            }
            break;

        case WM_RBUTTONUP:
            if (self->m_selectedIconRemoveDrag) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ClientToScreen(hwnd, &point);
                self->UpdateSelectedIconRemoval(point);
                self->FinishSelectedIconRemoval(true);
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (self->m_selectedIconRemoveDrag) {
                const LPCWSTR cursor = self->m_selectedIconOutside ? IDC_HAND : IDC_SIZEALL;
                ::SetCursor(::LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;

        case WM_KEYDOWN:
            if (self->m_selectedIconRemoveDrag && wParam == VK_ESCAPE) {
                self->FinishSelectedIconRemoval(false);
                return 0;
            }
            break;

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            if (self->m_selectedIconRemoveDrag) {
                self->FinishSelectedIconRemoval(false);
                return 0;
            }
            break;

        case WM_CONTEXTMENU:
            return 0;

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, &MainWindow::SelectedIconSubclassProc, subclassId);
            break;

        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::PickOutlineWndProc(HWND hwnd, UINT message, WPARAM wParam,
                                                LPARAM lParam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND: {
            RECT client{};
            ::GetClientRect(hwnd, &client);
            const HBRUSH red = ::CreateSolidBrush(kPickOutline);
            if (red) {
                ::FillRect(reinterpret_cast<HDC>(wParam), &client, red);
                ::DeleteObject(red);
            }
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = ::BeginPaint(hwnd, &paint);
            RECT client{};
            ::GetClientRect(hwnd, &client);
            const HBRUSH red = ::CreateSolidBrush(kPickOutline);
            if (red) {
                ::FillRect(dc, &client, red);
                ::DeleteObject(red);
            }
            ::EndPaint(hwnd, &paint);
            return 0;
        }

        default:
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
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
            ApplyTheme();
            UpdateHotkeyRegistration();
            Layout();
            ApplyTypography();
            ApplyViewMode();

            m_engine.Start(m_hwnd, m_settings.ResolvedTranscriptPath(), m_settings.pollIntervalMs,
                           ToSourceChoice(m_settings.captionSource));
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
            if (item && item->CtlType == ODT_STATIC &&
                item->hwndItem == m_selectedWindowIconView) {
                DrawSelectedWindowIcon(*item);
                return TRUE;
            }
            break;
        }

        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr && hdr->hwndFrom == m_statusBar && hdr->code == NM_CUSTOMDRAW) {
                const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
                if (draw->dwDrawStage == CDDS_PREPAINT) {
                    ::SetTextColor(draw->hdc,
                                   m_darkTheme ? kTextSecondary : kLightTextSecondary);
                    ::SetBkColor(draw->hdc, m_darkTheme ? kStatusBg : kLightStatusBg);
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
            if (wParam == HOTKEY_SEND) {
                QueueHotkeySend();
                return 0;
            }
            break;

        case WM_TIMER:
            if (wParam == kHotkeySendTimerId && m_hotkeySendPending) {
                if (HotkeyChordReleased()) {
                    ::KillTimer(m_hwnd, kHotkeySendTimerId);
                    m_hotkeySendPending = false;
                    OnSend();
                }
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

        case WM_SETTINGCHANGE:
            if (m_settings.theme == UiTheme::System) ApplyTheme();
            break;

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
            if (m_hotkeySendPending) {
                ::KillTimer(m_hwnd, kHotkeySendTimerId);
                m_hotkeySendPending = false;
            }
            if (m_pickOutlineWindow) {
                ::DestroyWindow(m_pickOutlineWindow);
                m_pickOutlineWindow = nullptr;
            }
            if (m_pickedWindowIcon) {
                ::DestroyIcon(m_pickedWindowIcon);
                m_pickedWindowIcon = nullptr;
            }
            if (m_hotkeyRegistered) {
                ::UnregisterHotKey(m_hwnd, HOTKEY_SEND);
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
    m_saveButton        = button(L"Save captions", IDC_BTN_SAVE);
    m_clearButton       = button(L"Clear captions", IDC_BTN_CLEAR);
    m_settingsButton    = button(L"Settings", IDC_BTN_SETTINGS);
    m_pickWindowButton  = button(L"Pick up window", IDC_BTN_PICK_WINDOW);
    m_selectedWindowIconView =
        ::CreateWindowExW(0, WC_STATICW, L"",
                          WS_CHILD | WS_CLIPSIBLINGS | SS_OWNERDRAW | SS_NOTIFY, 0, 0, 10, 10,
                          m_hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SELECTED_WINDOW)),
                          m_instance, nullptr);
    if (m_selectedWindowIconView &&
        !::SetWindowSubclass(m_selectedWindowIconView, &MainWindow::SelectedIconSubclassProc,
                             IDC_SELECTED_WINDOW, reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    if (m_pickWindowButton &&
        !::SetWindowSubclass(m_pickWindowButton, &MainWindow::PickButtonSubclassProc,
                             IDC_BTN_PICK_WINDOW, reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    m_pickOutlineWindow = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kPickOutlineClassName, L"", WS_POPUP, 0, 0, 0, 0, m_hwnd, nullptr, m_instance, nullptr);
    if (!m_pickOutlineWindow) return false;

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

    if (!m_sendButton || !m_saveButton || !m_clearButton || !m_settingsButton ||
        !m_pickWindowButton ||
        !m_selectedWindowIconView || !m_fontCombo || !m_statusBar) {
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
    if (!m_windowBrush)
        m_windowBrush = ::CreateSolidBrush(m_darkTheme ? kWindowBg : kLightWindowBg);
    if (!m_panelBrush)
        m_panelBrush = ::CreateSolidBrush(m_darkTheme ? kPanelBg : kLightPanelBg);
    if (!m_buttonBrush)
        m_buttonBrush = ::CreateSolidBrush(m_darkTheme ? kButtonFace : kLightButtonFace);
    if (!m_buttonPressedBrush)
        m_buttonPressedBrush =
            ::CreateSolidBrush(m_darkTheme ? kButtonPressed : kLightButtonPressed);
    if (!m_splitterBrush)
        m_splitterBrush =
            ::CreateSolidBrush(m_darkTheme ? kSplitterLine : kLightSplitterLine);
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

void MainWindow::ApplyTheme() {
    m_darkTheme = EffectiveDarkTheme(m_settings.theme);
    DiscardThemeBrushes();
    EnsureThemeBrushes();

    const HWND explorer[] = {m_pressEnterCheck,       m_hintLabel, m_rightPanel,
                             m_bottomPanel,           m_statusBar, m_view.Handle(),
                             m_selectedWindowIconView};
    for (HWND control : explorer)
        TryDarkControlTheme(control, m_darkTheme ? L"DarkMode_Explorer" : L"Explorer");

    const HWND combos[] = {m_fontCombo, m_sizeCombo, m_spacingCombo};
    for (HWND control : combos)
        TryDarkControlTheme(control, m_darkTheme ? L"DarkMode_CFD" : L"CFD");

    if (m_statusBar) {
        ::SendMessageW(m_statusBar, SB_SETBKCOLOR, 0,
                       static_cast<LPARAM>(m_darkTheme ? kStatusBg : kLightStatusBg));
    }
    m_view.SetDarkTheme(m_darkTheme);
    const BOOL darkTitleBar = m_darkTheme ? TRUE : FALSE;
    ::DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitleBar,
                            sizeof(darkTitleBar));

    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

LRESULT MainWindow::OnCtlColor(HDC dc, HWND control) {
    EnsureThemeBrushes();

    const bool isHint = control == m_hintLabel;
    // The panels, and the controls that sit on the bottom panel, share its fill.
    const bool onPanel = control == m_rightPanel || control == m_bottomPanel ||
                         control == m_hintLabel || control == m_pressEnterCheck;
    const COLORREF text =
        m_darkTheme ? (isHint ? kTextSecondary : kTextPrimary)
                    : (isHint ? kLightTextSecondary : kLightTextPrimary);
    const COLORREF background =
        m_darkTheme ? (onPanel ? kPanelBg : kWindowBg)
                    : (onPanel ? kLightPanelBg : kLightWindowBg);
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

    const HPEN border =
        ::CreatePen(PS_SOLID, 1, m_darkTheme ? kButtonBorder : kLightButtonBorder);
    const HGDIOBJ oldPen = ::SelectObject(item.hDC, border);
    const HGDIOBJ oldBrush = ::SelectObject(item.hDC, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    ::SelectObject(item.hDC, oldBrush);
    ::SelectObject(item.hDC, oldPen);
    ::DeleteObject(border);

    ::SetBkMode(item.hDC, TRANSPARENT);
    ::SetTextColor(item.hDC,
                   m_darkTheme ? (disabled ? kTextSecondary : kTextPrimary)
                               : (disabled ? kLightTextSecondary : kLightTextPrimary));

    RECT contentRc = item.rcItem;
    if (pressed) ::OffsetRect(&contentRc, 1, 1);

    // Right-panel icon buttons use Segoe MDL2 Assets glyphs.
    const wchar_t* iconGlyph = nullptr;
    if (item.hwndItem == m_saveButton) {
        iconGlyph = L"\uE74E";  // Save (disk)
    } else if (item.hwndItem == m_clearButton) {
        iconGlyph = L"\uE74D";  // Delete / clear
    } else if (item.hwndItem == m_settingsButton) {
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

void MainWindow::DrawSelectedWindowIcon(const DRAWITEMSTRUCT& item) const {
    ::FillRect(item.hDC, &item.rcItem,
               m_buttonBrush ? m_buttonBrush
                             : reinterpret_cast<HBRUSH>(::GetStockObject(DKGRAY_BRUSH)));

    const COLORREF borderColor =
        m_selectedIconRemoveDrag && m_selectedIconOutside
            ? kPickOutline
            : (m_darkTheme ? kButtonBorder : kLightButtonBorder);
    const HPEN border = ::CreatePen(PS_SOLID, 1, borderColor);
    const HGDIOBJ oldPen = ::SelectObject(item.hDC, border);
    const HGDIOBJ oldBrush = ::SelectObject(item.hDC, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    ::SelectObject(item.hDC, oldBrush);
    ::SelectObject(item.hDC, oldPen);
    ::DeleteObject(border);

    if (!m_pickedWindowIcon) return;
    const int iconSize = std::max(Scaled(20), 16);
    const int x = item.rcItem.left + ((item.rcItem.right - item.rcItem.left) - iconSize) / 2;
    const int y = item.rcItem.top + ((item.rcItem.bottom - item.rcItem.top) - iconSize) / 2;
    ::DrawIconEx(item.hDC, x, y, m_pickedWindowIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
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

    // Right panel: picker at the top, selected target beneath it, settings at the foot.
    const int buttonSize = Scaled(kRightButtonSize);
    const int buttonLeft = columnWidth + std::max((rightPanelWidth - buttonSize) / 2, 0);
    if (m_pickWindowButton) {
        ::SetWindowPos(m_pickWindowButton, nullptr, buttonLeft, pad, buttonSize, buttonSize,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (m_selectedWindowIconView) {
        ::SetWindowPos(m_selectedWindowIconView, nullptr, buttonLeft, pad + buttonSize + gap,
                       buttonSize, buttonSize, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    int rightButtonTop = std::max(clientHeight - pad - buttonSize, 0);
    if (m_settingsButton) {
        ::SetWindowPos(m_settingsButton, nullptr, buttonLeft, rightButtonTop, buttonSize,
                       buttonSize, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    rightButtonTop -= buttonSize + gap;
    if (m_clearButton) {
        ::SetWindowPos(m_clearButton, nullptr, buttonLeft, rightButtonTop, buttonSize, buttonSize,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    rightButtonTop -= buttonSize + gap;
    if (m_saveButton) {
        ::SetWindowPos(m_saveButton, nullptr, buttonLeft, rightButtonTop, buttonSize, buttonSize,
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
    if (m_hotkeySendPending) {
        ::KillTimer(m_hwnd, kHotkeySendTimerId);
        m_hotkeySendPending = false;
    }
    if (m_hotkeyRegistered) {
        ::UnregisterHotKey(m_hwnd, HOTKEY_SEND);
        m_hotkeyRegistered = false;
    }

    const unsigned key = m_settings.hotkeyVirtualKey;
    if (key != 0) {
        m_hotkeyRegistered =
            ::RegisterHotKey(m_hwnd, HOTKEY_SEND, m_settings.hotkeyModifiers | MOD_NOREPEAT, key) !=
            FALSE;
    }
    UpdateWindowTitle();
    if (!m_hotkeyRegistered) SetStatus(L"The Send hotkey is already in use by another app.");
}

void MainWindow::QueueHotkeySend() {
    if (m_hotkeySendPending) return;
    if (HotkeyChordReleased()) {
        OnSend();
        return;
    }

    m_hotkeySendPending = true;
    if (::SetTimer(m_hwnd, kHotkeySendTimerId, kHotkeySendPollMs, nullptr) == 0) {
        m_hotkeySendPending = false;
        SetStatus(L"Could not schedule the Send hotkey action.");
    }
}

bool MainWindow::HotkeyChordReleased() const {
    const auto released = [](int virtualKey) {
        return (::GetAsyncKeyState(virtualKey) & 0x8000) == 0;
    };

    if (m_settings.hotkeyVirtualKey != 0 &&
        !released(static_cast<int>(m_settings.hotkeyVirtualKey))) {
        return false;
    }
    const unsigned modifiers = m_settings.hotkeyModifiers;
    if ((modifiers & MOD_SHIFT) != 0 && !released(VK_SHIFT)) return false;
    if ((modifiers & MOD_CONTROL) != 0 && !released(VK_CONTROL)) return false;
    if ((modifiers & MOD_ALT) != 0 && !released(VK_MENU)) return false;
    if ((modifiers & MOD_WIN) != 0 && (!released(VK_LWIN) || !released(VK_RWIN))) return false;
    return true;
}

void MainWindow::UpdateWindowTitle() {
    std::wstring title = kBaseTitle;
    title += L" (Hotkey - ";
    title += HotkeyName(m_settings.hotkeyModifiers, m_settings.hotkeyVirtualKey);
    title += L") : ";
    title += m_webInputPicker.SelectedWindow()
                 ? (m_webInputPicker.SelectedName().empty() ? L"Untitled target"
                                                            : m_webInputPicker.SelectedName())
                 : L"No target selected";
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

void MainWindow::OnCommand(int controlId, int notifyCode) {
    switch (controlId) {
        case IDC_BTN_SEND:
            OnSend();
            return;

        case IDC_BTN_SAVE:
            OnSave();
            return;

        case IDC_BTN_CLEAR:
            OnClear();
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
            if (m_selectedIconRemoveDrag) {
                FinishSelectedIconRemoval(false);
                return;
            }
            if (m_windowPickDrag) {
                FinishWindowPick(false);
                return;
            }
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
    if (!m_view.HasSelection()) {
        SetStatus(L"Select caption text before sending.");
        return;
    }
    if (!m_webInputPicker.SelectedWindow()) {
        SetStatus(L"Pick a web tab before sending.");
        return;
    }

    const bool pressEnter =
        m_pressEnterCheck &&
        ::SendMessageW(m_pressEnterCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    m_settings.pressEnter = pressEnter;

    std::wstring error;
    if (!m_webInputPicker.SendText(m_view.SelectedText(), pressEnter, error)) {
        SetStatus(error);
        return;
    }

    std::wstring status = pressEnter ? L"Sent selection to " : L"Inserted selection in ";
    status += m_webInputPicker.SelectedName();
    status += pressEnter ? L" and pressed Enter." : L".";
    SetStatus(status);
}

void MainWindow::OnSave() {
    const std::wstring text = m_view.FullText();
    if (text.empty()) {
        SetStatus(L"There are no captions to save.");
        return;
    }

    wchar_t path[32768] = L"captions.txt";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_hwnd;
    dialog.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetSaveFileNameW(&dialog)) return;

    const HANDLE file =
        ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetStatus(L"Could not create the selected caption file.");
        return;
    }

    const std::string utf8 = util::ToUtf8(text);
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    bool saved = ::WriteFile(file, bom, sizeof(bom), &written, nullptr) != FALSE &&
                 written == sizeof(bom);
    if (saved && !utf8.empty()) {
        saved = ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                            nullptr) != FALSE &&
                written == utf8.size();
    }
    ::CloseHandle(file);

    SetStatus(saved ? std::wstring(L"Saved captions to ") + path
                    : L"Could not finish writing the caption file.");
}

void MainWindow::OnClear() {
    DrainPendingPayloads();
    m_view.Clear();
    SetStatus(L"Caption pane cleared.");
}

void MainWindow::OnSettings() {
    const CaptionSourcePreference previousSource = m_settings.captionSource;
    if (!ShowSettingsDialog(m_hwnd, m_instance, m_settings)) return;

    m_settings.Save();
    ApplyTheme();
    UpdateHotkeyRegistration();

    if (previousSource != m_settings.captionSource) {
        m_engine.Stop();
        DrainPendingPayloads();
        m_view.Clear();
        m_engine.Start(m_hwnd, m_settings.ResolvedTranscriptPath(), m_settings.pollIntervalMs,
                       ToSourceChoice(m_settings.captionSource));
    }
    SetStatus(L"Settings saved.");
}

void MainWindow::OnPickWindow() {
    std::wstring status = L"Drag this button onto a browser tab that contains an editable field.";
    if (m_webInputPicker.SelectedWindow()) {
        status += L" Current tab: ";
        status += m_webInputPicker.SelectedName();
        status += L".";
    }
    SetStatus(status);
}

void MainWindow::BeginWindowPick() {
    if (m_windowPickDrag) return;
    HidePickOutline();
    m_webInputPicker.ResetCandidate();
    m_windowPickState = WebInputPickState::NoWindow;
    m_windowPickDrag = true;
    ::SendMessageW(m_pickWindowButton, BM_SETSTATE, TRUE, 0);
    ::SetCapture(m_pickWindowButton);
    ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
    SetStatus(L"Drag over a browser tab. Release when the cursor changes to a crosshair.");
}

void MainWindow::UpdateWindowPick(POINT screenPoint) {
    if (!m_windowPickDrag) return;
    m_windowPickState = m_webInputPicker.Inspect(screenPoint, m_hwnd);
    const LPCWSTR cursor = m_windowPickState == WebInputPickState::Valid ? IDC_CROSS : IDC_NO;
    ::SetCursor(::LoadCursorW(nullptr, cursor));
    if (m_windowPickState == WebInputPickState::Valid) {
        UpdatePickOutline(m_webInputPicker.CandidateWindow());
    } else {
        HidePickOutline();
    }
    ShowWindowPickStatus(m_windowPickState);
}

void MainWindow::FinishWindowPick(bool accept) {
    if (!m_windowPickDrag) return;
    m_windowPickDrag = false;
    HidePickOutline();
    if (::GetCapture() == m_pickWindowButton) ::ReleaseCapture();
    ::SendMessageW(m_pickWindowButton, BM_SETSTATE, FALSE, 0);

    if (accept && m_windowPickState == WebInputPickState::Valid &&
        m_webInputPicker.CommitCandidate()) {
        UpdatePickedWindowIcon(m_webInputPicker.SelectedWindow());
        UpdateWindowTitle();

        std::wstring accessibleName = L"Selected window: ";
        accessibleName += m_webInputPicker.SelectedName();
        ::SetWindowTextW(m_selectedWindowIconView, accessibleName.c_str());
        ::ShowWindow(m_selectedWindowIconView, SW_SHOWNOACTIVATE);
        ::InvalidateRect(m_selectedWindowIconView, nullptr, TRUE);

        std::wstring status = L"Picked web tab: ";
        status += m_webInputPicker.SelectedName();
        status += L".";
        SetStatus(status);
    } else if (!accept) {
        SetStatus(L"Window picking cancelled.");
    } else {
        ShowWindowPickStatus(m_windowPickState);
    }

    m_webInputPicker.ResetCandidate();
}

void MainWindow::ShowWindowPickStatus(WebInputPickState state) {
    switch (state) {
        case WebInputPickState::Valid: {
            std::wstring status = L"Release to pick: ";
            status += m_webInputPicker.CandidateName();
            status += L".";
            SetStatus(status);
            return;
        }
        case WebInputPickState::OwnWindow:
            SetStatus(L"This app cannot be selected.");
            return;
        case WebInputPickState::NoWebDocument:
            SetStatus(L"Drop unavailable: this window does not expose a web tab.");
            return;
        case WebInputPickState::NoEditableInput:
            SetStatus(L"Drop unavailable: this web tab has no enabled editable input.");
            return;
        default:
            SetStatus(L"Drop unavailable: point at a visible browser or WebView window.");
            return;
    }
}

void MainWindow::UpdatePickedWindowIcon(HWND window) {
    HICON icon = CopyWindowIcon(window);
    if (!icon) return;
    if (m_pickedWindowIcon) ::DestroyIcon(m_pickedWindowIcon);
    m_pickedWindowIcon = icon;
}

void MainWindow::BeginSelectedIconRemoval() {
    if (m_selectedIconRemoveDrag || !m_webInputPicker.SelectedWindow()) return;
    m_selectedIconRemoveDrag = true;
    m_selectedIconOutside = false;
    ::SetCapture(m_selectedWindowIconView);

    POINT point{};
    if (::GetCursorPos(&point)) UpdateSelectedIconRemoval(point);
    SetStatus(L"Right-drag the selected icon outside this app and release to remove it.");
}

void MainWindow::UpdateSelectedIconRemoval(POINT screenPoint) {
    if (!m_selectedIconRemoveDrag) return;
    RECT appBounds{};
    const bool outside =
        !::GetWindowRect(m_hwnd, &appBounds) || !::PtInRect(&appBounds, screenPoint);
    if (outside != m_selectedIconOutside) {
        m_selectedIconOutside = outside;
        ::InvalidateRect(m_selectedWindowIconView, nullptr, TRUE);
        SetStatus(outside ? L"Release to remove the selected window."
                          : L"Drag outside this app to remove the selected window.");
    }
    ::SetCursor(::LoadCursorW(nullptr, outside ? IDC_HAND : IDC_SIZEALL));
}

void MainWindow::FinishSelectedIconRemoval(bool accept) {
    if (!m_selectedIconRemoveDrag) return;
    const bool remove = accept && m_selectedIconOutside;
    m_selectedIconRemoveDrag = false;
    m_selectedIconOutside = false;
    if (::GetCapture() == m_selectedWindowIconView) ::ReleaseCapture();
    ::InvalidateRect(m_selectedWindowIconView, nullptr, TRUE);

    if (remove) {
        ClearPickedWindow();
        SetStatus(L"Selected window removed.");
    } else {
        SetStatus(L"Selected-window removal cancelled.");
    }
}

void MainWindow::ClearPickedWindow() {
    m_webInputPicker.ClearSelected();
    UpdateWindowTitle();
    if (m_pickedWindowIcon) {
        ::DestroyIcon(m_pickedWindowIcon);
        m_pickedWindowIcon = nullptr;
    }
    if (m_selectedWindowIconView) {
        ::SetWindowTextW(m_selectedWindowIconView, L"");
        ::ShowWindow(m_selectedWindowIconView, SW_HIDE);
    }
}

void MainWindow::UpdatePickOutline(HWND window) {
    if (!m_pickOutlineWindow || !window || !::IsWindow(window)) {
        HidePickOutline();
        return;
    }

    RECT bounds{};
    if (FAILED(::DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds,
                                       sizeof(bounds))) ||
        ::IsRectEmpty(&bounds)) {
        if (!::GetWindowRect(window, &bounds) || ::IsRectEmpty(&bounds)) {
            HidePickOutline();
            return;
        }
    }

    UINT targetDpi = ::GetDpiForWindow(window);
    if (targetDpi == 0) targetDpi = 96;
    const int thickness = std::max(::MulDiv(3, static_cast<int>(targetDpi), 96), 3);
    ::InflateRect(&bounds, thickness, thickness);

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        HidePickOutline();
        return;
    }

    HRGN borderRegion = ::CreateRectRgn(0, 0, width, height);
    if (!borderRegion) {
        HidePickOutline();
        return;
    }
    if (width > thickness * 2 && height > thickness * 2) {
        const HRGN interior =
            ::CreateRectRgn(thickness, thickness, width - thickness, height - thickness);
        if (interior) {
            ::CombineRgn(borderRegion, borderRegion, interior, RGN_DIFF);
            ::DeleteObject(interior);
        }
    }

    // SetWindowRgn takes ownership only when it succeeds.
    if (::SetWindowRgn(m_pickOutlineWindow, borderRegion, FALSE) == 0) {
        ::DeleteObject(borderRegion);
        HidePickOutline();
        return;
    }
    ::SetWindowPos(m_pickOutlineWindow, HWND_TOPMOST, bounds.left, bounds.top, width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::RedrawWindow(m_pickOutlineWindow, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

void MainWindow::HidePickOutline() {
    if (m_pickOutlineWindow && ::IsWindowVisible(m_pickOutlineWindow)) {
        ::ShowWindow(m_pickOutlineWindow, SW_HIDE);
    }
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
