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
//
// Text can be selected with the mouse the way it can in a code editor: drag
// through the text, or drag down the left padding to take whole lines.
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

    // Mouse- and keyboard-driven text selection.
    bool         HasSelection() const;
    std::wstring SelectedText() const;
    void         SelectAll();
    void         ClearSelection();
    // True while the selection runs all the way to the end of the transcript,
    // which is the state in which newly recognised text joins it.
    bool         SelectionFollowsTail() const;

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Line {
        std::wstring             text;
        ComPtr<IDWriteTextLayout> layout;  // present only while on screen
        float                    height = -1.0f;  // -1 means "not measured yet"
    };

    // A caret position: an absolute transcript line plus a UTF-16 offset into
    // it. Absolute so that a selection survives lines being trimmed off the
    // front of the window.
    struct TextPos {
        size_t line = 0;
        size_t offset = 0;

        bool operator==(const TextPos& other) const {
            return line == other.line && offset == other.offset;
        }
        bool operator<(const TextPos& other) const {
            return line != other.line ? line < other.line : offset < other.offset;
        }
    };

    // Which granularity the in-flight drag selects at. Fixed when the button
    // goes down, as in VS Code: a drag that starts in the gutter keeps taking
    // whole lines even once the pointer wanders over the text.
    enum class DragMode { None, Text, Line };

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

    float GutterWidth() const;
    // Finds the line under a client point. `insideContent` reports whether the
    // point was within the painted lines at all rather than clamped to the
    // nearest one, which is what separates a click on a line from a click on
    // the empty space below the transcript.
    size_t LineAtPoint(POINT client, float* lineTop, bool* insideContent);
    // Maps a client point to the nearest caret position. `insideText` reports
    // whether the point landed on a glyph rather than on empty pane space.
    bool  PositionFromPoint(POINT client, TextPos* position, bool* insideText);

    TextPos LineStart(size_t line) const;
    TextPos LineBreak(size_t line) const;  // start of the next line, or the end
    void  OrderedSelection(TextPos* start, TextPos* end) const;
    void  SetSelection(const TextPos& anchor, const TextPos& caret);
    void  ResetSelection();
    void  ClampSelection();
    void  ExtendSelectionToTail();
    void  SelectWordAt(const TextPos& position);
    void  SelectLinesFromAnchor(size_t line);
    void  DrawLineSelection(size_t index, float originX, float y, const TextPos& start,
                            const TextPos& end);

    void  BeginDrag(POINT client, bool keepAnchor);
    void  UpdateDragSelection();
    void  EndDrag();
    void  UpdateAutoScroll();
    void  StopAutoScroll();
    void  UpdateGutterHover(POINT client);
    void  ClearGutterHover();
    void  ShowContextMenu(POINT screen);
    void  RequestCopy();

    HWND       m_hwnd = nullptr;
    HINSTANCE  m_instance = nullptr;

    ComPtr<ID2D1Factory>          m_d2dFactory;
    ComPtr<IDWriteFactory>        m_dwriteFactory;
    ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    ComPtr<ID2D1SolidColorBrush>  m_textBrush;
    ComPtr<ID2D1SolidColorBrush>  m_selectionBrush;
    ComPtr<ID2D1SolidColorBrush>  m_gutterBrush;
    ComPtr<IDWriteTextFormat>     m_textFormat;

    std::vector<Line> m_lines;
    size_t            m_lineOffset = 0;  // absolute index of m_lines[0]
    float             m_totalHeight = -1.0f;
    float             m_scrollY = 0.0f;
    bool              m_stickToBottom = true;

    TextPos  m_selectionAnchor;
    TextPos  m_selectionCaret;
    bool     m_selectionValid = false;
    DragMode m_dragMode = DragMode::None;
    size_t   m_dragAnchorLine = 0;  // absolute, for a gutter drag
    POINT    m_dragPoint{};
    bool     m_autoScrolling = false;
    size_t   m_hoverLine = 0;  // gutter row under the pointer
    bool     m_hoverValid = false;
    bool     m_trackingLeave = false;

    std::wstring m_fontFamily = L"Segoe UI";
    int          m_fontSizePt = 12;
    double       m_lineSpacing = 1.3;
    UINT         m_dpi = 96;

    // Caps memory for multi-hour sessions; older lines stay in the transcript
    // file but scroll out of the viewer.
    static constexpr size_t kMaxLines = 3000;
};
