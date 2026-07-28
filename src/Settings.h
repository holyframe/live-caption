#pragma once

#include <string>

// Simple INI-backed preferences stored next to the executable.
struct Settings {
    std::wstring fontFamily   = L"Segoe UI";
    int          fontSizePt   = 12;
    double       lineSpacing  = 1.3;
    bool         pressEnter   = true;
    bool         copyRealtime = false;
    bool         compactView  = false;
    bool         alwaysOnTop  = false;
    std::wstring transcriptPath;   // empty means "captions.txt" beside the exe

    // Global show/hide hotkey.
    //
    // Windows rejects RegisterHotKey for a bare Shift+<letter> combination, so
    // the "Shift+Z" from the original design cannot be honoured literally; the
    // only way to get it would be a system-wide low-level keyboard hook, which
    // would also stop you typing a capital Z anywhere else. Ctrl+Shift+Z is the
    // closest combination the OS will actually grant.
    unsigned hotkeyModifiers = 0x0002 /*MOD_CONTROL*/ | 0x0004 /*MOD_SHIFT*/;
    unsigned hotkeyVirtualKey = 'Z';

    int windowX = -1, windowY = -1, windowW = 960, windowH = 620;

    void Load();
    void Save() const;

    static std::wstring FilePath();
    std::wstring ResolvedTranscriptPath() const;
};
