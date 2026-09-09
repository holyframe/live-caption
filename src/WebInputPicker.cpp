#include "WebInputPicker.h"
#include "WebInputValidation.h"

#include <dwmapi.h>
#include <objbase.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
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

// Native hit testing stays on the UI thread too, so leaving a target removes
// its outline immediately even when its accessibility provider is busy.
WebInputPickState PickWindowAt(POINT point, HWND ownWindow, HWND& root) {
    const HWND hit = ::WindowFromPoint(point);
    root = hit ? ::GetAncestor(hit, GA_ROOT) : nullptr;
    if (!root || !::IsWindow(root) || !::IsWindowVisible(root) || ::IsIconic(root)) {
        return WebInputPickState::NoWindow;
    }
    DWORD ownPid = 0, targetPid = 0;
    ::GetWindowThreadProcessId(ownWindow, &ownPid);
    ::GetWindowThreadProcessId(root, &targetPid);
    if (root == ownWindow || (ownPid != 0 && targetPid == ownPid)) {
        return WebInputPickState::OwnWindow;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return WebInputPickState::NoWindow;
    }
    return WebInputPickState::Checking;
}

bool IsUsableWebEdit(IUIAutomationElement* element) {
    if (!element) return false;

    CONTROLTYPEID type = 0;
    BOOL enabled = FALSE;
    BOOL focusable = FALSE;
    BOOL offscreen = TRUE;
    BOOL password = TRUE;
    if (FAILED(element->get_CurrentControlType(&type)) ||
        FAILED(element->get_CurrentIsEnabled(&enabled)) || !enabled ||
        FAILED(element->get_CurrentIsKeyboardFocusable(&focusable)) || !focusable ||
        FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
        FAILED(element->get_CurrentIsPassword(&password)) || password) {
        return false;
    }

    webinput::InputCapabilities capabilities;
    capabilities.type = type;
    capabilities.enabled = enabled != FALSE;
    capabilities.focusable = focusable != FALSE;
    capabilities.offscreen = offscreen != FALSE;
    capabilities.password = password != FALSE;

    // A writable ValuePattern is the usual HTML input/textarea shape.
    ComPtr<IUnknown> unknown;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
        ComPtr<IUIAutomationValuePattern> valuePattern;
        if (SUCCEEDED(unknown.As(&valuePattern)) && valuePattern) {
            BOOL readOnly = TRUE;
            if (SUCCEEDED(valuePattern->get_CurrentIsReadOnly(&readOnly))) {
                capabilities.value = readOnly ? webinput::WriteAccess::ReadOnly
                                              : webinput::WriteAccess::Writable;
                return webinput::CanEdit(capabilities);
            }
        }
    }

    // Rich web editors can be Documents rather than Edits. TextPattern itself
    // is not proof of editability: ask for its explicit read-only attribute.
    // https://learn.microsoft.com/windows/win32/winauto/uiauto-textattribute-ids
    unknown.Reset();
    if (SUCCEEDED(element->GetCurrentPattern(UIA_TextPatternId, &unknown)) && unknown) {
        ComPtr<IUIAutomationTextPattern> text;
        ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(unknown.As(&text)) && text &&
            SUCCEEDED(text->get_DocumentRange(&range)) && range) {
            VARIANT readOnly{};
            if (SUCCEEDED(range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readOnly)) &&
                readOnly.vt == VT_BOOL) {
                capabilities.text = readOnly.boolVal == VARIANT_FALSE
                                        ? webinput::WriteAccess::Writable
                                        : webinput::WriteAccess::ReadOnly;
            }
            ::VariantClear(&readOnly);
        }
    }
    return webinput::CanEdit(capabilities);
}

void AddVirtualKey(std::vector<INPUT>& events, WORD key, bool keyUp = false) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    events.push_back(input);
}

