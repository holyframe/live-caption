#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "CaptionView.h"
#include "CaptureEngine.h"
#include "Settings.h"
#include "WebInputPicker.h"

class MainWindow {
public:
    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance, int showCommand);
    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PickButtonSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                                   DWORD_PTR);
    static LRESULT CALLBACK SelectedIconSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                                     DWORD_PTR);
    static LRESULT CALLBACK PickOutlineWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateChildren();
    void PopulateFontCombo();
    void PopulateSizeCombo();
    void PopulateSpacingCombo();
    void ApplyControlFont();
    void ApplyDarkTheme();
    void EnsureThemeBrushes();
    void DiscardThemeBrushes();
    void DrawDarkButton(const DRAWITEMSTRUCT& item) const;
    void DrawSelectedWindowIcon(const DRAWITEMSTRUCT& item) const;
    LRESULT OnCtlColor(HDC dc, HWND control);
    void DrawSplitter(HDC dc) const;
    int  LogBarHeight() const;
    int  ClampBottomPanelHeight(int wanted, int clientHeight) const;
    void DragSplitterTo(int splitterTop);
    void Layout();
    void UpdateHotkeyRegistration();
    void SetStatus(const std::wstring& text);
    void ApplyTypography();
    void ApplyViewMode();
    void ToggleVisibility();

    void OnCommand(int controlId, int notifyCode);
    void OnSend();
    void OnSettings();
    void OnPickWindow();
    void BeginWindowPick();
    void UpdateWindowPick(POINT screenPoint);
    void FinishWindowPick(bool accept);
    void ShowWindowPickStatus(WebInputPickState state);
    void UpdatePickedWindowIcon(HWND window);
    void BeginSelectedIconRemoval();
    void UpdateSelectedIconRemoval(POINT screenPoint);
    void FinishSelectedIconRemoval(bool accept);
    void ClearPickedWindow();
    void UpdatePickOutline(HWND window);
    void HidePickOutline();
    void OnCopy();
    void OnCaptionUpdate(CaptionUpdate* update);
    void MaybeCopyRealtime();
    void DrainPendingPayloads();

    bool CopyTextToClipboard(const std::wstring& text);
    int  Scaled(int value) const;
    int  Unscaled(int value) const;

    HWND      m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    UINT      m_dpi = 96;
    HFONT     m_controlFont = nullptr;
    bool      m_hotkeyRegistered = false;

    HWND m_sendButton = nullptr;
    HWND m_pressEnterCheck = nullptr;
    HWND m_hintLabel = nullptr;
    HWND m_fontCombo = nullptr;
    HWND m_sizeCombo = nullptr;
    HWND m_spacingCombo = nullptr;
    HWND m_rightPanel = nullptr;
    HWND m_bottomPanel = nullptr;
    HWND m_settingsButton = nullptr;
    HWND m_pickWindowButton = nullptr;
    HWND m_selectedWindowIconView = nullptr;
    HWND m_statusBar = nullptr;
    HWND m_pickOutlineWindow = nullptr;
    HICON m_pickedWindowIcon = nullptr;

    WebInputPicker m_webInputPicker;
    bool m_windowPickDrag = false;
    WebInputPickState m_windowPickState = WebInputPickState::NoWindow;
    bool m_selectedIconRemoveDrag = false;
    bool m_selectedIconOutside = false;

    HBRUSH m_windowBrush = nullptr;
    HBRUSH m_panelBrush = nullptr;
    HBRUSH m_buttonBrush = nullptr;
    HBRUSH m_buttonPressedBrush = nullptr;
    HBRUSH m_splitterBrush = nullptr;

    // Grab strip between the caption pane and the bottom panel. Parent area, so
    // the drag is handled here rather than by a child control.
    RECT m_splitterRect{};
    bool m_splitterDrag = false;
    int  m_splitterGrab = 0;  // cursor offset inside the strip when the drag began

    CaptionView   m_view;
    CaptureEngine m_engine;
    Settings      m_settings;

    ULONGLONG m_lastRealtimeCopyTick = 0;
};
