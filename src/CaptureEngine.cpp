#include "CaptureEngine.h"

#include <objbase.h>

#include "resource.h"

CaptureEngine::~CaptureEngine() { Stop(); }

bool CaptureEngine::Start(HWND notifyWindow, const std::wstring& transcriptPath,
                          int pollIntervalMs, CaptionSourceChoice sourceChoice) {
    if (m_running.load(std::memory_order_acquire)) return true;

    // Start always represents a new capture session. In particular, changing
    // the configured source stops and restarts this same engine object; keeping
    // the previous merger history would make the new source append to it and
    // cause already-flushed lines to be written a second time.
    m_merger.Reset();
    m_sawChangeEvent = false;
    m_pollIntervalMs = (pollIntervalMs >= 1 && pollIntervalMs <= 1000)
                           ? static_cast<DWORD>(pollIntervalMs)
                           : kDefaultPollIntervalMs;
    m_notifyWindow = notifyWindow;
    m_sourceChoice = sourceChoice;
    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
    m_changeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset
    m_controlEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset
    if (!m_stopEvent || !m_changeEvent || !m_controlEvent) {
        if (m_stopEvent) ::CloseHandle(m_stopEvent);
        if (m_changeEvent) ::CloseHandle(m_changeEvent);
        if (m_controlEvent) ::CloseHandle(m_controlEvent);
        m_stopEvent = nullptr;
        m_changeEvent = nullptr;
        m_controlEvent = nullptr;
        return false;
    }

    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&CaptureEngine::Run, this, transcriptPath);
    return true;
}

