#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "CaptionMerger.h"
#include "CaptionSource.h"
#include "TranscriptStore.h"

// Payload for WM_APP_CAPTION_UPDATE. The receiving window takes ownership.
struct CaptionUpdate {
    size_t                    firstDirtyLine = 0;
    std::vector<std::wstring> lines;  // replaces everything from firstDirtyLine on
};

// Owns the background polling thread that scrapes the caption window, merges
// snapshots into a transcript, and streams results to the UI.
//
// Unlike the Python version, every wait is performed on a stop event, so Stop()
// returns promptly even while the engine is searching for a caption window.
class CaptureEngine {
public:
    ~CaptureEngine();

    CaptureEngine() = default;
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    // `notifyWindow` receives WM_APP_CAPTION_UPDATE / WM_APP_STATUS /
    // WM_APP_SOURCE_CHANGED.
    bool Start(HWND notifyWindow, const std::wstring& transcriptPath, int pollIntervalMs);
    void Stop();

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    HWND SourceWindow() const { return m_sourceWindow.load(std::memory_order_acquire); }

private:
    void Run(std::wstring transcriptPath);
    bool Sleep(DWORD milliseconds);  // returns false when asked to stop
    // Blocks until the caption text changes, the safety poll expires, or the
    // engine is stopped. Returns false only when asked to stop.
    bool AwaitChange();
    void PostStatus(const std::wstring& text);

    // Measured: Windows 11 Live captions accepts a change subscription but never
    // raises an event, so polling is not a fallback there, it is the mechanism.
    // The interval is therefore configurable and tight by default.
    static constexpr DWORD kDefaultPollIntervalMs = 8;
    static constexpr DWORD kSearchIntervalMs = 100;
    // Floor between consecutive reads, so a provider that does raise events
    // rapidly cannot spin the thread.
    static constexpr DWORD kMinReadIntervalMs = 2;

    DWORD              m_pollIntervalMs = kDefaultPollIntervalMs;
    bool               m_sawChangeEvent = false;
    std::thread        m_thread;
    HANDLE             m_stopEvent = nullptr;
    HANDLE             m_changeEvent = nullptr;
    HWND               m_notifyWindow = nullptr;
    std::atomic<bool>  m_running{false};
    std::atomic<HWND>  m_sourceWindow{nullptr};

    CaptionSource   m_source;
    CaptionMerger   m_merger;
    TranscriptStore m_store;
};
