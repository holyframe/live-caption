#include "CaptureEngine.h"

#include <objbase.h>

#include "resource.h"

CaptureEngine::~CaptureEngine() { Stop(); }

bool CaptureEngine::Start(HWND notifyWindow, const std::wstring& transcriptPath) {
    if (m_running.load(std::memory_order_acquire)) return true;

    m_notifyWindow = notifyWindow;
    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
    m_changeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset
    if (!m_stopEvent || !m_changeEvent) {
        if (m_stopEvent) ::CloseHandle(m_stopEvent);
        if (m_changeEvent) ::CloseHandle(m_changeEvent);
        m_stopEvent = nullptr;
        m_changeEvent = nullptr;
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
    m_running.store(false, std::memory_order_release);
    m_sourceWindow.store(nullptr, std::memory_order_release);
}

bool CaptureEngine::Sleep(DWORD milliseconds) {
    return ::WaitForSingleObject(m_stopEvent, milliseconds) == WAIT_TIMEOUT;
}

bool CaptureEngine::AwaitChange() {
    const HANDLE handles[2] = {m_stopEvent, m_changeEvent};
    const DWORD result = ::WaitForMultipleObjects(2, handles, FALSE, kPollIntervalMs);
    if (result == WAIT_OBJECT_0) return false;  // stop requested

    if (result == WAIT_OBJECT_0 + 1) {
        // Woken by a change notification. Rate-limit so a provider that raises
        // events very rapidly cannot monopolise the thread.
        if (!Sleep(kMinReadIntervalMs)) return false;
    }
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
        if (!m_source.IsAttached()) {
            if (!searchingAnnounced) {
                PostStatus(L"Waiting for Chrome Live Caption or Windows 11 Live captions\u2026");
                searchingAnnounced = true;
            }
            if (m_source.Attach()) {
                searchingAnnounced = false;
                lastKind = m_source.Kind();
                m_sourceWindow.store(m_source.SourceWindow(), std::memory_order_release);

                // Push notifications remove the poll interval from the latency
                // budget; the timed poll stays on purely as a fallback.
                const bool live = m_source.SubscribeToChanges(m_changeEvent);
                PostStatus(std::wstring(L"Connected to ") + SourceKindName(lastKind) +
                           (live ? L" \u2014 live updates" : L" \u2014 polling"));
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
