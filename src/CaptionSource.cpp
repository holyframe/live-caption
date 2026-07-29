#include "CaptionSource.h"

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <vector>

#include "CaptionSourceMatcher.h"
#include "Util.h"

namespace {

std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))); });
    return text;
}

std::wstring WindowTitle(HWND hwnd) {
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<size_t>(std::max(copied, 0)));
    return title;
}

std::wstring ProcessImageName(DWORD pid) {
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};

    wchar_t buffer[MAX_PATH]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::wstring name;
    if (::QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        name = buffer;
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    ::CloseHandle(process);
    return ToLower(name);
}

struct EnumContext {
    HWND       found = nullptr;
    SourceKind kind = SourceKind::None;
    DWORD      selfPid = 0;
    CaptionSourceChoice choice = CaptionSourceChoice::WindowsLiveCaptions;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    if (!::IsWindowVisible(hwnd)) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    // Never scrape our own viewer window; its title also contains
    // "Live Caption", which would otherwise create a feedback loop.
    if (pid == 0 || pid == ctx->selfPid) return TRUE;

    const std::wstring image = ProcessImageName(pid);
    // Checking the process as well as the title is essential here. The Windows
    // panel is titled "Live captions", which also contains "live caption" and
    // would otherwise be mistaken for Chrome when Chrome is selected.
    const std::wstring title = ToLower(WindowTitle(hwnd));
    if (caption_source_detail::IsWindowForChoice(ctx->choice, image, title)) {
        ctx->found = hwnd;
        ctx->kind = ctx->choice == CaptionSourceChoice::Chrome
                        ? SourceKind::Chrome
                        : SourceKind::WindowsLiveCaptions;
        return FALSE;
    }
    return TRUE;
}

std::wstring BstrToString(BSTR bstr) {
    if (!bstr) return {};
    std::wstring out(bstr, ::SysStringLen(bstr));
    return out;
}

// Signals an event whenever the observed element's text changes. UI Automation
// invokes these callbacks on its own thread, so the handler does the minimum
// possible amount of work: waking the capture thread and returning.
//
// The event is auto-reset, which coalesces bursts of notifications into a single
// read for free.
class UiaChangeNotifier final : public IUIAutomationEventHandler,
                                public IUIAutomationPropertyChangedEventHandler {
public:
    explicit UiaChangeNotifier(HANDLE signal) : m_signal(signal) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAutomationEventHandler)) {
            *ppv = static_cast<IUIAutomationEventHandler*>(this);
        } else if (riid == __uuidof(IUIAutomationPropertyChangedEventHandler)) {
            *ppv = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement*, EVENTID) override {
        if (m_signal) ::SetEvent(m_signal);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement*, PROPERTYID,
                                                        VARIANT) override {
        if (m_signal) ::SetEvent(m_signal);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    HANDLE             m_signal = nullptr;
};

}  // namespace

const wchar_t* SourceKindName(SourceKind kind) {
    switch (kind) {
        case SourceKind::Chrome:              return L"Chrome Live Caption";
        case SourceKind::WindowsLiveCaptions: return L"Windows 11 Live captions";
        default:                              return L"none";
    }
}

bool CaptionSource::Initialize() {
    if (m_automation) return true;

    HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&m_automation));
    if (FAILED(hr)) {
        hr = ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&m_automation));
    }
    if (FAILED(hr) || !m_automation) return false;

    if (SUCCEEDED(m_automation->CreateCacheRequest(&m_cacheRequest)) && m_cacheRequest) {
        m_cacheRequest->AddProperty(UIA_NamePropertyId);
        m_cacheRequest->AddProperty(UIA_ControlTypePropertyId);
        m_cacheRequest->put_AutomationElementMode(AutomationElementMode_Full);
    }
    return true;
}

void CaptionSource::Shutdown() {
    Detach();
    m_cacheRequest.Reset();
    m_automation.Reset();
}

void CaptionSource::Detach() {
    Unsubscribe();
    m_textPattern.Reset();
    m_valuePattern.Reset();
    m_strategy = ReadStrategy::None;
    m_textElement.Reset();
    m_sourceWindow = nullptr;
    m_kind = SourceKind::None;
    m_consecutiveFailures = 0;
}

