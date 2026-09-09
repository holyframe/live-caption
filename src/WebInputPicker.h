#pragma once

#include <windows.h>

#include <memory>
#include <string>

enum class WebInputPickState {
    NoWindow,
    OwnWindow,
    NoWebDocument,
    NoEditableInput,
    Valid,
    Checking,
};

enum class WebInputSendResult {
    Failed,
    Inserted,
    Submitted,
    SubmissionUnconfirmed,
};

inline bool IsPickableState(WebInputPickState state) {
    return state == WebInputPickState::Valid;
}

// Resolves a browser/WebView tab and one of its editable fields through
// Windows UI Automation. The selected document and input element are retained
// so later features (such as Send) can use the exact tab that was picked.
// Browsers whose renderer accessibility is switched off are intentionally
// rejected: without an accessible input the app cannot prove where text or
// Enter would be delivered.
// Public methods are called by the UI thread. UIA objects and all their calls
// live exclusively on a dedicated, windowless COM MTA worker.
class WebInputPicker {
public:
    WebInputPicker();
    ~WebInputPicker();

    WebInputPicker(const WebInputPicker&) = delete;
    WebInputPicker& operator=(const WebInputPicker&) = delete;

    // Non-blocking, window-wide hover preview. Poll while dragging; obsolete
    // requests/results are discarded. This never authorizes a commit.
    WebInputPickState Preview(POINT screenPoint, HWND ownWindow);
    // Synchronous validation. A drop must force a fresh, point-specific check.
    WebInputPickState Inspect(POINT screenPoint, HWND ownWindow, bool forceRefresh = false);
    // Cancellation clears the UI snapshot immediately; COM cleanup is queued.
    void ResetCandidate();
    // Only the most recent successful forced Inspect may be committed.
    bool CommitCandidate();

    bool CandidateValid() const;
    HWND CandidateWindow() const;
    const std::wstring& CandidateName() const;

    HWND SelectedWindow() const;
    const std::wstring& SelectedName() const;
    void ClearSelected();

    // Asks Windows to bring the retained target forward. Send already does
    // this, but Windows only grants the request to a process it considers to
    // have received the last input event, and a global hotkey grants that for
    // roughly a quarter of a second. Waiting for the hotkey's keys to be
    // released outlasts it, so the hotkey path calls this first, while the
    // permission is still there. Returns without touching the worker thread so
    // that nothing can delay it. Clicking Send never needs it: the app is the
    // foreground window then. Send also releases modifiers before typing so a
    // still-held Shift chord cannot turn Enter into a newline.
    void RaiseSelectedWindow();

    // Re-activates the retained browser tab and exact editable field, preserves
    // its existing contents, and appends with real keyboard input. When
    // requested, Enter follows in the same injection so focus cannot move
    // between the caption and submission.
    WebInputSendResult SendText(const std::wstring& text, bool pressEnter,
                                std::wstring& message);

private:
#ifdef WEBINPUT_PICKER_TESTING
    friend struct WebInputPickerTestAccess;
#endif
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