// The Send button is a mouse click, so no modifier is held. The hotkey is
// typically Shift+Z: even after the physical keys come up, the target may not
// have processed those key-ups yet, and chat composers treat Shift+Enter as a
// newline instead of a send. Force every modifier up in the same injection as
// the caption so both paths type the same keys.
void AddModifierReleases(std::vector<INPUT>& events) {
    const WORD keys[] = {VK_LSHIFT, VK_RSHIFT, VK_SHIFT,      VK_LCONTROL, VK_RCONTROL, VK_CONTROL,
                         VK_LMENU,  VK_RMENU,  VK_MENU,       VK_LWIN,     VK_RWIN};
    for (WORD key : keys) AddVirtualKey(events, key, true);
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

// Collapses any selection at the end of the composer, appends the caption, and
// optionally presses Enter in one injection. Keeping Enter in the same batch
// closes the focus gap that can otherwise leave a caption in the field without
// submitting it. Ctrl+End preserves all text that was already present.
bool AppendWithKeyboard(const std::wstring& text, bool pressEnter) {
    std::vector<INPUT> events;
    events.reserve(text.size() * 2 + 24);
    AddModifierReleases(events);
    AddVirtualKey(events, VK_CONTROL);
    AddVirtualKey(events, VK_END);
    AddVirtualKey(events, VK_END, true);
    AddVirtualKey(events, VK_CONTROL, true);

    for (size_t index = 0; index < text.size(); ++index) {
        wchar_t character = text[index];
        if (character == L'\r') {
            if (index + 1 < text.size() && text[index + 1] == L'\n') continue;
            character = L'\n';
        }
        AddUnicodeKey(events, character);
        AddUnicodeKey(events, character, true);
    }

    if (pressEnter) {
        AddModifierReleases(events);
        AddVirtualKey(events, VK_RETURN);
        AddVirtualKey(events, VK_RETURN, true);
    }
    return InjectEvents(events);
}

// Long enough for a newly selected tab to lay itself out before validation.
constexpr DWORD kTabSettleMs = 40;
constexpr DWORD kWaitStepMs = 20;
// Raising a window owned by another process, and the tab switch that may
// precede it, both finish well after the call that requested them.
constexpr DWORD kRaiseTimeoutMs = 700;
constexpr DWORD kFocusTimeoutMs = 400;
constexpr DWORD kMessageSyncTimeoutMs = 500;
constexpr DWORD kInsertConfirmTimeoutMs = 1200;
constexpr DWORD kSubmitConfirmTimeoutMs = 1200;

bool WindowIsActive(HWND root) {
    const HWND foreground = ::GetForegroundWindow();
    return foreground == root || ::GetAncestor(foreground, GA_ROOT) == root;
}

// The Send button can always raise the target: this app is the foreground
// window then. A global hotkey only grants that for a moment, and waiting for
// its keys to come up outlasts the grant. Attach the calling thread to whoever
// currently holds the keyboard so SetForegroundWindow is allowed either way.
bool ActivateWindow(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return false;
    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
    if (WindowIsActive(hwnd) || ::SetForegroundWindow(hwnd)) return true;

    const HWND foreground = ::GetForegroundWindow();
    const DWORD currentThread = ::GetCurrentThreadId();
    const DWORD targetThread = ::GetWindowThreadProcessId(hwnd, nullptr);
    const DWORD foregroundThread =
        foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;

    if (foregroundThread && foregroundThread != currentThread) {
        ::AttachThreadInput(currentThread, foregroundThread, TRUE);
    }
    if (targetThread && targetThread != currentThread && targetThread != foregroundThread) {
        ::AttachThreadInput(currentThread, targetThread, TRUE);
    }

    ::BringWindowToTop(hwnd);
    const bool raised = ::SetForegroundWindow(hwnd) != FALSE || WindowIsActive(hwnd);

    if (targetThread && targetThread != currentThread && targetThread != foregroundThread) {
        ::AttachThreadInput(currentThread, targetThread, FALSE);
    }
    if (foregroundThread && foregroundThread != currentThread) {
        ::AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
    return raised;
}

template <typename Predicate>
bool WaitUntil(Predicate ready, DWORD timeoutMs) {
    for (DWORD waited = 0;; waited += kWaitStepMs) {
        if (ready()) return true;
        if (waited >= timeoutMs) return false;
        ::Sleep(kWaitStepMs);
    }
}

// Blocks until the target's UI thread has drained everything queued ahead of
// this message, which is how an injected click is known to be handled.
void WaitForMessagesProcessed(HWND window) {
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                          kMessageSyncTimeoutMs, &result);
}

}  // namespace