bool CaptionSource::SubscribeToChanges(HANDLE signalEvent) {
    if (!m_automation || !m_textElement || !signalEvent) return false;
    Unsubscribe();

    auto* raw = new (std::nothrow) UiaChangeNotifier(signalEvent);
    if (!raw) return false;

    // Adopt the constructor's initial reference, then reach the second interface
    // by explicit cast rather than QueryInterface.
    ComPtr<IUIAutomationEventHandler> notifier;
    notifier.Attach(static_cast<IUIAutomationEventHandler*>(raw));
    auto* propertyHandler = static_cast<IUIAutomationPropertyChangedEventHandler*>(raw);

    bool subscribed = false;

    // TextChanged is the precise signal, but it only exists where the provider
    // implements TextPattern.
    if (m_strategy == ReadStrategy::TextPattern) {
        if (SUCCEEDED(m_automation->AddAutomationEventHandler(
                UIA_Text_TextChangedEventId, m_textElement.Get(), TreeScope_Element, nullptr,
                notifier.Get()))) {
            subscribed = true;
        }
    }

    // Name/Value changes cover the providers that expose their caption as a
    // plain property, and act as a second chance for the rest.
    PROPERTYID properties[] = {UIA_NamePropertyId, UIA_ValueValuePropertyId};
    if (SUCCEEDED(m_automation->AddPropertyChangedEventHandlerNativeArray(
            m_textElement.Get(), TreeScope_Element, nullptr, propertyHandler, properties,
            static_cast<int>(std::size(properties))))) {
        subscribed = true;
    }

    if (!subscribed) return false;
    m_notifier = notifier;
    return true;
}

void CaptionSource::Unsubscribe() {
    if (m_notifier && m_automation) {
        m_automation->RemoveAllEventHandlers();
    }
    m_notifier.Reset();
}

bool CaptionSource::ProbeStrategy() {
    m_strategy = ReadStrategy::None;
    m_textPattern.Reset();
    m_valuePattern.Reset();
    if (!m_textElement) return false;

    ComPtr<IUnknown> unknown;
    if (SUCCEEDED(m_textElement->GetCurrentPattern(UIA_TextPatternId, &unknown)) && unknown) {
        ComPtr<IUIAutomationTextPattern> pattern;
        if (SUCCEEDED(unknown.As(&pattern)) && pattern) {
            std::wstring probe;
            m_textPattern = pattern;
            m_strategy = ReadStrategy::TextPattern;
            if (ReadWithStrategy(probe) && !probe.empty()) return true;
            m_textPattern.Reset();
        }
    }

    unknown.Reset();
    if (SUCCEEDED(m_textElement->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
        ComPtr<IUIAutomationValuePattern> pattern;
        if (SUCCEEDED(unknown.As(&pattern)) && pattern) {
            std::wstring probe;
            m_valuePattern = pattern;
            m_strategy = ReadStrategy::ValuePattern;
            if (ReadWithStrategy(probe) && !probe.empty()) return true;
            m_valuePattern.Reset();
        }
    }

    m_strategy = ReadStrategy::Name;
    std::wstring probe;
    if (ReadWithStrategy(probe) && !probe.empty()) return true;

    m_strategy = ReadStrategy::None;
    return false;
}

bool CaptionSource::ReadWithStrategy(std::wstring& out) {
    out.clear();
    switch (m_strategy) {
        case ReadStrategy::TextPattern: {
            if (!m_textPattern) return false;
            ComPtr<IUIAutomationTextRange> range;
            if (FAILED(m_textPattern->get_DocumentRange(&range)) || !range) return false;
            BSTR bstr = nullptr;
            if (FAILED(range->GetText(-1, &bstr)) || !bstr) return false;
            out = BstrToString(bstr);
            ::SysFreeString(bstr);
            return true;
        }
        case ReadStrategy::ValuePattern: {
            if (!m_valuePattern) return false;
            BSTR bstr = nullptr;
            if (FAILED(m_valuePattern->get_CurrentValue(&bstr)) || !bstr) return false;
            out = BstrToString(bstr);
            ::SysFreeString(bstr);
            return true;
        }
        case ReadStrategy::Name: {
            if (!m_textElement) return false;
            BSTR bstr = nullptr;
            if (FAILED(m_textElement->get_CurrentName(&bstr)) || !bstr) return false;
            out = BstrToString(bstr);
            ::SysFreeString(bstr);
            return true;
        }
        default:
            return false;
    }
}

CaptionSource::Candidate CaptionSource::FindCaptionWindow(CaptionSourceChoice choice) {
    EnumContext ctx;
    ctx.selfPid = ::GetCurrentProcessId();
    ctx.choice = choice;
    ::EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return Candidate{ctx.found, ctx.kind};
}

bool CaptionSource::Attach(CaptionSourceChoice choice) {
    if (!m_automation && !Initialize()) return false;

    const Candidate candidate = FindCaptionWindow(choice);
    if (!candidate.hwnd) return false;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(m_automation->ElementFromHandle(candidate.hwnd, &root)) || !root) return false;

    ComPtr<IUIAutomationElement> text = ResolveTextElement(root.Get(), candidate.kind);
    if (!text) return false;

    m_textElement = text;
    m_sourceWindow = candidate.hwnd;
    m_kind = candidate.kind;
    m_consecutiveFailures = 0;

    if (!ProbeStrategy()) {
        Detach();
        return false;
    }
    return true;
}

