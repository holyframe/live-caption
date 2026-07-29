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
};

// Resolves a browser/WebView tab and one of its editable fields through
// Windows UI Automation. The selected document and input element are retained
// so later features (such as Send) can use the exact tab that was picked.
class WebInputPicker {
public:
    WebInputPicker();
    ~WebInputPicker();

    WebInputPicker(const WebInputPicker&) = delete;
    WebInputPicker& operator=(const WebInputPicker&) = delete;

    WebInputPickState Inspect(POINT screenPoint, HWND ownWindow);
    void ResetCandidate();
    bool CommitCandidate();

    bool CandidateValid() const;
    HWND CandidateWindow() const;
    const std::wstring& CandidateName() const;

    HWND SelectedWindow() const;
    const std::wstring& SelectedName() const;

    // Re-activates the retained browser tab, focuses its editable field, and
    // replaces the field contents. When pressEnter is true a real Enter key is
    // injected after the text has been set.
    bool SendText(const std::wstring& text, bool pressEnter, std::wstring& error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
