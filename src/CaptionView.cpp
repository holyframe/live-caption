#include "CaptionView.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "resource.h"

namespace {

constexpr const wchar_t* kClassName = L"LiveCaptionViewPane";

// Colours sampled to match the dark caption pane in the reference design.
constexpr D2D1_COLOR_F kBackground = {43.0f / 255.0f, 43.0f / 255.0f, 43.0f / 255.0f, 1.0f};
constexpr D2D1_COLOR_F kForeground = {228.0f / 255.0f, 228.0f / 255.0f, 228.0f / 255.0f, 1.0f};

constexpr float kMarginXDip = 14.0f;
constexpr float kMarginYDip = 10.0f;

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

        case WM_LBUTTONDOWN:
            ::SetFocus(m_hwnd);
            return 0;

        case WM_LBUTTONDBLCLK:
            // "Double-click empty area to send" from the reference design.
            ::SendMessageW(::GetParent(m_hwnd), WM_COMMAND,
                           MAKEWPARAM(IDC_BTN_SEND, BN_CLICKED), 0);
            return 0;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;

        case WM_KEYDOWN:
            switch (wParam) {
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
    }
    if (!m_textBrush) {
        if (FAILED(m_renderTarget->CreateSolidColorBrush(kForeground,
                                                         m_textBrush.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }
    return EnsureTextFormat();
}

void CaptionView::DiscardDeviceResources() {
    InvalidateAllLayouts();
    m_textFormat.Reset();
    m_textBrush.Reset();
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

float CaptionView::LayoutWidth() const {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const float width = static_cast<float>(rc.right - rc.left) - 2.0f * kMarginXDip * scale;
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

        const float originX = kMarginXDip * scale;
        float y = kMarginYDip * scale - m_scrollY;

        size_t firstVisible = m_lines.size();
        size_t lastVisible = 0;

        for (size_t i = 0; i < m_lines.size(); ++i) {
            EnsureHeight(i);
            const float height = std::max(m_lines[i].height, 0.0f);

            if (y + height >= 0.0f && y <= viewport) {
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
