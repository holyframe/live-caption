#pragma once

#include <string>

enum class CaptionSourcePreference {
    WindowsLiveCaptions,
    ChromeLiveCaption,
};

enum class UiTheme {
    Dark,
    Light,
    System,
};

// Simple INI-backed preferences stored next to the executable.
struct Settings {
    CaptionSourcePreference captionSource = CaptionSourcePreference::WindowsLiveCaptions;
    UiTheme      theme        = UiTheme::Dark;
    std::wstring fontFamily   = L"Segoe UI";
    int          fontSizePt   = 12;
    double       lineSpacing  = 1.3;
    bool         pressEnter   = true;
    bool         copyRealtime = false;
    bool         compactView  = false;
    bool         alwaysOnTop  = false;
    std::wstring transcriptPath;   // empty means "captions.txt" beside the exe

    // How often the caption source is re-read, in milliseconds.
    //
    // This is the dominant source of client-side lag, because Windows 11 Live
    // captions accepts a UI Automation change subscription but never actually
    // raises an event, leaving polling as the only mechanism that works. A read
    // costs about 0.4 ms, so a tight interval is affordable.
    int pollIntervalMs = 8;

    // Global shortcut for the Send button.
    unsigned hotkeyModifiers = 0x0004 /*MOD_SHIFT*/;
    unsigned hotkeyVirtualKey = 'Z';

    int windowX = -1, windowY = -1, windowW = 960, windowH = 620;

    // Height of the bottom panel at 96 dpi; the caption pane takes whatever the
    // window has left over, so dragging the splitter changes this.
    int bottomPanelHeight = 32;

    void Load();
    void Save() const;

    static std::wstring FilePath();
    std::wstring ResolvedTranscriptPath() const;
};