void CaptureEngine::Stop() {
    if (m_stopEvent) ::SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
    if (m_stopEvent) {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    if (m_changeEvent) {
        ::CloseHandle(m_changeEvent);
        m_changeEvent = nullptr;
    }
    if (m_controlEvent) {
        ::CloseHandle(m_controlEvent);
        m_controlEvent = nullptr;
    }
    m_running.store(false, std::memory_order_release);
    m_sourceWindow.store(nullptr, std::memory_order_release);
}

void CaptureEngine::SetPaused(bool paused) {
    m_paused.store(paused, std::memory_order_release);
    // Wake either AwaitChange() or the indefinite paused wait so the new state
    // takes effect immediately.
    if (m_controlEvent) ::SetEvent(m_controlEvent);
}

bool CaptureEngine::Sleep(DWORD milliseconds) {
    return ::WaitForSingleObject(m_stopEvent, milliseconds) == WAIT_TIMEOUT;
}

bool CaptureEngine::AwaitChange() {
    const HANDLE handles[3] = {m_stopEvent, m_changeEvent, m_controlEvent};
    const DWORD result = ::WaitForMultipleObjects(3, handles, FALSE, m_pollIntervalMs);
    if (result == WAIT_OBJECT_0) return false;  // stop requested

    if (result == WAIT_OBJECT_0 + 1) {
        // A provider that really does raise events lets us skip the poll wait.
        // Report it the first time, since accepting a subscription turned out to
        // be no guarantee that anything is ever delivered.
        if (!m_sawChangeEvent) {
            m_sawChangeEvent = true;
            PostStatus(L"Source is pushing live change notifications.");
        }
        // Rate-limit so a very chatty provider cannot monopolise the thread.
        if (!Sleep(kMinReadIntervalMs)) return false;
    }
    // WAIT_OBJECT_0 + 2 is a pause/resume request. Return to the top of the
    // capture loop, which observes the new atomic state.
    return true;
}

void CaptureEngine::PostStatus(const std::wstring& text) {
    if (!m_notifyWindow) return;
    auto* payload = new std::wstring(text);
    if (!::PostMessageW(m_notifyWindow, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void CaptureEngine::Run(std::wstring transcriptPath) {
    // UI Automation clients should live in the MTA to avoid re-entrancy
    // deadlocks against the provider process.
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (!m_source.Initialize()) {
        PostStatus(L"Failed to initialise UI Automation.");
        if (SUCCEEDED(comInit)) ::CoUninitialize();
        return;
    }

    if (!transcriptPath.empty() && !m_store.Open(transcriptPath)) {
        PostStatus(L"Could not open transcript file; continuing without saving.");
    }

    size_t     flushedLines = 0;
    SourceKind lastKind = SourceKind::None;
    bool       searchingAnnounced = false;

    for (;;) {
        if (m_paused.load(std::memory_order_acquire)) {
            // Do no window discovery and no cross-process text reads while
            // paused. Resume and Stop both wake this wait immediately.
            const HANDLE handles[2] = {m_stopEvent, m_controlEvent};
            if (::WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            continue;
        }

        if (!m_source.IsAttached()) {
            if (!searchingAnnounced) {
                PostStatus(m_sourceChoice == CaptionSourceChoice::Chrome
                               ? L"Waiting for Chrome Live Caption\u2026"
                               : L"Waiting for Windows 11 Live Captions\u2026");
                searchingAnnounced = true;
            }
            if (m_source.Attach(m_sourceChoice)) {
                searchingAnnounced = false;
                lastKind = m_source.Kind();
                m_sourceWindow.store(m_source.SourceWindow(), std::memory_order_release);

                // Subscribe opportunistically, but do not advertise "live
                // updates" on the strength of it: Windows 11 Live captions
                // accepts the subscription and then never raises an event. The
                // status only claims push once one actually arrives.
                m_source.SubscribeToChanges(m_changeEvent);
                PostStatus(std::wstring(L"Connected to ") + SourceKindName(lastKind) +
                           L" \u2014 reading every " + std::to_wstring(m_pollIntervalMs) + L" ms");
                if (m_notifyWindow) {
                    ::PostMessageW(m_notifyWindow, WM_APP_SOURCE_CHANGED,
                                   static_cast<WPARAM>(lastKind),
                                   reinterpret_cast<LPARAM>(m_source.SourceWindow()));
                }
            } else {
                if (!Sleep(kSearchIntervalMs)) break;
                continue;
            }
        }

        std::wstring snapshot;
        if (!m_source.ReadText(snapshot)) {
            m_source.Detach();
            m_sourceWindow.store(nullptr, std::memory_order_release);
            PostStatus(L"Caption source closed. Reconnecting\u2026");
            searchingAnnounced = true;
            if (m_notifyWindow) {
                ::PostMessageW(m_notifyWindow, WM_APP_SOURCE_CHANGED,
                               static_cast<WPARAM>(SourceKind::None), 0);
            }
            if (!Sleep(kSearchIntervalMs)) break;
            continue;
        }

        if (!snapshot.empty()) {
            const CaptionMerger::Update update = m_merger.Ingest(snapshot);
            if (update.changed) {
                const std::vector<std::wstring>& lines = m_merger.Lines();
                auto* payload = new CaptionUpdate();
                payload->firstDirtyLine = update.firstDirtyLine;
                payload->lines.assign(lines.begin() + static_cast<ptrdiff_t>(update.firstDirtyLine),
                                      lines.end());
                if (!m_notifyWindow ||
                    !::PostMessageW(m_notifyWindow, WM_APP_CAPTION_UPDATE, 0,
                                    reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                }

                // Commit only the lines that have scrolled out of the caption
                // window, since anything still visible may yet be revised.
                const size_t stable = m_merger.StableLineCount();
                if (stable > flushedLines) {
                    m_store.AppendLines(lines, flushedLines, stable);
                    flushedLines = stable;
                }
            }
        }

        if (!AwaitChange()) break;
    }

    // The tail is still "unstable" by the scroll heuristic, but the session is
    // over, so write whatever is left.
    const std::vector<std::wstring>& lines = m_merger.Lines();
    if (flushedLines < lines.size()) {
        m_store.AppendLines(lines, flushedLines, lines.size());
    }
    m_store.Close();
    m_source.Shutdown();

    if (SUCCEEDED(comInit)) ::CoUninitialize();
}