CaptionSource::ComPtr<IUIAutomationElement> CaptionSource::ResolveTextElement(
    IUIAutomationElement* root, SourceKind kind) {
    ComPtr<IUIAutomationElement> result;

    // Windows 11 exposes the caption text under a well-known automation id.
    if (kind == SourceKind::WindowsLiveCaptions) {
        VARIANT value{};
        value.vt = VT_BSTR;
        value.bstrVal = ::SysAllocString(L"CaptionsTextBlock");
        ComPtr<IUIAutomationCondition> condition;
        if (SUCCEEDED(m_automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value,
                                                            &condition)) &&
            condition) {
            root->FindFirst(TreeScope_Descendants, condition.Get(), &result);
        }
        ::VariantClear(&value);
        if (result) return result;
    }

    // Chrome puts the caption inside a Document element.
    {
        VARIANT value{};
        value.vt = VT_I4;
        value.lVal = UIA_DocumentControlTypeId;
        ComPtr<IUIAutomationCondition> condition;
        if (SUCCEEDED(m_automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value,
                                                            &condition)) &&
            condition) {
            root->FindFirst(TreeScope_Descendants, condition.Get(), &result);
        }
        if (result) {
            std::wstring probe;
            if (ExtractText(result.Get(), probe)) return result;
            result.Reset();
        }
    }

    // Fallback: take whichever Text element currently holds the most content.
    {
        VARIANT value{};
        value.vt = VT_I4;
        value.lVal = UIA_TextControlTypeId;
        ComPtr<IUIAutomationCondition> condition;
        if (FAILED(m_automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value,
                                                         &condition)) ||
            !condition) {
            return nullptr;
        }

        ComPtr<IUIAutomationElementArray> array;
        // BuildCache fetches every Name in one cross-process call instead of
        // one call per element.
        HRESULT hr = m_cacheRequest
                         ? root->FindAllBuildCache(TreeScope_Descendants, condition.Get(),
                                                   m_cacheRequest.Get(), &array)
                         : root->FindAll(TreeScope_Descendants, condition.Get(), &array);
        if (FAILED(hr) || !array) return nullptr;

        int count = 0;
        array->get_Length(&count);
        size_t bestLength = 0;
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> element;
            if (FAILED(array->GetElement(i, &element)) || !element) continue;

            BSTR name = nullptr;
            if (m_cacheRequest) {
                if (FAILED(element->get_CachedName(&name))) name = nullptr;
            }
            if (!name && FAILED(element->get_CurrentName(&name))) continue;

            const std::wstring text = BstrToString(name);
            ::SysFreeString(name);
            if (text.size() > bestLength) {
                bestLength = text.size();
                result = element;
            }
        }
    }
    return result;
}

bool CaptionSource::ExtractText(IUIAutomationElement* element, std::wstring& out) {
    if (!element) return false;
    out.clear();

    // TextPattern is the most faithful for multi-line caption panels.
    ComPtr<IUnknown> unknown;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_TextPatternId, &unknown)) && unknown) {
        ComPtr<IUIAutomationTextPattern> textPattern;
        if (SUCCEEDED(unknown.As(&textPattern)) && textPattern) {
            ComPtr<IUIAutomationTextRange> range;
            if (SUCCEEDED(textPattern->get_DocumentRange(&range)) && range) {
                BSTR bstr = nullptr;
                if (SUCCEEDED(range->GetText(-1, &bstr)) && bstr) {
                    out = BstrToString(bstr);
                    ::SysFreeString(bstr);
                    if (!out.empty()) return true;
                }
            }
        }
    }

    unknown.Reset();
    if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
        ComPtr<IUIAutomationValuePattern> valuePattern;
        if (SUCCEEDED(unknown.As(&valuePattern)) && valuePattern) {
            BSTR bstr = nullptr;
            if (SUCCEEDED(valuePattern->get_CurrentValue(&bstr)) && bstr) {
                out = BstrToString(bstr);
                ::SysFreeString(bstr);
                if (!out.empty()) return true;
            }
        }
    }

    BSTR name = nullptr;
    if (SUCCEEDED(element->get_CurrentName(&name)) && name) {
        out = BstrToString(name);
        ::SysFreeString(name);
        return !out.empty();
    }
    return false;
}

bool CaptionSource::ReadText(std::wstring& out) {
    if (!m_textElement) return false;

    // A destroyed source window invalidates the cached element immediately.
    if (m_sourceWindow && !::IsWindow(m_sourceWindow)) return false;

    if (ReadWithStrategy(out) && !out.empty()) {
        m_consecutiveFailures = 0;
        return true;
    }

    // An empty read is normal when the speaker pauses, so only treat a run of
    // failures as a lost connection.
    out.clear();
    if (++m_consecutiveFailures >= 25) return false;
    return true;
}
