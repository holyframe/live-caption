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

std::wstring WindowClass(HWND hwnd) {
    wchar_t name[256]{};
    const int copied = ::GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
    return std::wstring(name, static_cast<size_t>(std::max(copied, 0)));
}

struct ContentFrameSearch {
    RECT frame{};
    long long area = 0;
    bool found = false;
};

BOOL CALLBACK FindContentFrame(HWND child, LPARAM param) {
    auto* search = reinterpret_cast<ContentFrameSearch*>(param);
    if (!::IsWindowVisible(child) || !webinput::IsWebContentWindowClass(WindowClass(child))) {
        return TRUE;
    }
    RECT bounds{};
    if (!::GetWindowRect(child, &bounds) || ::IsRectEmpty(&bounds)) return TRUE;

    // Chromium keeps several render surfaces per window: besides the page there
    // is one covering the address bar for its dropdown, and a placeholder a few
    // pixels wide. They overlap, and enumeration order is z-order, so compare
    // sizes instead of taking the first match; the page viewport is the largest.
    const long long area = static_cast<long long>(bounds.right - bounds.left) *
                           static_cast<long long>(bounds.bottom - bounds.top);
    if (!search->found || area > search->area) {
        search->frame = bounds;
        search->area = area;
        search->found = true;
    }
    return TRUE;
}

// Locates the page viewport of a browser window. Chromium draws pages into a
// dedicated child window, so its rectangle separates the page from the toolbar
// and tab strip. Browsers without such a child fall back to the client area.
// With a point supplied, the page area must also contain it. The point is only
// ever compared against the viewport, never against the other render surfaces:
// the one behind the address bar dropdown reaches over the toolbar, and letting
// it answer for the page would make the toolbar look pickable.
bool WebContentFrame(HWND root, const POINT* requirePoint, RECT& frame) {
    ContentFrameSearch search;
    ::EnumChildWindows(root, &FindContentFrame, reinterpret_cast<LPARAM>(&search));
    if (search.found) {
        if (requirePoint && !::PtInRect(&search.frame, *requirePoint)) return false;
        frame = search.frame;
        return true;
    }

    RECT client{};
    if (!::GetClientRect(root, &client) || ::IsRectEmpty(&client)) return false;
    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!::ClientToScreen(root, &topLeft) || !::ClientToScreen(root, &bottomRight)) return false;
    frame = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return !requirePoint || ::PtInRect(&frame, *requirePoint) != FALSE;
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

// Clears the target and types the caption, optionally submitting it, as one
// injection. Windows hands queued keystrokes to whatever holds the keyboard
// focus at the moment it delivers each one, so every gap between calls here was
// a chance for the target to lose focus and swallow part of a caption: the text
// could land while the Enter that submits it did not, leaving the caption in a
// draft that the next send then overwrote.
bool ReplaceWithKeyboard(const std::wstring& text, bool pressEnter) {
    std::vector<INPUT> events;
    events.reserve(text.size() * 2 + 8);
    AddVirtualKey(events, VK_CONTROL);
    AddVirtualKey(events, 'A');
    AddVirtualKey(events, 'A', true);
    AddVirtualKey(events, VK_CONTROL, true);
    AddVirtualKey(events, VK_BACK);
    AddVirtualKey(events, VK_BACK, true);

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
        AddVirtualKey(events, VK_RETURN);
        AddVirtualKey(events, VK_RETURN, true);
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

// Long enough for a browser to notice an injected click, or a newly selected
// tab to lay itself out, before anything else disturbs it.
constexpr DWORD kClickSettleMs = 40;
constexpr DWORD kWaitStepMs = 20;
// Raising a window owned by another process, and the tab switch that may
// precede it, both finish well after the call that requested them.
constexpr DWORD kRaiseTimeoutMs = 700;
constexpr DWORD kFocusTimeoutMs = 400;
constexpr DWORD kMessageSyncTimeoutMs = 500;
// Keyboard focus reaching the browser is observable, but the page element it
// then hands focus to is chosen over IPC, which this delay covers.
constexpr DWORD kTypeSettleMs = 60;
constexpr int kClickAttempts = 3;

bool WindowOwnsPoint(HWND root, POINT point) {
    const HWND hit = ::WindowFromPoint(point);
    return hit && ::GetAncestor(hit, GA_ROOT) == root;
}

bool WindowIsActive(HWND root) {
    const HWND foreground = ::GetForegroundWindow();
    return foreground == root || ::GetAncestor(foreground, GA_ROOT) == root;
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

// Keyboard focus arriving anywhere in the browser window is the last step
// observable from outside. It cannot say whether the page or the address bar
// will receive the keys: Chromium keeps Windows-level focus on its frame window
// and routes keys to the page itself. Focus of the element inside the page also
// follows over IPC, which is what the settle delay after this covers. Best
// effort only; whether the browser ends up in front is what gates typing.
void WaitForKeyboardFocus(HWND root) {
    const DWORD thread = ::GetWindowThreadProcessId(root, nullptr);
    if (!thread) return;
    WaitUntil(
        [thread, root] {
            GUITHREADINFO info{};
            info.cbSize = sizeof(info);
            return ::GetGUIThreadInfo(thread, &info) && info.hwndFocus &&
                   ::GetAncestor(info.hwndFocus, GA_ROOT) == root;
        },
        kFocusTimeoutMs);
}

// Absolute pointer moves are expressed in a 0..65535 square spanning the whole
// virtual desktop. https://learn.microsoft.com/windows/win32/api/winuser/ns-winuser-mouseinput
bool AddAbsoluteMove(std::vector<INPUT>& events, POINT point) {
    const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) return false;

    INPUT move{};
    move.type = INPUT_MOUSE;
    move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    move.mi.dx = ::MulDiv(point.x - left, 65535, width - 1);
    move.mi.dy = ::MulDiv(point.y - top, 65535, height - 1);
    events.push_back(move);
    return true;
}

// Places the caret by clicking, for pages that expose no accessible input.
// The pointer is returned to where the user left it.
bool ClickScreenPoint(POINT point) {
    POINT original{};
    const bool restore = ::GetCursorPos(&original) != FALSE;

    // The move and the button travel as one batch, so a hand on the mouse
    // cannot slip between them and drag the click onto another window.
    std::vector<INPUT> events;
    events.reserve(3);
    if (!AddAbsoluteMove(events, point)) return false;
    INPUT button{};
    button.type = INPUT_MOUSE;
    button.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    events.push_back(button);
    button.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    events.push_back(button);
    if (!InjectEvents(events)) return false;

    ::Sleep(kClickSettleMs);
    if (restore) {
        events.clear();
        if (AddAbsoluteMove(events, original)) InjectEvents(events);
    }
    return true;
}

}  // namespace

