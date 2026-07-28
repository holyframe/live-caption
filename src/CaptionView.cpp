#include "CaptionView.h"

#include <windowsx.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "resource.h"

namespace {

constexpr const wchar_t* kClassName = L"LiveCaptionViewPane";

// Colours sampled to match the dark caption pane in the reference design.
constexpr D2D1_COLOR_F kBackground = {43.0f / 255.0f, 43.0f / 255.0f, 43.0f / 255.0f, 1.0f};
constexpr D2D1_COLOR_F kForeground = {228.0f / 255.0f, 228.0f / 255.0f, 228.0f / 255.0f, 1.0f};
constexpr D2D1_COLOR_F kSelection  = {88.0f / 255.0f, 88.0f / 255.0f, 88.0f / 255.0f, 1.0f};
constexpr D2D1_COLOR_F kGutterHover = {55.0f / 255.0f, 55.0f / 255.0f, 55.0f / 255.0f, 1.0f};

// Left strip is the line-select gutter; a modest right margin keeps glyphs off
// the scrollbar. The control-button panel lives outside this pane, to the right
// of the scrollbar.
constexpr float kGutterDip = 75.0f;
constexpr float kMarginRightDip = 14.0f;
constexpr float kMarginYDip = 10.0f;

// Drives the scroll while a selection is dragged past the top or bottom edge.
constexpr UINT_PTR kAutoScrollTimerId = 1;
constexpr UINT     kAutoScrollIntervalMs = 40;
constexpr float    kAutoScrollMaxStepDip = 48.0f;

enum ContextMenuCommand : UINT {
    kMenuCopy = 1,
    kMenuSelectAll,
    kMenuClearSelection,
};

bool IsWordCharacter(wchar_t ch) {
    return std::iswalnum(static_cast<wint_t>(ch)) != 0 || ch == L'_';
}

}  // namespace

bool CaptionView::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &CaptionView::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // Direct2D paints every pixel
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool CaptionView::Create(HWND parent, HINSTANCE instance, int controlId) {
    m_instance = instance;
    m_hwnd = ::CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 100,
                               100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                               instance, this);
    if (!m_hwnd) return false;

    m_dpi = ::GetDpiForWindow(m_hwnd);
    if (m_dpi == 0) m_dpi = 96;
    // DarkMode_Explorer paints the WS_VSCROLL bar to match the dark pane;
    // without it the system keeps the light classic thumb.
    ::SetWindowTheme(m_hwnd, L"DarkMode_Explorer", nullptr);
    return EnsureDeviceResources();
}

LRESULT CALLBACK CaptionView::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<CaptionView*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = hwnd;
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<CaptionView*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, message, wParam, lParam);
    return self->WndProc(message, wParam, lParam);
}

