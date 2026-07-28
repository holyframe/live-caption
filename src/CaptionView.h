#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

// Dark, scrolling, word-wrapped caption pane rendered with Direct2D and
// DirectWrite so font family, point size and line spacing can change live.
//
// Only the lines that are actually on screen keep a DirectWrite layout object;
// every line caches just its measured height, which keeps memory flat over a
// long session while still allowing exact scroll extents.
class CaptionView {
public:
    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HWND parent, HINSTANCE instance, int controlId);
    HWND Handle() const { return m_hwnd; }

    // Replaces every line from `firstDirtyLine` (an absolute transcript index)
    // with `lines`. Touches only the model, so a burst of updates that arrives
    // faster than the screen can be repainted collapses into a single frame.
    void QueueUpdate(size_t firstDirtyLine, std::vector<std::wstring> lines);
    // Puts everything queued so far on screen, synchronously.
    void Present();
    void Clear();

    void SetTypography(const std::wstring& fontFamily, int fontSizePt, double lineSpacing);
    void ScrollByLines(int lines);
    void ScrollToBottom();

    std::wstring FullText() const;
    bool Empty() const { return m_lines.empty(); }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Line {
        std::wstring             text;
        ComPtr<IDWriteTextLayout> layout;  // present only while on screen
        float                    height = -1.0f;  // -1 means "not measured yet"
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    bool  EnsureDeviceResources();
    void  DiscardDeviceResources();
    bool  EnsureTextFormat();
    void  InvalidateAllLayouts();
    float LayoutWidth() const;
    void  EnsureHeight(size_t index);
    IDWriteTextLayout* EnsureLayout(size_t index);
    float TotalHeight();
    void  ReleaseLayoutsOutside(size_t firstVisible, size_t lastVisible);
    void  RefreshAfterContentChange();
    void  UpdateScrollBar();
    void  SetScrollPos(float y, bool redraw);
    void  OnPaint();
    void  OnSize();

    HWND       m_hwnd = nullptr;
    HINSTANCE  m_instance = nullptr;

    ComPtr<ID2D1Factory>          m_d2dFactory;
    ComPtr<IDWriteFactory>        m_dwriteFactory;
    ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    ComPtr<ID2D1SolidColorBrush>  m_textBrush;
    ComPtr<IDWriteTextFormat>     m_textFormat;

    std::vector<Line> m_lines;
    size_t            m_lineOffset = 0;  // absolute index of m_lines[0]
    float             m_totalHeight = -1.0f;
    float             m_scrollY = 0.0f;
    bool              m_stickToBottom = true;

    std::wstring m_fontFamily = L"Segoe UI";
    int          m_fontSizePt = 12;
    double       m_lineSpacing = 1.3;
    UINT         m_dpi = 96;

    // Caps memory for multi-hour sessions; older lines stay in the transcript
    // file but scroll out of the viewer.
    static constexpr size_t kMaxLines = 3000;
};