struct PickerAutomation {
    struct Target {
        HWND hwnd = nullptr;
        std::wstring name;
        ComPtr<IUIAutomationElement> document;
        ComPtr<IUIAutomationElement> input;
        ComPtr<IUIAutomationElement> browserTab;
        // Set when the page exposes no accessibility tree and the caret has to
        // be placed by clicking the remembered viewport point instead.
        bool clicksPoint = false;
        webinput::PickAnchor anchor;

        void Reset() {
            hwnd = nullptr;
            name.clear();
            document.Reset();
            input.Reset();
            browserTab.Reset();
            clicksPoint = false;
            anchor = {};
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
            return InspectWithoutAccessiblePage(root, rootElement.Get(), point, retainTab);
        }

        ComPtr<IUIAutomationElement> input = EditableInput(document.Get(), &point);
        if (!input) {
            lastState = WebInputPickState::NoEditableInput;
            return lastState;
        }

        candidate.hwnd = root;
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

    // Browsers started with --disable-renderer-accessibility (several privacy
    // and anti-fingerprinting builds do this) never expose a page, so no input
    // element can be found however long we wait. Remember where the user
    // pointed inside the page instead; Send clicks there to place the caret.
    WebInputPickState InspectWithoutAccessiblePage(HWND root, IUIAutomationElement* rootElement,
                                                   POINT point, bool retainTab) {
        if (!webinput::IsBrowserWindowClass(WindowClass(root))) {
            lastState = WebInputPickState::NoWebDocument;
            return lastState;
        }

        RECT frame{};
        if (!WebContentFrame(root, &point, frame)) {
            // Typing into the address bar would navigate, so refuse anything
            // outside the page area.
            lastState = WebInputPickState::NoWebContent;
            return lastState;
        }

        candidate.hwnd = root;
        candidate.clicksPoint = true;
        candidate.anchor = webinput::MakePickAnchor(frame, point);
        if (retainTab) candidate.browserTab = SelectedBrowserTab(rootElement);
        candidate.name = WindowTitle(root);
        if (candidate.name.empty()) candidate.name = L"web tab";
        lastState = WebInputPickState::ValidByPoint;
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
        switched = true;
        return true;
    }

    bool WaitForSelectedActive(std::wstring& error) {
        const HWND hwnd = selected.hwnd;
        if (WaitUntil([hwnd] { return WindowIsActive(hwnd); }, kRaiseTimeoutMs)) return true;
        error = L"Windows would not activate the picked window. Bring it forward and try again.";
        return false;
    }