struct PickerAutomation {
    struct Target {
        HWND hwnd = nullptr;
        DWORD processId = 0;
        std::wstring name;
        ComPtr<IUIAutomationElement> document;
        ComPtr<IUIAutomationElement> input;
        ComPtr<IUIAutomationElement> browserTab;

        void Reset() {
            hwnd = nullptr;
            processId = 0;
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
        ComPtr<IUIAutomation2> automation2;
        if (automation && SUCCEEDED(automation.As(&automation2))) {
            automation2->put_ConnectionTimeout(1000);
            automation2->put_TransactionTimeout(1000);
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

    // A browser whose renderer accessibility is switched off can still expose a
    // Document standing in for the page, with nothing whatsoever beneath it.
    // A real page always has children, even one that holds no editable field,
    // so this separates "no page exposed" from "page without an input".
    bool DocumentExposesPage(IUIAutomationElement* document) {
        ComPtr<IUIAutomationTreeWalker> walker;
        if (!document || FAILED(automation->get_ControlViewWalker(&walker)) || !walker) {
            return true;  // Cannot tell; keep using the accessibility path.
        }
        ComPtr<IUIAutomationElement> child;
        return SUCCEEDED(walker->GetFirstChildElement(document, &child)) && child;
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

        const CONTROLTYPEID types[] = {UIA_EditControlTypeId, UIA_DocumentControlTypeId,
                                       UIA_CustomControlTypeId, UIA_PaneControlTypeId,
                                       UIA_GroupControlTypeId};
        std::vector<ComPtr<IUIAutomationCondition>> conditions;
        std::vector<IUIAutomationCondition*> rawConditions;
        for (CONTROLTYPEID type : types) {
            auto condition = PropertyCondition(automation.Get(), UIA_ControlTypePropertyId,
                                                VT_I4, type);
            if (!condition) return nullptr;
            rawConditions.push_back(condition.Get());
            conditions.push_back(std::move(condition));
        }
        ComPtr<IUIAutomationCondition> edit;
        if (FAILED(automation->CreateOrConditionFromNativeArray(
                rawConditions.data(), static_cast<int>(rawConditions.size()), &edit))) {
            return nullptr;
        }
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
        if (FAILED(document->FindAll(TreeScope_Subtree, usableEdit.Get(), &inputs)) || !inputs) {
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

    WebInputPickState Inspect(POINT point, HWND ownWindow, bool forceRefresh) {
        HWND root = nullptr;
        const auto hitState = PickWindowAt(point, ownWindow, root);
        if (hitState != WebInputPickState::Checking) {
            candidate.Reset();
            lastWindow = nullptr;
            lastState = hitState;
            return lastState;
        }

        // Chromium can populate its accessibility tree after the first query.
        // Never cache a negative result for the whole drag. Valid results also
        // expire so tab switches/navigation in the same HWND are rechecked.
        const ULONGLONG now = ::GetTickCount64();
        if (webinput::CanReuseInspection(root, lastWindow, now, lastInspectionTick, forceRefresh)) {
            // Hover is window-wide. Resolve the precise field on drop instead
            // of walking the browser tree on every cached mouse movement.
            return lastState;
        }
        candidate.Reset();
        lastWindow = root;
        lastState = InspectWindow(root, point, forceRefresh);
        // A slow provider must not consume the cache lifetime during the scan.
        lastInspectionTick = ::GetTickCount64();
        return lastState;
    }

    WebInputPickState InspectWindow(HWND root, POINT point, bool retainTab) {
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
        if (!document || !DocumentExposesPage(document.Get())) {
            lastState = WebInputPickState::NoWebDocument;
            return lastState;
        }

        ComPtr<IUIAutomationElement> input = EditableInput(document.Get(), &point);
        if (!input) {
            lastState = WebInputPickState::NoEditableInput;
            return lastState;
        }

        candidate.hwnd = root;
        ::GetWindowThreadProcessId(root, &candidate.processId);
        if (!candidate.processId) {
            candidate.Reset();
            lastState = WebInputPickState::NoWindow;
            return lastState;
        }
        candidate.document = std::move(document);
        candidate.input = std::move(input);
        // Tab enumeration is needed only for the final retained target.
        if (retainTab) candidate.browserTab = SelectedBrowserTab(rootElement.Get());
        candidate.name = ElementName(candidate.document.Get());
        if (candidate.name.empty()) candidate.name = WindowTitle(root);
        if (candidate.name.empty()) candidate.name = L"web tab";
        lastState = WebInputPickState::Valid;
        return lastState;
    }

    bool SelectRetainedBrowserTab(bool& switched, std::wstring& error) {
        switched = false;
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
        if (!WaitUntil(
                [&selection] {
                    BOOL selectedNow = FALSE;
                    return SUCCEEDED(selection->get_CurrentIsSelected(&selectedNow)) && selectedNow;
                },
                kRaiseTimeoutMs)) {
            error = L"The picked browser tab did not become active. Pick it again.";
            return false;
        }
        switched = true;
        return true;
    }

    bool WaitForSelectedActive(std::wstring& error) {
        const HWND hwnd = selected.hwnd;
        if (WaitUntil([hwnd] { return WindowIsActive(hwnd); }, kRaiseTimeoutMs)) return true;
        error = L"Windows would not activate the picked window. Bring it forward and try again.";
        return false;
    }

    bool IsSameOrDescendant(IUIAutomationElement* element, IUIAutomationElement* ancestor) {
        if (!element || !ancestor) return false;
        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) return false;

        ComPtr<IUIAutomationElement> current = element;
        for (int depth = 0; depth < 64 && current; ++depth) {
            BOOL same = FALSE;
            if (SUCCEEDED(automation->CompareElements(current.Get(), ancestor, &same)) && same) {
                return true;
            }
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.Get(), &parent)) || !parent) break;
            current = std::move(parent);
        }
        return false;
    }

