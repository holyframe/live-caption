#include "WebInputPicker.h"

#include <dwmapi.h>
#include <objbase.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>
#include <vector>

namespace {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

std::wstring BstrToString(BSTR value) {
    if (!value) return {};
    return std::wstring(value, ::SysStringLen(value));
}

std::wstring ElementName(IUIAutomationElement* element) {
    if (!element) return {};
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentName(&value)) || !value) return {};
    std::wstring name = BstrToString(value);
    ::SysFreeString(value);
    return name;
}

std::wstring WindowTitle(HWND hwnd) {
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<size_t>(std::max(copied, 0)));
    return title;
}

std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character)));
    });
    return text;
}

std::wstring ElementAutomationId(IUIAutomationElement* element) {
    if (!element) return {};
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentAutomationId(&value)) || !value) return {};
    std::wstring id = BstrToString(value);
    ::SysFreeString(value);
    return id;
}

ComPtr<IUIAutomationCondition> PropertyCondition(IUIAutomation* automation, PROPERTYID property,
                                                  VARTYPE type, LONG value) {
    ComPtr<IUIAutomationCondition> condition;
    if (!automation) return condition;

    VARIANT variant{};
    variant.vt = type;
    if (type == VT_BOOL) {
        variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    } else {
        variant.lVal = value;
    }
    automation->CreatePropertyCondition(property, variant, &condition);
    return condition;
}

ComPtr<IUIAutomationCondition> And(IUIAutomation* automation, IUIAutomationCondition* left,
                                   IUIAutomationCondition* right) {
    ComPtr<IUIAutomationCondition> condition;
    if (automation && left && right) {
        automation->CreateAndCondition(left, right, &condition);
    }
    return condition;
}

bool IsVisibleDocument(IUIAutomationElement* element) {
    if (!element) return false;
    CONTROLTYPEID type = 0;
    BOOL offscreen = TRUE;
    return SUCCEEDED(element->get_CurrentControlType(&type)) &&
           type == UIA_DocumentControlTypeId &&
           SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen;
}

bool PointInside(const RECT& rect, POINT point) {
    return ::PtInRect(&rect, point) != FALSE;
}

bool IsUsableWebEdit(IUIAutomationElement* element) {
    if (!element) return false;

    CONTROLTYPEID type = 0;
    BOOL enabled = FALSE;
    BOOL focusable = FALSE;
    BOOL offscreen = TRUE;
    BOOL password = TRUE;
    if (FAILED(element->get_CurrentControlType(&type)) || type != UIA_EditControlTypeId ||
        FAILED(element->get_CurrentIsEnabled(&enabled)) || !enabled ||
        FAILED(element->get_CurrentIsKeyboardFocusable(&focusable)) || !focusable ||
        FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
        FAILED(element->get_CurrentIsPassword(&password)) || password) {
        return false;
    }

    // A writable ValuePattern is the usual HTML input/textarea shape.
    ComPtr<IUnknown> unknown;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
        ComPtr<IUIAutomationValuePattern> valuePattern;
        if (SUCCEEDED(unknown.As(&valuePattern)) && valuePattern) {
            BOOL readOnly = TRUE;
            if (SUCCEEDED(valuePattern->get_CurrentIsReadOnly(&readOnly)) && !readOnly) return true;
        }
    }

    // Chromium exposes some contenteditable elements as Edit + TextPattern
    // instead of ValuePattern.
    unknown.Reset();
    return SUCCEEDED(element->GetCurrentPattern(UIA_TextPatternId, &unknown)) && unknown;
}

void AddVirtualKey(std::vector<INPUT>& events, WORD key, bool keyUp = false) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    events.push_back(input);
}

void AddUnicodeKey(std::vector<INPUT>& events, wchar_t character, bool keyUp = false) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(character);
    input.ki.dwFlags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0);
    events.push_back(input);
}

bool InjectEvents(const std::vector<INPUT>& events) {
    if (events.empty()) return true;
    return ::SendInput(static_cast<UINT>(events.size()), const_cast<INPUT*>(events.data()),
                       sizeof(INPUT)) == events.size();
}

