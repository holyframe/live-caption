#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "CaptionView.h"
#include "CaptureEngine.h"
#include "Settings.h"

class MainWindow {
public:
    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance, int showCommand);
    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
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
    LRESULT OnCtlColor(HDC dc, HWND control);
    void Layout();
    void UpdateHotkeyRegistration();
    void SetStatus(const std::wstring& text);
    void ApplyTypography();
    void ApplyViewMode();
    void ToggleVisibility();

    void OnCommand(int controlId, int notifyCode);
    void OnSend();
    void OnCopy();
    void OnFrontAll();
    void OnMinimizeAll();
    void OnCaptionUpdate(CaptionUpdate* update);
    void MaybeCopyRealtime();
    void DrainPendingPayloads();

    bool CopyTextToClipboard(const std::wstring& text);
    int  Scaled(int value) const;

    HWND      m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    UINT      m_dpi = 96;
    HFONT     m_controlFont = nullptr;
    bool      m_hotkeyRegistered = false;

    HWND m_sendButton = nullptr;
    HWND m_pressEnterCheck = nullptr;
    HWND m_hintLabel = nullptr;
    HWND m_viewModeButton = nullptr;
    HWND m_fontCombo = nullptr;
    HWND m_sizeCombo = nullptr;
    HWND m_spacingCombo = nullptr;
    HWND m_copyLiveCheck = nullptr;
    HWND m_frontAllButton = nullptr;
    HWND m_minimizeAllButton = nullptr;
    HWND m_copyButton = nullptr;
    HWND m_sidePanel = nullptr;
    HWND m_statusBar = nullptr;

    HBRUSH m_windowBrush = nullptr;
    HBRUSH m_panelBrush = nullptr;
    HBRUSH m_buttonBrush = nullptr;
    HBRUSH m_buttonPressedBrush = nullptr;

    CaptionView   m_view;
    CaptureEngine m_engine;
    Settings      m_settings;

    HWND      m_sourceWindow = nullptr;
    ULONGLONG m_lastRealtimeCopyTick = 0;
};