    bool ExactInputIsAvailable() {
        return selected.document && selected.input &&
               IsVisibleDocument(selected.document.Get()) &&
               DocumentExposesPage(selected.document.Get()) &&
               IsUsableWebEdit(selected.input.Get()) &&
               IsSameOrDescendant(selected.input.Get(), selected.document.Get());
    }

    bool ExactInputHasFocus() {
        ComPtr<IUIAutomationElement> focused;
        return SUCCEEDED(automation->GetFocusedElement(&focused)) && focused &&
               IsSameOrDescendant(focused.Get(), selected.input.Get());
    }

    bool WaitForExactInputFocus() {
        return WaitUntil([this] { return ExactInputHasFocus(); }, kFocusTimeoutMs);
    }

    bool ReadSelectedText(std::wstring& value) {
        value.clear();
        if (!selected.input) return false;

        ComPtr<IUnknown> unknown;
        if (SUCCEEDED(selected.input->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
            ComPtr<IUIAutomationValuePattern> pattern;
            if (SUCCEEDED(unknown.As(&pattern)) && pattern) {
                BSTR text = nullptr;
                const HRESULT read = pattern->get_CurrentValue(&text);
                if (SUCCEEDED(read)) {
                    value = BstrToString(text);
                    ::SysFreeString(text);
                    return true;
                }
                ::SysFreeString(text);
            }
        }

        unknown.Reset();
        if (SUCCEEDED(selected.input->GetCurrentPattern(UIA_TextPatternId, &unknown)) && unknown) {
            ComPtr<IUIAutomationTextPattern> pattern;
            ComPtr<IUIAutomationTextRange> range;
            if (SUCCEEDED(unknown.As(&pattern)) && pattern &&
                SUCCEEDED(pattern->get_DocumentRange(&range)) && range) {
                BSTR text = nullptr;
                const HRESULT read = range->GetText(-1, &text);
                if (SUCCEEDED(read)) {
                    value = BstrToString(text);
                    ::SysFreeString(text);
                    return true;
                }
                ::SysFreeString(text);
            }
        }
        return false;
    }

    bool WaitForSelectedText(const std::wstring& expected, DWORD timeoutMs) {
        return WaitUntil(
            [this, &expected] {
                std::wstring actual;
                return ReadSelectedText(actual) && webinput::TextMatches(actual, expected);
            },
            timeoutMs);
    }

    bool WaitForSelectedEmpty(DWORD timeoutMs) {
        return WaitUntil(
            [this] {
                std::wstring actual;
                return ReadSelectedText(actual) && webinput::ComposerIsEmpty(actual);
            },
            timeoutMs);
    }

    bool FocusSelectedInput(std::wstring& error) {
        if (!selected.hwnd || !::IsWindow(selected.hwnd)) {
            error = L"The picked window has closed. Pick a web tab again.";
            return false;
        }
        DWORD processId = 0;
        ::GetWindowThreadProcessId(selected.hwnd, &processId);
        if (!processId || processId != selected.processId) {
            error = L"The picked browser window has been replaced. Pick the tab again.";
            return false;
        }
        if (!EnsureAutomation()) {
            error = L"Windows UI Automation is unavailable.";
            return false;
        }
        bool switchedTab = false;
        if (!SelectRetainedBrowserTab(switchedTab, error)) return false;

        ActivateWindow(selected.hwnd);

        // A tab that was just brought forward still has to lay itself out
        // before its input is where the pick recorded it.
        if (switchedTab) {
            WaitForMessagesProcessed(selected.hwnd);
            ::Sleep(kTabSettleMs);
        }

        if (!WaitForSelectedActive(error)) return false;

        if (!WaitUntil([this] { return ExactInputIsAvailable(); }, kFocusTimeoutMs)) {
            error = L"The picked tab or its exact chat input changed. Pick it again.";
            return false;
        }
        if (FAILED(selected.input->SetFocus()) || !WaitForExactInputFocus()) {
            error = L"Windows could not focus the exact picked chat input. Pick it again.";
            return false;
        }
        return true;
    }

    WebInputSendResult SendText(const std::wstring& text, bool pressEnter,
                                std::wstring& message) {
        message.clear();
        if (text.empty()) {
            message = L"The selected caption text is empty.";
            return WebInputSendResult::Failed;
        }
        if (!selected.hwnd || !selected.document || !selected.input) {
            message = L"Pick a web tab before sending.";
            return WebInputSendResult::Failed;
        }
        if (!FocusSelectedInput(message)) return WebInputSendResult::Failed;

        std::wstring previousText;
        if (!ReadSelectedText(previousText)) {
            message = L"The picked input's existing text could not be read, so it was left "
                      L"unchanged.";
            return WebInputSendResult::Failed;
        }
        std::wstring expectedText = previousText;
        expectedText += text;

        // Real keystrokes give Chromium and page frameworks their normal input
        // events. The existing value was read first and Ctrl+End only collapses
        // the selection, so an existing draft is never erased. Enter shares
        // this batch to keep focus from changing between insertion and submit.
        if (!AppendWithKeyboard(text, pressEnter)) {
            message = pressEnter
                          ? L"Windows could not append the caption and press Enter in the picked "
                            L"input. An elevated target may require this app to run as "
                            L"administrator."
                          : L"Windows could not append the caption in the picked input. An elevated "
                            L"target may require this app to run as administrator.";
            return WebInputSendResult::Failed;
        }
        WaitForMessagesProcessed(selected.hwnd);
        if (!WindowIsActive(selected.hwnd)) {
            message = L"The picked window lost focus while the caption was being delivered. "
                      L"Check it before retrying.";
            return WebInputSendResult::Failed;
        }

        if (pressEnter) {
            if (WaitForSelectedEmpty(kSubmitConfirmTimeoutMs)) {
                return WebInputSendResult::Submitted;
            }
            message = L"The caption and Enter were delivered together, but submission could not "
                      L"be confirmed. Check the browser before sending this caption again.";
            return WebInputSendResult::SubmissionUnconfirmed;
        }

        if (!WaitForExactInputFocus()) {
            message = L"The exact picked input lost focus while the caption was being appended. "
                      L"Pick it again before retrying.";
            return WebInputSendResult::Failed;
        }
        if (!WaitForSelectedText(expectedText, kInsertConfirmTimeoutMs)) {
            message = L"Text input was attempted, but its contents could not be verified. "
                      L"Check the browser before retrying.";
            return WebInputSendResult::Failed;
        }
        return WebInputSendResult::Inserted;
    }

    ComPtr<IUIAutomation> automation;
    Target candidate;
    Target selected;
    HWND lastWindow = nullptr;
    ULONGLONG lastInspectionTick = 0;
    WebInputPickState lastState = WebInputPickState::NoWindow;
};

struct WebInputPicker::Impl {
    // COM objects never cross the worker boundary. Only HWNDs, states and names
    // are copied back to the UI, including when committing/clearing a target.
    struct Snapshot {
        WebInputPickState state = WebInputPickState::NoWindow;
        HWND candidateWindow = nullptr;
        std::wstring candidateName;
        bool candidateValid = false;
        HWND selectedWindow = nullptr;
        std::wstring selectedName;
    } snapshot;