LRESULT CaptionView::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_ERASEBKGND:
            return 1;  // avoid a flash of the default brush

        case WM_SIZE:
            OnSize();
            return 0;

        case WM_DPICHANGED_AFTERPARENT:
            m_dpi = ::GetDpiForWindow(m_hwnd);
            if (m_dpi == 0) m_dpi = 96;
            m_textFormat.Reset();
            InvalidateAllLayouts();
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            UINT linesPerNotch = 3;
            ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
            if (linesPerNotch == 0) linesPerNotch = 3;
            ScrollByLines(-(delta / WHEEL_DELTA) * static_cast<int>(linesPerNotch));
            return 0;
        }

        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            ::GetScrollInfo(m_hwnd, SB_VERT, &si);
            const int page = static_cast<int>(si.nPage);
            int pos = si.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP:        pos -= 20; break;
                case SB_LINEDOWN:      pos += 20; break;
                case SB_PAGEUP:        pos -= page; break;
                case SB_PAGEDOWN:      pos += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP:           pos = si.nMin; break;
                case SB_BOTTOM:        pos = si.nMax; break;
                default: return 0;
            }
            SetScrollPos(static_cast<float>(pos), true);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            ::SetFocus(m_hwnd);
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            BeginDrag(pt, (wParam & MK_SHIFT) != 0);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (m_dragKind != DragKind::None) {
                m_dragPoint = pt;
                UpdateDrag(pt);
                UpdateAutoScroll();
            } else {
                UpdateHover(pt);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            m_trackingLeave = false;
            if (m_hoverValid) {
                m_hoverValid = false;
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            EndDrag();
            return 0;

        case WM_CAPTURECHANGED:
            // Someone else took the mouse; abandon the drag but keep what is
            // already selected.
            m_dragKind = DragKind::None;
            StopAutoScroll();
            return 0;

        case WM_TIMER:
            if (wParam == kAutoScrollTimerId) {
                UpdateAutoScroll();
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ::ScreenToClient(m_hwnd, &pt);
                ::SetCursor(::LoadCursorW(nullptr, InGutter(pt) ? IDC_ARROW : IDC_IBEAM));
                return TRUE;
            }
            break;

        case WM_LBUTTONDBLCLK: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (InGutter(pt)) {
                // A second click in the gutter is still a line select, never Send.
                EndDrag();
                size_t line = 0;
                if (LineIndexFromPoint(pt, &line)) SelectLinesTo(line, false);
                return 0;
            }
            TextPos position;
            bool insideText = false;
            if (PositionFromPoint(pt, &position, &insideText) && insideText) {
                // Drop the drag the preceding WM_LBUTTONDOWN started, so a
                // twitch before the button comes up cannot collapse the word
                // back to a caret.
                EndDrag();
                SelectWordAt(position);
                return 0;
            }
            // "Double-click empty area to send" from the reference design; on a
            // word the gesture selects instead, as it does everywhere else.
            ::SendMessageW(::GetParent(m_hwnd), WM_COMMAND,
                           MAKEWPARAM(IDC_BTN_SEND, BN_CLICKED), 0);
            return 0;
        }

        case WM_RBUTTONUP: {
            ::SetFocus(m_hwnd);
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ClientToScreen(m_hwnd, &pt);
            ShowContextMenu(pt);
            return 0;
        }

        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.x == -1 && pt.y == -1) {  // menu key rather than the mouse
                RECT rc{};
                ::GetClientRect(m_hwnd, &rc);
                pt = POINT{rc.left + (rc.right - rc.left) / 2, rc.top + (rc.bottom - rc.top) / 2};
                ::ClientToScreen(m_hwnd, &pt);
            }
            ShowContextMenu(pt);
            return 0;
        }

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        case WM_KEYDOWN:
            if (::GetKeyState(VK_CONTROL) < 0) {
                switch (wParam) {
                    case 'A': SelectAll();   return 0;
                    case 'C': RequestCopy(); return 0;
                    case VK_INSERT: RequestCopy(); return 0;
                    default: break;
                }
            }
            switch (wParam) {
                case VK_ESCAPE: ClearSelection(); return 0;
                case VK_UP:   ScrollByLines(-1); return 0;
                case VK_DOWN: ScrollByLines(1);  return 0;
                case VK_PRIOR: {
                    RECT rc{};
                    ::GetClientRect(m_hwnd, &rc);
                    SetScrollPos(m_scrollY - static_cast<float>(rc.bottom - rc.top), true);
                    return 0;
                }
                case VK_NEXT: {
                    RECT rc{};
                    ::GetClientRect(m_hwnd, &rc);
                    SetScrollPos(m_scrollY + static_cast<float>(rc.bottom - rc.top), true);
                    return 0;
                }
                case VK_HOME: SetScrollPos(0.0f, true); return 0;
                case VK_END:  ScrollToBottom(); return 0;
                default: break;
            }
            break;

        case WM_DESTROY:
            StopAutoScroll();
            DiscardDeviceResources();
            break;

        default:
            break;
    }
    return ::DefWindowProcW(m_hwnd, message, wParam, lParam);
}