bool ReplaceWithKeyboard(const std::wstring& text) {
    std::vector<INPUT> events;
    events.reserve(8);
    AddVirtualKey(events, VK_CONTROL);
    AddVirtualKey(events, 'A');
    AddVirtualKey(events, 'A', true);
    AddVirtualKey(events, VK_CONTROL, true);
    AddVirtualKey(events, VK_BACK);
    AddVirtualKey(events, VK_BACK, true);
    if (!InjectEvents(events)) return false;

    events.clear();
    events.reserve(256);
    for (size_t index = 0; index < text.size(); ++index) {
        wchar_t character = text[index];
        if (character == L'\r') {
            if (index + 1 < text.size() && text[index + 1] == L'\n') continue;
            character = L'\n';
        }
        AddUnicodeKey(events, character);
        AddUnicodeKey(events, character, true);
        if (events.size() >= 256) {
            if (!InjectEvents(events)) return false;
            events.clear();
        }
    }
    return InjectEvents(events);
}

bool InjectEnter() {
    std::vector<INPUT> events;
    events.reserve(2);
    AddVirtualKey(events, VK_RETURN);
    AddVirtualKey(events, VK_RETURN, true);
    return InjectEvents(events);
}

}  // namespace

struct WebInputPicker::Impl {
    struct Target {
        HWND hwnd = nullptr;
        std::wstring name;
        ComPtr<IUIAutomationElement> document;
        ComPtr<IUIAutomationElement> input;
        ComPtr<IUIAutomationElement> browserTab;

        void Reset() {
            hwnd = nullptr;
            name.clear();
            document.Reset();
            input.Reset();
            browserTab.Reset();
        }
    };