    struct HoverRequest {
        POINT point{};
        HWND ownWindow = nullptr;
        HWND window = nullptr;
        ULONGLONG generation = 0;
    };
    struct HoverResult {
        HoverRequest request;
        Snapshot snapshot;
        ULONGLONG completedAt = 0;
    };

    static Snapshot Capture(const PickerAutomation& automation) {
        const auto& candidate = automation.candidate;
        const bool accessible = automation.lastState == WebInputPickState::Valid &&
                                 candidate.hwnd && candidate.processId &&
                                 candidate.document && candidate.input;
        return {automation.lastState,    candidate.hwnd,
                candidate.name,         accessible,
                automation.selected.hwnd, automation.selected.name};
    }

    void ClearPreviewSnapshot(WebInputPickState state = WebInputPickState::NoWindow) {
        snapshot.state = state;
        snapshot.candidateWindow = nullptr;
        snapshot.candidateName.clear();
        snapshot.candidateValid = false;
        commitReady = false;
    }

    // Caller holds mutex. Generation changes invalidate an in-flight scan too;
    // no COM object is released or touched from this thread.
    void InvalidateHoverLocked() {
        ++hoverGeneration;
        pendingHover.reset();
        completedHover.reset();
        hoverWindow = nullptr;
        hoverOwnWindow = nullptr;
        hasHoverResult = false;
    }

