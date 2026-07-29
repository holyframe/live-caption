#pragma once

#include <windows.h>
// uiautomation.h relies on the `interface` macro and on IAccessible, neither of
// which windows.h provides once WIN32_LEAN_AND_MEAN is defined.
#include <objbase.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <string>

enum class SourceKind {
    None,
    Chrome,          // Google Chrome "Live Caption" bubble
    WindowsLiveCaptions,  // Windows 11 built-in Live captions
};

enum class CaptionSourceChoice {
    WindowsLiveCaptions,
    Chrome,
};

const wchar_t* SourceKindName(SourceKind kind);

// Reads caption text out of another process's window via UI Automation.
//
// Two things make this much cheaper than the Python original, which walked
// every descendant of the window on each 100 ms tick:
//   * the text element is resolved once with a targeted FindFirst and then
//     cached, so a steady-state poll is a single cross-process property read;
//   * a cache request batches the property fetch into that one call.
// The element is re-resolved only when the cached one stops responding.
class CaptionSource {
public:
    bool Initialize();
    void Shutdown();

    // Looks for a supported caption window. Returns true if one is attached.
    bool Attach(CaptionSourceChoice choice = CaptionSourceChoice::WindowsLiveCaptions);
    void Detach();

    bool IsAttached() const { return m_textElement != nullptr; }
    SourceKind Kind() const { return m_kind; }
    HWND SourceWindow() const { return m_sourceWindow; }

    // Reads the current caption text. Returns false if the source went away,
    // in which case the caller should Detach() and retry Attach().
    bool ReadText(std::wstring& out);

    // Asks UI Automation to signal `signalEvent` whenever the caption text
    // changes, so the caller can react immediately instead of waiting for its
    // next poll. Must be called from the same thread that owns the source.
    bool SubscribeToChanges(HANDLE signalEvent);
    void Unsubscribe();

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Candidate {
        HWND       hwnd = nullptr;
        SourceKind kind = SourceKind::None;
    };

    // Which accessor actually yields text for this element. Deciding once at
    // attach time keeps a steady-state read down to one or two cross-process
    // calls, instead of re-probing all three every time.
    enum class ReadStrategy { None, TextPattern, ValuePattern, Name };

    static Candidate FindCaptionWindow(CaptionSourceChoice choice);
    ComPtr<IUIAutomationElement> ResolveTextElement(IUIAutomationElement* root, SourceKind kind);
    bool ExtractText(IUIAutomationElement* element, std::wstring& out);
    bool ProbeStrategy();
    bool ReadWithStrategy(std::wstring& out);

    ComPtr<IUIAutomation>        m_automation;
    ComPtr<IUIAutomationElement> m_textElement;
    ComPtr<IUIAutomationCacheRequest> m_cacheRequest;
    ComPtr<IUIAutomationTextPattern>  m_textPattern;
    ComPtr<IUIAutomationValuePattern> m_valuePattern;
    // Holds the change-notification callback alive. Typed rather than IUnknown
    // because the handler inherits two COM interfaces, which makes the upcast to
    // IUnknown ambiguous.
    ComPtr<IUIAutomationEventHandler> m_notifier;
    ReadStrategy m_strategy = ReadStrategy::None;
    HWND       m_sourceWindow = nullptr;
    SourceKind m_kind = SourceKind::None;
    int        m_consecutiveFailures = 0;
};