    bool EnsureAutomation() {
        if (automation) return true;
        HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&automation));
        if (FAILED(hr)) {
            hr = ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&automation));
        }
        return SUCCEEDED(hr) && automation;
    }

    ComPtr<IUIAutomationElement> DocumentFromPoint(POINT point) {
        ComPtr<IUIAutomationElement> current;
        if (FAILED(automation->ElementFromPoint(point, &current)) || !current) return nullptr;

        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) return nullptr;

        for (int depth = 0; depth < 48 && current; ++depth) {
            if (IsVisibleDocument(current.Get())) return current;
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.Get(), &parent)) || !parent) break;
            current = std::move(parent);
        }
        return nullptr;
    }

    ComPtr<IUIAutomationElement> VisibleDocument(IUIAutomationElement* root, POINT point) {
        ComPtr<IUIAutomationElement> document = DocumentFromPoint(point);
        if (document) return document;

        auto documentType = PropertyCondition(automation.Get(), UIA_ControlTypePropertyId, VT_I4,
                                              UIA_DocumentControlTypeId);
        if (!documentType) return nullptr;

        ComPtr<IUIAutomationElementArray> documents;
        if (FAILED(root->FindAll(TreeScope_Descendants, documentType.Get(), &documents)) ||
            !documents) {
            return nullptr;
        }

        int count = 0;
        documents->get_Length(&count);
        ComPtr<IUIAutomationElement> firstVisible;
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> item;
            if (FAILED(documents->GetElement(index, &item)) || !IsVisibleDocument(item.Get())) {
                continue;
            }
            if (!firstVisible) firstVisible = item;

            RECT bounds{};
            if (SUCCEEDED(item->get_CurrentBoundingRectangle(&bounds)) &&
                PointInside(bounds, point)) {
                return item;
            }
        }
        return firstVisible;
    }

    bool IsInsideDocument(IUIAutomationElement* element) {
        if (!element) return false;
        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) return false;

        ComPtr<IUIAutomationElement> current = element;
        for (int depth = 0; depth < 48 && current; ++depth) {
            CONTROLTYPEID type = 0;
            if (SUCCEEDED(current->get_CurrentControlType(&type)) &&
                type == UIA_DocumentControlTypeId) {
                return true;
            }
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.Get(), &parent)) || !parent) break;
            current = std::move(parent);
        }
        return false;
    }

    ComPtr<IUIAutomationElement> InputFromPoint(IUIAutomationElement* document, POINT point) {
        ComPtr<IUIAutomationElement> current;
        if (!document || FAILED(automation->ElementFromPoint(point, &current)) || !current) {
            return nullptr;
        }

        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) return nullptr;

        ComPtr<IUIAutomationElement> edit;
        for (int depth = 0; depth < 48 && current; ++depth) {
            if (!edit && IsUsableWebEdit(current.Get())) edit = current;

            CONTROLTYPEID type = 0;
            if (SUCCEEDED(current->get_CurrentControlType(&type)) &&
                type == UIA_DocumentControlTypeId) {
                BOOL same = FALSE;
                if (SUCCEEDED(automation->CompareElements(current.Get(), document, &same)) && same) {
                    return edit;
                }
                return nullptr;
            }

            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.Get(), &parent)) || !parent) break;
            current = std::move(parent);
        }
        return nullptr;
    }

    int InputScore(IUIAutomationElement* input) {
        std::wstring description = ElementName(input);
        description += L" ";
        description += ElementAutomationId(input);
        description = ToLower(std::move(description));

        int score = 0;
        const wchar_t* preferred[] = {L"message", L"prompt", L"chat",  L"reply",
                                      L"compose", L"ask",    L"type",  L"write",
                                      L"send"};
        for (const wchar_t* word : preferred) {
            if (description.find(word) != std::wstring::npos) score += 80;
        }
        const wchar_t* avoided[] = {L"search", L"find", L"filter", L"username", L"email"};
        for (const wchar_t* word : avoided) {
            if (description.find(word) != std::wstring::npos) score -= 100;
        }

        ComPtr<IUnknown> unknown;
        if (SUCCEEDED(input->GetCurrentPattern(UIA_TextPatternId, &unknown)) && unknown) {
            score += 30;  // multiline/contenteditable fields are usually chat composers
        }

        RECT bounds{};
        if (SUCCEEDED(input->get_CurrentBoundingRectangle(&bounds))) {
            const int height = std::max(bounds.bottom - bounds.top, 0L);
            if (height >= 32) score += 20;
            score += std::min(height / 8, 20);
        }
        return score;
    }

    ComPtr<IUIAutomationElement> EditableInput(IUIAutomationElement* document,
                                                const POINT* pickPoint = nullptr) {
        if (!document) return nullptr;

        if (pickPoint) {
            ComPtr<IUIAutomationElement> pointed = InputFromPoint(document, *pickPoint);
            if (pointed) return pointed;
        }

        auto edit = PropertyCondition(automation.Get(), UIA_ControlTypePropertyId, VT_I4,
                                      UIA_EditControlTypeId);
        auto enabled =
            PropertyCondition(automation.Get(), UIA_IsEnabledPropertyId, VT_BOOL, TRUE);
        auto focusable =
            PropertyCondition(automation.Get(), UIA_IsKeyboardFocusablePropertyId, VT_BOOL, TRUE);
        auto onscreen =
            PropertyCondition(automation.Get(), UIA_IsOffscreenPropertyId, VT_BOOL, FALSE);
        auto enabledEdit = And(automation.Get(), edit.Get(), enabled.Get());
        auto focusableEdit = And(automation.Get(), enabledEdit.Get(), focusable.Get());
        auto usableEdit = And(automation.Get(), focusableEdit.Get(), onscreen.Get());
        if (!usableEdit) return nullptr;

        ComPtr<IUIAutomationElementArray> inputs;
        if (FAILED(document->FindAll(TreeScope_Descendants, usableEdit.Get(), &inputs)) || !inputs) {
            return nullptr;
        }

        int count = 0;
        inputs->get_Length(&count);
        int bestScore = (std::numeric_limits<int>::min)();
        ComPtr<IUIAutomationElement> best;
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> input;
            if (SUCCEEDED(inputs->GetElement(index, &input)) && IsUsableWebEdit(input.Get())) {
                const int score = InputScore(input.Get());
                if (!best || score > bestScore) {
                    bestScore = score;
                    best = input;
                }
            }
        }
        return best;
    }

    ComPtr<IUIAutomationElement> SelectedBrowserTab(IUIAutomationElement* root) {
        auto tabType = PropertyCondition(automation.Get(), UIA_ControlTypePropertyId, VT_I4,
                                         UIA_TabItemControlTypeId);
        if (!root || !tabType) return nullptr;

        ComPtr<IUIAutomationElementArray> tabs;
        if (FAILED(root->FindAll(TreeScope_Descendants, tabType.Get(), &tabs)) || !tabs) {
            return nullptr;
        }

        int count = 0;
        tabs->get_Length(&count);
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> tab;
            if (FAILED(tabs->GetElement(index, &tab)) || !tab || IsInsideDocument(tab.Get())) {
                continue;  // ignore ARIA tabs inside the web page
            }

            ComPtr<IUnknown> unknown;
            if (FAILED(tab->GetCurrentPattern(UIA_SelectionItemPatternId, &unknown)) || !unknown) {
                continue;
            }
            ComPtr<IUIAutomationSelectionItemPattern> selection;
            if (FAILED(unknown.As(&selection)) || !selection) continue;

            BOOL isSelected = FALSE;
            if (SUCCEEDED(selection->get_CurrentIsSelected(&isSelected)) && isSelected) return tab;
        }
        return nullptr;
    }

    WebInputPickState Inspect(POINT point, HWND ownWindow) {
        HWND hit = ::WindowFromPoint(point);
        HWND root = hit ? ::GetAncestor(hit, GA_ROOT) : nullptr;
        if (!root || !::IsWindow(root) || !::IsWindowVisible(root) || ::IsIconic(root)) {
            candidate.Reset();
            lastWindow = nullptr;
            lastState = WebInputPickState::NoWindow;
            return lastState;
        }

        DWORD ownPid = 0;
        DWORD targetPid = 0;
        ::GetWindowThreadProcessId(ownWindow, &ownPid);
        ::GetWindowThreadProcessId(root, &targetPid);
        if (root == ownWindow || (ownPid != 0 && targetPid == ownPid)) {
            candidate.Reset();
            lastWindow = root;
            lastState = WebInputPickState::OwnWindow;
            return lastState;
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(::DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
            cloaked != 0) {
            candidate.Reset();
            lastWindow = root;
            lastState = WebInputPickState::NoWindow;
            return lastState;
        }

        // UI Automation tree walks are cross-process calls. Reuse the result
        // while the pointer remains over the same top-level window. A cheap
        // point walk still lets the user disambiguate pages with several
        // fields by releasing directly over the intended chat composer.
        if (root == lastWindow) {
            if (lastState == WebInputPickState::Valid && candidate.document) {
                ComPtr<IUIAutomationElement> pointed =
                    InputFromPoint(candidate.document.Get(), point);
                if (pointed) candidate.input = std::move(pointed);
            }
            return lastState;
        }
        candidate.Reset();
        lastWindow = root;

        if (!EnsureAutomation()) {
            lastState = WebInputPickState::NoWebDocument;
            return lastState;
        }

        ComPtr<IUIAutomationElement> rootElement;
        if (FAILED(automation->ElementFromHandle(root, &rootElement)) || !rootElement) {
            lastState = WebInputPickState::NoWebDocument;
            return lastState;
        }

        ComPtr<IUIAutomationElement> document = VisibleDocument(rootElement.Get(), point);
        if (!document) {
            lastState = WebInputPickState::NoWebDocument;
            return lastState;
        }

        ComPtr<IUIAutomationElement> input = EditableInput(document.Get(), &point);
        if (!input) {
            lastState = WebInputPickState::NoEditableInput;
            return lastState;
        }

        candidate.hwnd = root;
        candidate.document = std::move(document);
        candidate.input = std::move(input);
        candidate.browserTab = SelectedBrowserTab(rootElement.Get());
        candidate.name = ElementName(candidate.document.Get());
        if (candidate.name.empty()) candidate.name = WindowTitle(root);
        if (candidate.name.empty()) candidate.name = L"web tab";
        lastState = WebInputPickState::Valid;
        return lastState;
    }

    bool SelectRetainedBrowserTab(std::wstring& error) {
        if (!selected.browserTab) return true;

        ComPtr<IUnknown> unknown;
        if (FAILED(selected.browserTab->GetCurrentPattern(UIA_SelectionItemPatternId, &unknown)) ||
            !unknown) {
            error = L"The picked browser tab is no longer available. Pick it again.";
            return false;
        }
        ComPtr<IUIAutomationSelectionItemPattern> selection;
        if (FAILED(unknown.As(&selection)) || !selection) {
            error = L"Could not inspect the picked browser tab. Pick it again.";
            return false;
        }

        BOOL isSelected = FALSE;
        if (SUCCEEDED(selection->get_CurrentIsSelected(&isSelected)) && isSelected) return true;
        if (FAILED(selection->Select())) {
            error = L"Could not reactivate the picked browser tab. Pick it again.";
            return false;
        }
        return true;
    }

    bool FocusSelectedInput(std::wstring& error) {
        if (!selected.hwnd || !::IsWindow(selected.hwnd)) {
            error = L"The picked window has closed. Pick a web tab again.";
            return false;
        }
        if (!EnsureAutomation()) {
            error = L"Windows UI Automation is unavailable.";
            return false;
        }
        if (!SelectRetainedBrowserTab(error)) return false;

        if (::IsIconic(selected.hwnd)) ::ShowWindow(selected.hwnd, SW_RESTORE);
        ::SetForegroundWindow(selected.hwnd);
        HWND foreground = ::GetForegroundWindow();
        if (foreground != selected.hwnd && ::GetAncestor(foreground, GA_ROOT) != selected.hwnd) {
            error = L"Windows would not activate the picked window. Bring it forward and try again.";
            return false;
        }

        // First try the exact input retained at pick time. If the browser
        // recreated its accessibility tree while switching tabs, resolve the
        // visible document and chat input again in that same selected tab.
        if (selected.input && IsUsableWebEdit(selected.input.Get()) &&
            SUCCEEDED(selected.input->SetFocus())) {
            return true;
        }

        RECT bounds{};
        ::GetWindowRect(selected.hwnd, &bounds);
        const POINT center{bounds.left + (bounds.right - bounds.left) / 2,
                           bounds.top + (bounds.bottom - bounds.top) / 2};

        for (int attempt = 0; attempt < 6; ++attempt) {
            ComPtr<IUIAutomationElement> root;
            if (SUCCEEDED(automation->ElementFromHandle(selected.hwnd, &root)) && root) {
                ComPtr<IUIAutomationElement> document = VisibleDocument(root.Get(), center);
                ComPtr<IUIAutomationElement> input = EditableInput(document.Get());
                if (input && SUCCEEDED(input->SetFocus())) {
                    selected.document = std::move(document);
                    selected.input = std::move(input);
                    return true;
                }
            }
            if (attempt + 1 < 6) ::Sleep(20);
        }

        error = L"The picked tab no longer exposes its chat input. Pick the tab again.";
        return false;
    }

    bool SendText(const std::wstring& text, bool pressEnter, std::wstring& error) {
        error.clear();
        if (text.empty()) {
            error = L"The selected caption text is empty.";
            return false;
        }
        if (!selected.hwnd || !selected.input) {
            error = L"Pick a web tab before sending.";
            return false;
        }
        if (!FocusSelectedInput(error)) return false;

        bool valueSet = false;
        ComPtr<IUnknown> unknown;
        if (SUCCEEDED(selected.input->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
            ComPtr<IUIAutomationValuePattern> valuePattern;
            if (SUCCEEDED(unknown.As(&valuePattern)) && valuePattern) {
                BOOL readOnly = TRUE;
                if (SUCCEEDED(valuePattern->get_CurrentIsReadOnly(&readOnly)) && !readOnly) {
                    BSTR value = ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
                    if (value) {
                        valueSet = SUCCEEDED(valuePattern->SetValue(value));
                        ::SysFreeString(value);
                    }
                }
            }
        }

        // Contenteditable chat composers sometimes expose TextPattern only.
        // Real keyboard input also gives web frameworks their normal input
        // events, unlike writing an accessibility property directly.
        if (!valueSet && !ReplaceWithKeyboard(text)) {
            error =
                L"Windows could not type into the picked input. An elevated target may require "
                L"this app to run as administrator.";
            return false;
        }

        if (pressEnter && !InjectEnter()) {
            error = L"The text was inserted, but Windows could not press Enter in the target.";
            return false;
        }
        return true;
    }

    ComPtr<IUIAutomation> automation;
    Target candidate;
    Target selected;
    HWND lastWindow = nullptr;
    WebInputPickState lastState = WebInputPickState::NoWindow;
};

WebInputPicker::WebInputPicker() : m_impl(std::make_unique<Impl>()) {}
WebInputPicker::~WebInputPicker() = default;

WebInputPickState WebInputPicker::Inspect(POINT screenPoint, HWND ownWindow) {
    return m_impl->Inspect(screenPoint, ownWindow);
}

void WebInputPicker::ResetCandidate() {
    m_impl->candidate.Reset();
    m_impl->lastWindow = nullptr;
    m_impl->lastState = WebInputPickState::NoWindow;
}

bool WebInputPicker::CommitCandidate() {
    if (m_impl->lastState != WebInputPickState::Valid || !m_impl->candidate.hwnd ||
        !m_impl->candidate.document || !m_impl->candidate.input) {
        return false;
    }
    m_impl->selected = m_impl->candidate;
    return true;
}

bool WebInputPicker::CandidateValid() const {
    return m_impl->lastState == WebInputPickState::Valid && m_impl->candidate.hwnd;
}

HWND WebInputPicker::CandidateWindow() const {
    return m_impl->candidate.hwnd;
}

const std::wstring& WebInputPicker::CandidateName() const {
    return m_impl->candidate.name;
}

HWND WebInputPicker::SelectedWindow() const {
    return m_impl->selected.hwnd;
}

const std::wstring& WebInputPicker::SelectedName() const {
    return m_impl->selected.name;
}

bool WebInputPicker::SendText(const std::wstring& text, bool pressEnter, std::wstring& error) {
    return m_impl->SendText(text, pressEnter, error);
}