    void InvalidateHover() {
        std::lock_guard lock(mutex);
        InvalidateHoverLocked();
        commitReady = false;
    }

    WebInputPickState Preview(POINT point, HWND ownWindow, HWND window,
                             WebInputPickState hitState, ULONGLONG now) {
        std::lock_guard lock(mutex);
        commitReady = false;
        if (hitState != WebInputPickState::Checking) {
            InvalidateHoverLocked();
            ClearPreviewSnapshot(hitState);
            return snapshot.state;
        }
        if (window != hoverWindow || ownWindow != hoverOwnWindow) {
            InvalidateHoverLocked();
            hoverWindow = window;
            hoverOwnWindow = ownWindow;
            ClearPreviewSnapshot(WebInputPickState::Checking);
        }
        if (completedHover) {
            // Apply candidate data only: a preview never owns selection state.
            snapshot.state = completedHover->snapshot.state;
            snapshot.candidateWindow = completedHover->snapshot.candidateWindow;
            snapshot.candidateName = std::move(completedHover->snapshot.candidateName);
            snapshot.candidateValid = completedHover->snapshot.candidateValid;
            hoverCompletedAt = completedHover->completedAt;
            hasHoverResult = true;
            completedHover.reset();
        }
        const bool fresh = hasHoverResult &&
            webinput::CanReuseInspection(window, hoverWindow,
                                        std::max(now, hoverCompletedAt), hoverCompletedAt, false);
        const bool running = runningHover && runningHover->generation == hoverGeneration;
        if (!fresh && !running) {
            // One replaceable slot, never one queued job per mouse message.
            pendingHover = HoverRequest{point, ownWindow, window, hoverGeneration};
            wake.notify_one();
        }
        return snapshot.state;
    }

