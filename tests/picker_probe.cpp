// Read-only probe: prints UIA structure/flags, never page text or input values.
#include "../src/WebInputPicker.cpp"
#include <cstdio>
#include <chrono>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::puts("Usage: picker_probe.exe <decimal window handle> [expected final state]");
        return 2;
    }
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    const HWND hwnd = reinterpret_cast<HWND>(_wcstoui64(argv[1], nullptr, 10));
    int result = 0;
    const int expected = argc == 3 ? _wtoi(argv[2]) : -1;
    int finalState = -1;
    bool sawExpectedPreview = false;
    double maxPreviewMs = 0;
    {
        ComPtr<IUIAutomation> uia;
        HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&uia));
        if (FAILED(hr)) return 2;
        ComPtr<IUIAutomation2> uia2;
        if (SUCCEEDED(uia.As(&uia2))) {
            uia2->put_ConnectionTimeout(1000);
            uia2->put_TransactionTimeout(1000);
        }
        std::printf("window visible=%d minimized=%d\n", ::IsWindowVisible(hwnd), ::IsIconic(hwnd));
        WebInputPicker picker;
        const int passes = expected >= 0 ? 12 : 3;
        for (int pass = 0; pass < passes; ++pass) {
            DWORD cloaked = 0;
            ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            std::printf("target visible=%d minimized=%d cloaked=%lu\n",
                        ::IsWindowVisible(hwnd), ::IsIconic(hwnd), cloaked);
            RECT bounds{};
            ::GetWindowRect(hwnd, &bounds);
            const POINT point{bounds.left + (bounds.right - bounds.left) / 2,
                              bounds.top + (bounds.bottom - bounds.top) / 2};
            const HWND hit = ::WindowFromPoint(point);
            if (hit && ::GetAncestor(hit, GA_ROOT) == hwnd) {
                const auto started = std::chrono::steady_clock::now();
                finalState = static_cast<int>(expected >= 0 ? picker.Preview(point, nullptr)
                                                            : picker.Inspect(point, nullptr));
                if (expected >= 0) {
                    maxPreviewMs = std::max(maxPreviewMs,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started).count());
                    sawExpectedPreview = sawExpectedPreview || finalState == expected;
                }
                std::printf("picker state=%d (Valid=%d)\n", finalState,
                            static_cast<int>(WebInputPickState::Valid));
            } else {
                std::puts("picker point check skipped: window center is occluded");
            }
            ComPtr<IUIAutomationElement> root;
            hr = uia->ElementFromHandle(hwnd, &root);
            if (FAILED(hr) || !root) {
                std::printf("ElementFromHandle failed: 0x%08lX\n", hr);
                result = 1;
                break;
            }
            auto documentType = PropertyCondition(uia.Get(), UIA_ControlTypePropertyId, VT_I4,
                                                  UIA_DocumentControlTypeId);
            ComPtr<IUIAutomationElementArray> docs;
            hr = root->FindAll(TreeScope_Subtree, documentType.Get(), &docs);
            int count = 0;
            if (docs) docs->get_Length(&count);
            std::printf("pass=%d documents=%d hr=0x%08lX\n", pass, count, hr);
            auto focusable = PropertyCondition(uia.Get(), UIA_IsKeyboardFocusablePropertyId,
                                               VT_BOOL, TRUE);
            for (int d = 0; d < count && d < 12; ++d) {
                ComPtr<IUIAutomationElement> doc;
                docs->GetElement(d, &doc);
                if (!doc) continue;
                BOOL offscreen = TRUE;
                doc->get_CurrentIsOffscreen(&offscreen);
                // A browser with its renderer accessibility switched off can
                // still expose an empty Document standing in for the page.
                ComPtr<IUIAutomationCondition> anyChild;
                uia->CreateTrueCondition(&anyChild);
                ComPtr<IUIAutomationElementArray> children;
                doc->FindAll(TreeScope_Children, anyChild.Get(), &children);
                int childCount = 0;
                if (children) children->get_Length(&childCount);
                std::printf("  document=%d offscreen=%d usable=%d children=%d\n", d, offscreen,
                            IsUsableWebEdit(doc.Get()), childCount);
                ComPtr<IUIAutomationElementArray> nodes;
                doc->FindAll(TreeScope_Subtree, focusable.Get(), &nodes);
                int nodeCount = 0;
                if (nodes) nodes->get_Length(&nodeCount);
                std::printf("  focusable=%d\n", nodeCount);
                for (int n = 0; n < nodeCount && n < 1000; ++n) {
                    ComPtr<IUIAutomationElement> node;
                    nodes->GetElement(n, &node);
                    if (!node) continue;
                    CONTROLTYPEID type = 0;
                    node->get_CurrentControlType(&type);
                    if (type != UIA_EditControlTypeId && type != UIA_DocumentControlTypeId &&
                        type != UIA_PaneControlTypeId && type != UIA_CustomControlTypeId) continue;
                    BOOL enabled = FALSE, password = TRUE;
                    node->get_CurrentIsEnabled(&enabled);
                    node->get_CurrentIsOffscreen(&offscreen);
                    node->get_CurrentIsPassword(&password);
                    ComPtr<IUIAutomationValuePattern> value;
                    node->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value));
                    BOOL readOnly = TRUE;
                    if (value) value->get_CurrentIsReadOnly(&readOnly);
                    ComPtr<IUIAutomationTextPattern> text;
                    node->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text));
                    VARIANT attr{};
                    ComPtr<IUIAutomationTextRange> range;
                    if (text && SUCCEEDED(text->get_DocumentRange(&range)) && range) {
                        range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &attr);
                    }
                    std::printf("    type=%d enabled=%d offscreen=%d password=%d value=%d "
                                "readonly=%d text=%d attrType=%u attrBool=%d usable=%d\n",
                                type, enabled, offscreen, password, value != nullptr, readOnly,
                                text != nullptr, attr.vt, attr.vt == VT_BOOL ? attr.boolVal : -2,
                                IsUsableWebEdit(node.Get()));
                    ::VariantClear(&attr);
                }
            }
            ::Sleep(300);
        }
        if (expected >= 0) {
            std::printf("maximum preview return time: %.3f ms\n", maxPreviewMs);
            if (!sawExpectedPreview) {
                std::puts("FAIL: async hover never produced the expected result");
                result = 1;
            }
            RECT bounds{};
            ::GetWindowRect(hwnd, &bounds);
            const POINT point{bounds.left + (bounds.right - bounds.left) / 2,
                              bounds.top + (bounds.bottom - bounds.top) / 2};
            finalState = static_cast<int>(picker.Inspect(point, nullptr, true));
            const bool shouldCommit =
                IsPickableState(static_cast<WebInputPickState>(expected));
            const bool committed = picker.CommitCandidate();
            if (committed != shouldCommit ||
                (committed && picker.SelectedWindow() != hwnd)) {
                std::puts("FAIL: fresh drop/retained target does not match expected window");
                result = 1;
            }
            picker.ClearSelected();
            if (picker.SelectedWindow() || !picker.SelectedName().empty()) {
                std::puts("FAIL: target cleanup left selected information behind");
                result = 1;
            }
        }
    }
    ::CoUninitialize();
    if (expected >= 0 && finalState != expected) {
        std::printf("FAIL: expected final state %d, got %d\n", expected, finalState);
        result = 1;
    }
    return result;
}