    bool ClickSelectedPoint(std::wstring& error) {
        const HWND hwnd = selected.hwnd;
        // Clicking the page activates the browser even when this app is not
        // allowed to raise it, but activation can still be lost to whatever the
        // user touches meanwhile. Keystrokes go wherever the keyboard focus
        // ended up, so never type without the browser in front; click the same
        // spot again instead, which is harmless for a text field.
        for (int attempt = 0; attempt < kClickAttempts; ++attempt) {
            if (attempt > 0) ::SetForegroundWindow(hwnd);

            RECT frame{};
            if (!WebContentFrame(hwnd, nullptr, frame)) {
                error = L"The picked browser no longer shows a page area. Pick the tab again.";
                return false;
            }

            // This click is aimed at bare screen coordinates, so anything
            // covering the browser would receive the caption instead of the
            // chat input. Activation was only requested, not completed, so the
            // window may still be climbing the z-order past this app; wait for
            // it rather than hit testing once and giving up.
            const POINT target = webinput::ResolvePickAnchor(frame, selected.anchor);
            if (!WaitUntil([hwnd, target] { return WindowOwnsPoint(hwnd, target); },
                           kRaiseTimeoutMs)) {
                error = L"Another window is covering the picked input. Uncover the browser, or "
                        L"turn off Always on top for this app, and try again.";
                return false;
            }
            if (!ClickScreenPoint(target)) {
                error = L"Windows could not click the picked input. An elevated target may "
                        L"require this app to run as administrator.";
                return false;
            }

            // Release keystrokes only once the browser has actually taken the
            // click and put the keyboard on its page surface. Typing straight
            // after the click used to lose captions whenever focus was still
            // on its way.
            WaitForMessagesProcessed(hwnd);
            WaitForKeyboardFocus(hwnd);
            ::Sleep(kTypeSettleMs);
            if (WindowIsActive(hwnd)) return true;
        }

        error = L"Windows would not activate the picked window. Bring it forward and try again.";
        return false;
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
        bool switchedTab = false;
        if (!SelectRetainedBrowserTab(switchedTab, error)) return false;

        if (::IsIconic(selected.hwnd)) ::ShowWindow(selected.hwnd, SW_RESTORE);
        ::SetForegroundWindow(selected.hwnd);

        // A tab that was just brought forward still has to lay itself out
        // before its input is where the pick recorded it.
        if (switchedTab) {
            WaitForMessagesProcessed(selected.hwnd);
            ::Sleep(kClickSettleMs);
        }

        if (selected.clicksPoint) return ClickSelectedPoint(error);
        if (!WaitForSelectedActive(error)) return false;

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
        if (!selected.hwnd || (!selected.input && !selected.clicksPoint)) {
            error = L"Pick a web tab before sending.";
            return false;
        }
        if (!FocusSelectedInput(error)) return false;

        bool valueSet = false;
        ComPtr<IUnknown> unknown;
        if (selected.input &&
            SUCCEEDED(selected.input->GetCurrentPattern(UIA_ValuePatternId, &unknown)) && unknown) {
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
        if (!valueSet) {
            if (!ReplaceWithKeyboard(text, pressEnter)) {
                error = L"Windows could not type into the picked input. An elevated target may "
                        L"require this app to run as administrator.";
                return false;
            }
            // Injecting keystrokes only queues them. Wait for the target to
            // work through them, then confirm it still held the keyboard the
            // whole time; otherwise the caption went to whatever took over,
            // and reporting success would hide that it never arrived.
            WaitForMessagesProcessed(selected.hwnd);
            if (!WindowIsActive(selected.hwnd)) {
                error = L"The picked window lost focus while the caption was being typed, so it "
                        L"may not have arrived. Try again.";
                return false;
            }
            return true;
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
        bool selectedClicksPoint = false;
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
                                candidate.hwnd && candidate.document && candidate.input;
        const bool byPoint = automation.lastState == WebInputPickState::ValidByPoint &&
                             candidate.hwnd && candidate.clicksPoint;
        return {automation.lastState,    candidate.hwnd,
                candidate.name,         accessible || byPoint,
                automation.selected.hwnd, automation.selected.name,
                automation.selected.clicksPoint};
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

bool WebInputPicker::SelectedClicksPoint() const {
    return m_impl->snapshot.selectedClicksPoint;
}

void WebInputPicker::ClearSelected() {
    m_impl->InvalidateHover();
    m_impl->ClearPreviewSnapshot();
    m_impl->Update([](PickerAutomation& automation) { automation.selected.Reset(); });
}

bool WebInputPicker::SendText(const std::wstring& text, bool pressEnter, std::wstring& error) {
    m_impl->InvalidateHover();
    auto result = m_impl->Invoke([text, pressEnter](PickerAutomation& automation) {
        std::wstring message;
        const bool sent = automation.SendText(text, pressEnter, message);
        return std::make_pair(sent, std::move(message));
    });
    error = std::move(result.second);
    return result.first;
}