    Impl() : worker([this] { Run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            pendingHover.reset();
        }
        wake.notify_one();
        worker.join();
    }

    template <typename Function>
    void Post(Function function) {
        {
            std::lock_guard lock(mutex);
            jobs.emplace_back(std::move(function));
        }
        wake.notify_one();
    }

    template <typename Function>
    auto Invoke(Function function) -> std::invoke_result_t<Function, PickerAutomation&> {
        using Result = std::invoke_result_t<Function, PickerAutomation&>;
        auto task = std::make_shared<std::packaged_task<Result(PickerAutomation&)>>(
            std::move(function));
        auto result = task->get_future();
        Post([task](PickerAutomation& automation) { (*task)(automation); });
        return result.get();
    }

    template <typename Function>
    void Update(Function function) {
        snapshot = Invoke([function = std::move(function)](PickerAutomation& automation) {
            function(automation);
            return Capture(automation);
        });
    }

    void Run() {
        // UI Automation clients must use an MTA thread without owned windows.
        // https://learn.microsoft.com/windows/win32/winauto/uiauto-threading
        const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            PickerAutomation automation;
            for (;;) {
                std::function<void(PickerAutomation&)> job;
                std::optional<HoverRequest> hover;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [this] { return stopping || !jobs.empty() || pendingHover; });
                    if (jobs.empty() && stopping) break;
                    if (!jobs.empty()) {
                        // Drop/send/reset take priority over queued previews.
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    } else {
                        hover = std::move(pendingHover);
                        pendingHover.reset();
                        runningHover = hover;
                    }
                }
                if (job) {
                    job(automation);
                    continue;
                }
                Snapshot result;
#ifdef WEBINPUT_PICKER_TESTING
                if (inspectHoverForTesting) result = inspectHoverForTesting(*hover);
                else
#endif
                {
                    // A newer generation may return to the same HWND after a
                    // cancelled drag; never borrow that older drag's cache.
                    if (workerHoverGeneration != hover->generation) {
                        automation.lastWindow = nullptr;
                        workerHoverGeneration = hover->generation;
                    }
                    automation.Inspect(hover->point, hover->ownWindow, false);
                    result = Capture(automation);
                }
                {
                    std::lock_guard lock(mutex);
                    runningHover.reset();
                    // The browser can move under the original point while UIA
                    // is busy. Do not display a different window's candidate.
                    if (hover->generation == hoverGeneration &&
                        (!result.candidateWindow || result.candidateWindow == hover->window)) {
                        completedHover = HoverResult{*hover, std::move(result), ::GetTickCount64()};
                    }
                }
            }
        } // Release every UIA interface on its owning thread before COM exits.
        if (SUCCEEDED(initialized)) ::CoUninitialize();
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void(PickerAutomation&)>> jobs;
    std::optional<HoverRequest> pendingHover;
    std::optional<HoverRequest> runningHover;
    std::optional<HoverResult> completedHover;
    ULONGLONG hoverGeneration = 0;
    ULONGLONG workerHoverGeneration = 0;
    HWND hoverWindow = nullptr;
    HWND hoverOwnWindow = nullptr;
    ULONGLONG hoverCompletedAt = 0;
    bool hasHoverResult = false;
    bool commitReady = false;
#ifdef WEBINPUT_PICKER_TESTING
    std::function<Snapshot(const HoverRequest&)> inspectHoverForTesting;
#endif
    bool stopping = false;
    std::thread worker;
};