bool CaptionView::EnsureDeviceResources() {
    if (!m_d2dFactory) {
        if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       m_d2dFactory.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dwriteFactory) {
        if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(
                                             m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }
    if (!m_renderTarget) {
        RECT rc{};
        ::GetClientRect(m_hwnd, &rc);
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 1)),
                                             static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 1)));
        // PRESENT_OPTIONS_IMMEDIATELY matters more than it looks. The default
        // makes EndDraw block until the next vertical blank, measured at
        // 17.7 ms per repaint against 0.6 ms here (tests\render_probe.bat).
        // Captions revise faster than the refresh rate, so a vblank-locked
        // present cannot keep up and updates pile into the message queue.
        if (FAILED(m_d2dFactory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(m_hwnd, size,
                                                 D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                m_renderTarget.ReleaseAndGetAddressOf()))) {
            return false;
        }
        // Work in device pixels so scroll offsets and hit-testing stay integral;
        // DPI is applied to the font size instead.
        m_renderTarget->SetDpi(96.0f, 96.0f);
        m_textBrush.Reset();
        m_selectionBrush.Reset();
        m_hoverBrush.Reset();
    }
    if (!m_textBrush) {
        if (FAILED(m_renderTarget->CreateSolidColorBrush(kForeground,
                                                         m_textBrush.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    if (!m_selectionBrush) {
        if (FAILED(m_renderTarget->CreateSolidColorBrush(kSelection,
                                                         m_selectionBrush.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    if (!m_hoverBrush) {
        if (FAILED(m_renderTarget->CreateSolidColorBrush(kGutterHover,
                                                         m_hoverBrush.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    return EnsureTextFormat();
}

void CaptionView::DiscardDeviceResources() {
    InvalidateAllLayouts();
    m_textFormat.Reset();
    m_textBrush.Reset();
    m_selectionBrush.Reset();
    m_hoverBrush.Reset();
    m_renderTarget.Reset();
}

bool CaptionView::EnsureTextFormat() {
    if (m_textFormat) return true;
    if (!m_dwriteFactory) return false;

    // Points -> device pixels: pt * (96/72) * (dpi/96) == pt * dpi / 72.
    const float sizePx = static_cast<float>(m_fontSizePt) * static_cast<float>(m_dpi) / 72.0f;

    if (FAILED(m_dwriteFactory->CreateTextFormat(m_fontFamily.c_str(), nullptr,
                                                 DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, sizePx, L"",
                                                 m_textFormat.ReleaseAndGetAddressOf()))) {
        return false;
    }

    m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // UNIFORM spacing is supported on every target OS, unlike PROPORTIONAL.
    const float lineHeight = sizePx * 1.2f * static_cast<float>(m_lineSpacing);
    m_textFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight,
                                 lineHeight * 0.8f);
    return true;
}

void CaptionView::InvalidateAllLayouts() {
    for (Line& line : m_lines) {
        line.layout.Reset();
        line.height = -1.0f;
    }
    m_totalHeight = -1.0f;
}

float CaptionView::GutterWidth() const {
    return kGutterDip * static_cast<float>(m_dpi) / 96.0f;
}

float CaptionView::RightMargin() const {
    return kMarginRightDip * static_cast<float>(m_dpi) / 96.0f;
}

float CaptionView::LayoutWidth() const {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const float width =
        static_cast<float>(rc.right - rc.left) - GutterWidth() - RightMargin();
    return std::max(width, 32.0f);
}

IDWriteTextLayout* CaptionView::EnsureLayout(size_t index) {
    if (index >= m_lines.size()) return nullptr;
    Line& line = m_lines[index];
    if (line.layout) return line.layout.Get();
    if (!EnsureTextFormat()) return nullptr;

    if (FAILED(m_dwriteFactory->CreateTextLayout(
            line.text.c_str(), static_cast<UINT32>(line.text.size()), m_textFormat.Get(),
            LayoutWidth(), 4096.0f, line.layout.ReleaseAndGetAddressOf()))) {
        return nullptr;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(line.layout->GetMetrics(&metrics))) {
        line.height = std::max(metrics.height, 1.0f);
    }
    return line.layout.Get();
}

void CaptionView::EnsureHeight(size_t index) {
    if (index >= m_lines.size()) return;
    if (m_lines[index].height >= 0.0f) return;
    EnsureLayout(index);
    // Measured; the layout itself is released later if it is off screen.
}

float CaptionView::TotalHeight() {
    if (m_totalHeight >= 0.0f) return m_totalHeight;
    float total = 0.0f;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        EnsureHeight(i);
        total += std::max(m_lines[i].height, 0.0f);
    }
    const float scale = static_cast<float>(m_dpi) / 96.0f;
    m_totalHeight = total + 2.0f * kMarginYDip * scale;
    return m_totalHeight;
}

void CaptionView::ReleaseLayoutsOutside(size_t firstVisible, size_t lastVisible) {
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (i < firstVisible || i > lastVisible) m_lines[i].layout.Reset();
    }
}

void CaptionView::QueueUpdate(size_t firstDirtyLine, std::vector<std::wstring> lines) {
    const bool growEnd = FarEndAtTranscriptEnd();

    // Translate the transcript-absolute index into our trimmed window.
    size_t local = 0;
    if (firstDirtyLine >= m_lineOffset) {
        local = firstDirtyLine - m_lineOffset;
        if (local > m_lines.size()) local = m_lines.size();
    } else {
        // The dirty region predates what we still hold; rebuild everything.
        m_lines.clear();
        m_lineOffset = firstDirtyLine;
        local = 0;
    }

    m_lines.resize(local);
    m_lines.reserve(local + lines.size());
    for (std::wstring& text : lines) {
        Line line;
        line.text = std::move(text);
        m_lines.push_back(std::move(line));
    }

    if (m_lines.size() > kMaxLines) {
        const size_t drop = m_lines.size() - kMaxLines;
        m_lines.erase(m_lines.begin(), m_lines.begin() + static_cast<ptrdiff_t>(drop));
        m_lineOffset += drop;
    }

    m_totalHeight = -1.0f;
    // A revised or trimmed line can leave an endpoint pointing past the end of
    // its text; pull it back in rather than dropping the selection outright, so
    // a selection made while the speaker is still talking survives.
    ClampSelection();
    GrowSelectionEndIfNeeded(growEnd);
}

void CaptionView::RefreshAfterContentChange() {
    if (m_stickToBottom) {
        ScrollToBottom();
    } else {
        UpdateScrollBar();
    }
    // Unconditional, and deliberately not left to SetScrollPos: that skips its
    // redraw when the offset is unchanged, which is exactly what happens when
    // the recogniser revises the last line in place without changing its height.
    // Relying on it meant a revised line could sit unrepainted indefinitely.
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::Present() {
    RefreshAfterContentChange();
    // Paint now rather than when the message queue next runs dry. WM_PAINT is
    // the lowest-priority message there is, so leaving it to be delivered
    // normally can hold a finished caption off the screen for another frame.
    ::UpdateWindow(m_hwnd);
}

void CaptionView::Clear() {
    m_lines.clear();
    m_lineOffset = 0;
    m_totalHeight = -1.0f;
    m_scrollY = 0.0f;
    m_stickToBottom = true;
    ResetSelection();
    m_hoverValid = false;
    UpdateScrollBar();
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::SetTypography(const std::wstring& fontFamily, int fontSizePt, double lineSpacing) {
    m_fontFamily = fontFamily;
    m_fontSizePt = fontSizePt;
    m_lineSpacing = lineSpacing;
    m_textFormat.Reset();
    InvalidateAllLayouts();
    RefreshAfterContentChange();
}

void CaptionView::ScrollByLines(int lines) {
    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const float step = static_cast<float>(m_fontSizePt) * scale * 1.6f;
    SetScrollPos(m_scrollY + step * static_cast<float>(lines), true);
}

void CaptionView::ScrollToBottom() {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const float viewport = static_cast<float>(rc.bottom - rc.top);
    SetScrollPos(std::max(TotalHeight() - viewport, 0.0f), true);
    m_stickToBottom = true;
}

void CaptionView::SetScrollPos(float y, bool redraw) {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const float viewport = static_cast<float>(rc.bottom - rc.top);
    const float maxScroll = std::max(TotalHeight() - viewport, 0.0f);

    const float clamped = std::clamp(y, 0.0f, maxScroll);
    // Re-enable auto-follow only when parked at the very bottom.
    m_stickToBottom = clamped >= maxScroll - 1.0f;

    if (std::fabs(clamped - m_scrollY) < 0.5f) {
        UpdateScrollBar();
        return;
    }
    m_scrollY = clamped;
    UpdateScrollBar();
    if (redraw) ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::UpdateScrollBar() {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const int viewport = std::max<int>(rc.bottom - rc.top, 1);
    const int total = static_cast<int>(std::ceil(TotalHeight()));

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = std::max(total - 1, 0);
    si.nPage = static_cast<UINT>(viewport);
    si.nPos = static_cast<int>(m_scrollY);
    ::SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
}

void CaptionView::OnSize() {
    if (m_renderTarget) {
        RECT rc{};
        ::GetClientRect(m_hwnd, &rc);
        m_renderTarget->Resize(D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(rc.right - rc.left, 1)),
                                           static_cast<UINT32>(std::max<LONG>(rc.bottom - rc.top, 1))));
    }
    // Wrapping width changed, so every measurement is stale.
    InvalidateAllLayouts();
    if (m_stickToBottom) {
        ScrollToBottom();
    } else {
        SetScrollPos(m_scrollY, false);
    }
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::OnPaint() {
    PAINTSTRUCT ps{};
    ::BeginPaint(m_hwnd, &ps);

    if (EnsureDeviceResources() && m_renderTarget) {
        RECT rc{};
        ::GetClientRect(m_hwnd, &rc);
        const float viewport = static_cast<float>(rc.bottom - rc.top);
        const float scale = static_cast<float>(m_dpi) / 96.0f;

        m_renderTarget->BeginDraw();
        m_renderTarget->Clear(kBackground);

        const float originX = GutterWidth();
        const float paintWidth = static_cast<float>(rc.right - rc.left);
        float y = kMarginYDip * scale - m_scrollY;

        size_t firstVisible = m_lines.size();
        size_t lastVisible = 0;

        const bool drawSelection = HasSelection();
        TextPos selectionStart;
        TextPos selectionEnd;
        if (drawSelection) OrderedSelection(&selectionStart, &selectionEnd);

        for (size_t i = 0; i < m_lines.size(); ++i) {
            EnsureHeight(i);
            const float height = std::max(m_lines[i].height, 0.0f);

            if (y + height >= 0.0f && y <= viewport) {
                if (m_hoverValid && m_lineOffset + i == m_hoverLine) {
                    DrawGutterHover(i, y, paintWidth);
                }
                if (drawSelection) DrawLineSelection(i, originX, y, selectionStart, selectionEnd);
                if (IDWriteTextLayout* layout = EnsureLayout(i)) {
                    m_renderTarget->DrawTextLayout(D2D1::Point2F(originX, y), layout,
                                                   m_textBrush.Get(),
                                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
                }
                firstVisible = std::min(firstVisible, i);
                lastVisible = std::max(lastVisible, i);
            }
            y += height;
            if (y > viewport) {
                // Remaining lines are below the fold; their heights are already
                // accounted for by TotalHeight().
                break;
            }
        }

        const HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
        } else if (firstVisible <= lastVisible) {
            ReleaseLayoutsOutside(firstVisible, lastVisible);
        }
    }

    ::EndPaint(m_hwnd, &ps);
}

bool CaptionView::InGutter(POINT client) const {
    return client.x < static_cast<LONG>(GutterWidth());
}

bool CaptionView::LineIndexFromPoint(POINT client, size_t* absoluteLine) {
    if (!absoluteLine || m_lines.empty()) return false;

    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const float pointY = static_cast<float>(client.y);

    float top = kMarginYDip * scale - m_scrollY;
    size_t index = m_lines.size() - 1;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        EnsureHeight(i);
        const float height = std::max(m_lines[i].height, 0.0f);
        if (pointY < top + height || i + 1 == m_lines.size()) {
            index = i;
            break;
        }
        top += height;
    }

    *absoluteLine = m_lineOffset + index;
    return true;
}

bool CaptionView::PositionFromPoint(POINT client, TextPos* position, bool* insideText) {
    if (insideText) *insideText = false;
    if (!position || m_lines.empty()) return false;

    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const float originX = GutterWidth();
    const float pointY = static_cast<float>(client.y);

    // Walk down to the line under the pointer; a point above the first line or
    // below the last one clamps to that line, which is what a drag past either
    // edge needs.
    float top = kMarginYDip * scale - m_scrollY;
    size_t index = m_lines.size() - 1;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        EnsureHeight(i);
        const float height = std::max(m_lines[i].height, 0.0f);
        if (pointY < top + height || i + 1 == m_lines.size()) {
            index = i;
            break;
        }
        top += height;
    }

    const std::wstring& text = m_lines[index].text;
    position->line = m_lineOffset + index;
    position->offset = text.size();

    IDWriteTextLayout* layout = EnsureLayout(index);
    if (!layout) return false;

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(static_cast<float>(client.x) - originX, pointY - top, &trailing,
                                    &inside, &metrics))) {
        return false;
    }

    const size_t offset =
        static_cast<size_t>(metrics.textPosition) + (trailing ? static_cast<size_t>(metrics.length) : 0);
    position->offset = std::min(offset, text.size());
    if (insideText) *insideText = inside != FALSE;
    return true;
}

CaptionView::TextPos CaptionView::TranscriptEnd() const {
    if (m_lines.empty()) return {};
    return TextPos{m_lineOffset + m_lines.size() - 1, m_lines.back().text.size()};
}

CaptionView::TextPos CaptionView::LineEndExclusive(size_t absoluteLine) const {
    if (m_lines.empty()) return {};
    const size_t last = m_lineOffset + m_lines.size() - 1;
    if (absoluteLine >= last) {
        return TextPos{last, m_lines.back().text.size()};
    }
    return TextPos{absoluteLine + 1, 0};
}

bool CaptionView::FarEndAtTranscriptEnd() const {
    if (!HasSelection() || m_lines.empty()) return false;
    TextPos start;
    TextPos end;
    OrderedSelection(&start, &end);
    return end == TranscriptEnd();
}

bool CaptionView::HasSelection() const {
    return m_selectionValid && !(m_selectionAnchor == m_selectionCaret);
}

void CaptionView::OrderedSelection(TextPos* start, TextPos* end) const {
    const bool forward = !(m_selectionCaret < m_selectionAnchor);
    *start = forward ? m_selectionAnchor : m_selectionCaret;
    *end = forward ? m_selectionCaret : m_selectionAnchor;
}

void CaptionView::ResetSelection() {
    m_selectionValid = false;
    m_lineSelectionActive = false;
    m_selectionAnchor = TextPos{};
    m_selectionCaret = TextPos{};
}

void CaptionView::ClearSelection() {
    const bool had = HasSelection();
    ResetSelection();
    if (had) ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::ClampSelection() {
    if (!m_selectionValid) return;
    if (m_lines.empty()) {
        ResetSelection();
        return;
    }

    const size_t lastLine = m_lineOffset + m_lines.size() - 1;
    const auto clamp = [&](TextPos& position) {
        if (position.line < m_lineOffset) {
            position.line = m_lineOffset;
            position.offset = 0;
        } else if (position.line > lastLine) {
            position.line = lastLine;
            position.offset = m_lines.back().text.size();
        }
        const std::wstring& text = m_lines[position.line - m_lineOffset].text;
        position.offset = std::min(position.offset, text.size());
    };
    clamp(m_selectionAnchor);
    clamp(m_selectionCaret);

    if (m_selectionAnchor == m_selectionCaret) ResetSelection();
}

void CaptionView::GrowSelectionEndIfNeeded(bool wasAtEnd) {
    if (!wasAtEnd || !m_selectionValid || m_lines.empty()) return;

    const TextPos newEnd = TranscriptEnd();
    if (m_selectionCaret < m_selectionAnchor) {
        m_selectionAnchor = newEnd;
    } else {
        m_selectionCaret = newEnd;
    }
}

void CaptionView::MoveSelectionTo(const TextPos& caret, bool keepAnchor) {
    const TextPos previousAnchor = m_selectionAnchor;
    const TextPos previousCaret = m_selectionCaret;
    const bool wasValid = m_selectionValid;

    if (!keepAnchor || !m_selectionValid) m_selectionAnchor = caret;
    m_selectionCaret = caret;
    m_selectionValid = true;
    m_lineSelectionActive = false;

    // Mouse moves arrive far more often than the selection actually changes.
    if (wasValid && previousAnchor == m_selectionAnchor && previousCaret == m_selectionCaret) return;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::SelectWordAt(const TextPos& position) {
    if (position.line < m_lineOffset) return;
    const size_t index = position.line - m_lineOffset;
    if (index >= m_lines.size()) return;

    const std::wstring& text = m_lines[index].text;
    if (text.empty()) return;

    size_t start = std::min(position.offset, text.size() - 1);
    size_t end = start;
    if (IsWordCharacter(text[start])) {
        while (start > 0 && IsWordCharacter(text[start - 1])) --start;
        while (end < text.size() && IsWordCharacter(text[end])) ++end;
    } else {
        end = start + 1;  // punctuation or space: take just that character
    }

    m_selectionAnchor = TextPos{position.line, start};
    m_selectionCaret = TextPos{position.line, end};
    m_selectionValid = true;
    m_lineSelectionActive = false;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::SelectLinesTo(size_t absoluteLine, bool keepAnchor) {
    if (m_lines.empty()) return;

    const size_t first = m_lineOffset;
    const size_t last = m_lineOffset + m_lines.size() - 1;
    absoluteLine = std::clamp(absoluteLine, first, last);

    if (!keepAnchor || !m_selectionValid) {
        m_lineDragOrigin = absoluteLine;
        m_selectionAnchor = TextPos{absoluteLine, 0};
        m_selectionCaret = LineEndExclusive(absoluteLine);
        m_selectionValid = true;
        m_lineSelectionActive = true;
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Dragging up past the origin flips the anchor to the end of that origin
    // row so the row the gesture started on stays wholly selected — same as
    // VS Code's line-number gutter.
    if (absoluteLine >= m_lineDragOrigin) {
        m_selectionAnchor = TextPos{m_lineDragOrigin, 0};
        m_selectionCaret = LineEndExclusive(absoluteLine);
    } else {
        m_selectionAnchor = LineEndExclusive(m_lineDragOrigin);
        m_selectionCaret = TextPos{absoluteLine, 0};
    }
    m_selectionValid = true;
    m_lineSelectionActive = true;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::DrawGutterHover(size_t index, float y, float width) {
    if (!m_hoverBrush || index >= m_lines.size()) return;
    const float height = std::max(m_lines[index].height, 1.0f);
    m_renderTarget->FillRectangle(D2D1::RectF(0.0f, y, width, y + height), m_hoverBrush.Get());
}

void CaptionView::DrawLineSelection(size_t index, float originX, float y, const TextPos& start,
                                    const TextPos& end) {
    if (!m_selectionBrush || index >= m_lines.size()) return;

    const size_t absolute = m_lineOffset + index;
    if (absolute < start.line || absolute > end.line) return;

    const std::wstring& text = m_lines[index].text;
    const size_t from = absolute == start.line ? std::min(start.offset, text.size()) : 0;
    const size_t to = absolute == end.line ? std::min(end.offset, text.size()) : text.size();
    if (to < from) return;

    // A line the selection continues past ends in a break that has no glyph of
    // its own; a stub of highlight past the last character stands in for it so a
    // multi-line selection reads as one continuous run.
    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const float breakWidth =
        absolute < end.line ? static_cast<float>(m_fontSizePt) * scale * 0.45f : 0.0f;

    IDWriteTextLayout* layout = EnsureLayout(index);
    if (!layout) return;

    // One rectangle per wrapped row the range covers.
    std::vector<DWRITE_HIT_TEST_METRICS> runs;
    if (to > from) {
        const UINT32 first = static_cast<UINT32>(from);
        const UINT32 length = static_cast<UINT32>(to - from);
        UINT32 needed = 0;
        const HRESULT probe =
            layout->HitTestTextRange(first, length, originX, y, nullptr, 0, &needed);
        if (FAILED(probe) && probe != E_NOT_SUFFICIENT_BUFFER) return;
        if (needed > 0) {
            runs.resize(needed);
            UINT32 written = 0;
            if (FAILED(layout->HitTestTextRange(first, length, originX, y, runs.data(), needed,
                                                &written))) {
                return;
            }
            runs.resize(std::min<size_t>(written, runs.size()));
        }
    }

    if (runs.empty()) {
        if (breakWidth > 0.0f) {
            const float height = std::max(m_lines[index].height, 1.0f);
            m_renderTarget->FillRectangle(D2D1::RectF(originX, y, originX + breakWidth, y + height),
                                          m_selectionBrush.Get());
        }
        return;
    }

    for (size_t i = 0; i < runs.size(); ++i) {
        const DWRITE_HIT_TEST_METRICS& run = runs[i];
        float right = run.left + run.width;
        if (i + 1 == runs.size()) right += breakWidth;
        m_renderTarget->FillRectangle(D2D1::RectF(run.left, run.top, right, run.top + run.height),
                                      m_selectionBrush.Get());
    }
}

void CaptionView::SelectAll() {
    if (m_lines.empty()) return;
    m_selectionAnchor = TextPos{m_lineOffset, 0};
    m_selectionCaret = TextPos{m_lineOffset + m_lines.size() - 1, m_lines.back().text.size()};
    m_selectionValid = true;
    m_lineSelectionActive = true;
    m_lineDragOrigin = m_lineOffset;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

std::wstring CaptionView::SelectedText() const {
    if (!HasSelection()) return {};

    TextPos start;
    TextPos end;
    OrderedSelection(&start, &end);

    std::wstring out;
    for (size_t absolute = std::max(start.line, m_lineOffset); absolute <= end.line; ++absolute) {
        const size_t index = absolute - m_lineOffset;
        if (index >= m_lines.size()) break;

        const std::wstring& text = m_lines[index].text;
        const size_t from = absolute == start.line ? std::min(start.offset, text.size()) : 0;
        const size_t to = absolute == end.line ? std::min(end.offset, text.size()) : text.size();
        if (to > from) out.append(text, from, to - from);
        if (absolute < end.line) out += L"\r\n";
    }
    return out;
}

void CaptionView::BeginDrag(POINT client, bool keepAnchor) {
    if (m_lines.empty()) {
        ClearSelection();
        return;
    }

    m_dragPoint = client;
    ::SetCapture(m_hwnd);

    if (InGutter(client)) {
        m_dragKind = DragKind::Line;
        size_t line = 0;
        if (!LineIndexFromPoint(client, &line)) {
            EndDrag();
            ClearSelection();
            return;
        }
        if (!keepAnchor || !m_selectionValid) {
            m_lineDragOrigin = line;
        } else if (!m_lineSelectionActive) {
            // Extending a character selection by lines starts from the line of
            // the existing anchor; an existing line selection keeps its origin.
            m_lineDragOrigin = m_selectionAnchor.line;
        }
        SelectLinesTo(line, keepAnchor);
        return;
    }

    m_dragKind = DragKind::Text;
    TextPos position;
    if (!PositionFromPoint(client, &position, nullptr)) {
        EndDrag();
        ClearSelection();
        return;
    }
    MoveSelectionTo(position, keepAnchor);
}

void CaptionView::UpdateDrag(POINT client) {
    if (m_dragKind == DragKind::Line) {
        size_t line = 0;
        if (LineIndexFromPoint(client, &line)) SelectLinesTo(line, true);
        return;
    }
    if (m_dragKind == DragKind::Text) {
        TextPos position;
        if (PositionFromPoint(client, &position, nullptr)) MoveSelectionTo(position, true);
    }
}

void CaptionView::UpdateHover(POINT client) {
    if (!m_trackingLeave) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = m_hwnd;
        m_trackingLeave = ::TrackMouseEvent(&track) != FALSE;
    }

    bool valid = false;
    size_t line = 0;
    if (InGutter(client) && LineIndexFromPoint(client, &line)) {
        valid = true;
    }

    if (valid == m_hoverValid && (!valid || line == m_hoverLine)) return;
    m_hoverValid = valid;
    m_hoverLine = line;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CaptionView::EndDrag() {
    StopAutoScroll();
    m_dragKind = DragKind::None;
    if (::GetCapture() == m_hwnd) ::ReleaseCapture();
}

void CaptionView::UpdateAutoScroll() {
    if (m_dragKind == DragKind::None) {
        StopAutoScroll();
        return;
    }

    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    float overshoot = 0.0f;
    if (m_dragPoint.y < rc.top) {
        overshoot = static_cast<float>(m_dragPoint.y - rc.top);
    } else if (m_dragPoint.y > rc.bottom) {
        overshoot = static_cast<float>(m_dragPoint.y - rc.bottom);
    }
    if (overshoot == 0.0f) {
        StopAutoScroll();
        return;
    }

    // Keep scrolling while the pointer is held outside the pane, even though no
    // further mouse moves arrive once it stops moving.
    if (!m_autoScrolling) {
        m_autoScrolling =
            ::SetTimer(m_hwnd, kAutoScrollTimerId, kAutoScrollIntervalMs, nullptr) != 0;
    }

    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const float limit = kAutoScrollMaxStepDip * scale;
    SetScrollPos(m_scrollY + std::clamp(overshoot, -limit, limit), true);
    UpdateDrag(m_dragPoint);
}

void CaptionView::StopAutoScroll() {
    if (!m_autoScrolling) return;
    ::KillTimer(m_hwnd, kAutoScrollTimerId);
    m_autoScrolling = false;
}

void CaptionView::ShowContextMenu(POINT screen) {
    const HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    const bool hasSelection = HasSelection();
    ::AppendMenuW(menu, MF_STRING | (m_lines.empty() ? MF_GRAYED : 0), kMenuCopy,
                  hasSelection ? L"Copy selection\tCtrl+C" : L"Copy all\tCtrl+C");
    ::AppendMenuW(menu, MF_STRING | (m_lines.empty() ? MF_GRAYED : 0), kMenuSelectAll,
                  L"Select all\tCtrl+A");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), kMenuClearSelection,
                  L"Clear selection\tEsc");

    const int chosen = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        screen.x, screen.y, 0, m_hwnd, nullptr);
    ::DestroyMenu(menu);

    switch (chosen) {
        case kMenuCopy:           RequestCopy();    break;
        case kMenuSelectAll:      SelectAll();      break;
        case kMenuClearSelection: ClearSelection(); break;
        default: break;
    }
}

void CaptionView::RequestCopy() {
    // The parent owns the clipboard and the status bar, and decides between the
    // selection and the whole transcript.
    ::SendMessageW(::GetParent(m_hwnd), WM_COMMAND, MAKEWPARAM(IDC_BTN_COPY, BN_CLICKED), 0);
}

std::wstring CaptionView::FullText() const {
    size_t total = 0;
    for (const Line& line : m_lines) total += line.text.size() + 2;

    std::wstring out;
    out.reserve(total);
    for (const Line& line : m_lines) {
        out += line.text;
        out += L"\r\n";
    }
    return out;
}