WebInputPicker::WebInputPicker() : m_impl(std::make_unique<Impl>()) {}
WebInputPicker::~WebInputPicker() = default;

WebInputPickState WebInputPicker::Preview(POINT screenPoint, HWND ownWindow) {
    HWND window = nullptr;
    const auto state = PickWindowAt(screenPoint, ownWindow, window);
    return m_impl->Preview(screenPoint, ownWindow, window, state, ::GetTickCount64());
}

WebInputPickState WebInputPicker::Inspect(POINT screenPoint, HWND ownWindow, bool forceRefresh) {
    m_impl->InvalidateHover();
    m_impl->Update([=](PickerAutomation& automation) {
        automation.Inspect(screenPoint, ownWindow, forceRefresh);
    });
    m_impl->commitReady = forceRefresh && CandidateValid();
    return m_impl->snapshot.state;
}

void WebInputPicker::ResetCandidate() {
    m_impl->InvalidateHover();
    m_impl->ClearPreviewSnapshot();
    m_impl->Post([](PickerAutomation& automation) {
        automation.candidate.Reset();
        automation.lastWindow = nullptr;
        automation.lastInspectionTick = 0;
        automation.lastState = WebInputPickState::NoWindow;
    });
}

bool WebInputPicker::CommitCandidate() {
    if (!m_impl->commitReady || !CandidateValid()) return false;
    m_impl->InvalidateHover();
    m_impl->Update([](PickerAutomation& automation) { automation.selected = automation.candidate; });
    return true;
}

bool WebInputPicker::CandidateValid() const {
    return m_impl->snapshot.candidateValid;
}

HWND WebInputPicker::CandidateWindow() const {
    return m_impl->snapshot.candidateWindow;
}

const std::wstring& WebInputPicker::CandidateName() const {
    return m_impl->snapshot.candidateName;
}

HWND WebInputPicker::SelectedWindow() const {
    return m_impl->snapshot.selectedWindow;
}

const std::wstring& WebInputPicker::SelectedName() const {
    return m_impl->snapshot.selectedName;
}

void WebInputPicker::RaiseSelectedWindow() {
    const HWND target = m_impl->snapshot.selectedWindow;
    if (!target || !::IsWindow(target)) return;
    // Called from the hotkey handler while Windows still grants this process
    // the right to raise another window. Send later uses ActivateWindow, which
    // can attach threads if that grant has expired.
    if (::IsIconic(target)) ::ShowWindow(target, SW_RESTORE);
    ::SetForegroundWindow(target);
}

void WebInputPicker::ClearSelected() {
    m_impl->InvalidateHover();
    m_impl->ClearPreviewSnapshot();
    m_impl->Update([](PickerAutomation& automation) { automation.selected.Reset(); });
}

WebInputSendResult WebInputPicker::SendText(const std::wstring& text, bool pressEnter,
                                            std::wstring& message) {
    m_impl->InvalidateHover();
    auto result = m_impl->Invoke([text, pressEnter](PickerAutomation& automation) {
        std::wstring detail;
        const auto outcome = automation.SendText(text, pressEnter, detail);
        return std::make_pair(outcome, std::move(detail));
    });
    message = std::move(result.second);
    return result.first;
}
