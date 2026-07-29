#include "SettingsDialog.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>

#include "resource.h"

namespace {

constexpr COLORREF kDarkBackground = RGB(32, 32, 32);
constexpr COLORREF kDarkText = RGB(235, 235, 235);

struct DialogState {
    Settings value;
    HBRUSH darkBrush = nullptr;
};

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD bytes = sizeof(value);
    return ::RegGetValueW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &bytes) ==
               ERROR_SUCCESS &&
           value == 0;
}

bool IsDark(UiTheme theme) {
    return theme == UiTheme::Dark || (theme == UiTheme::System && SystemUsesDarkTheme());
}

std::wstring DialogText(HWND dialog, int controlId) {
    const HWND control = ::GetDlgItem(dialog, controlId);
    const int length = control ? ::GetWindowTextLengthW(control) : 0;
    if (length <= 0) return {};

    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(copied, 0)));
    return text;
}

bool PickSaveFolder(HWND owner, const std::wstring& initialFolder, std::wstring& selectedFolder) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IFileOpenDialog> picker;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&picker))) ||
        !picker) {
        return false;
    }

    DWORD options = 0;
    picker->GetOptions(&options);
    picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    picker->SetTitle(L"Choose caption save folder");

    if (!initialFolder.empty()) {
        ComPtr<IShellItem> initial;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initialFolder.c_str(), nullptr,
                                                    IID_PPV_ARGS(&initial))) &&
            initial) {
            picker->SetFolder(initial.Get());
        }
    }

    if (picker->Show(owner) != S_OK) return false;

    ComPtr<IShellItem> result;
    if (FAILED(picker->GetResult(&result)) || !result) return false;

    PWSTR path = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
    selectedFolder = path;
    ::CoTaskMemFree(path);
    return !selectedFolder.empty();
}

WORD ToHotkeyControlValue(unsigned modifiers, unsigned key) {
    BYTE flags = 0;
    if (modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    if (modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    return MAKEWORD(static_cast<BYTE>(key), flags);
}

void ApplyDialogTheme(HWND dialog, DialogState* state) {
    const bool dark = state && IsDark(state->value.theme);
    const BOOL darkTitle = dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(dialog, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitle,
                            sizeof(darkTitle));
    const wchar_t* explorer = dark ? L"DarkMode_Explorer" : L"Explorer";
    const wchar_t* field = dark ? L"DarkMode_CFD" : L"CFD";
    const int explorerIds[] = {IDC_BTN_HOTKEY_CHANGE, IDC_BTN_SAVE_FOLDER, IDOK, IDCANCEL};
    for (int id : explorerIds) ::SetWindowTheme(::GetDlgItem(dialog, id), explorer, nullptr);
    const int fieldIds[] = {IDC_CBO_SOURCE, IDC_CBO_THEME, IDC_EDT_HOTKEY,
                            IDC_EDT_SAVE_FOLDER};
    for (int id : fieldIds) ::SetWindowTheme(::GetDlgItem(dialog, id), field, nullptr);
    ::InvalidateRect(dialog, nullptr, TRUE);
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(::GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<DialogState*>(lParam);
            ::SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            state->darkBrush = ::CreateSolidBrush(kDarkBackground);

            HWND source = ::GetDlgItem(dialog, IDC_CBO_SOURCE);
            ::SendMessageW(source, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(L"Windows 11 Live Captions"));
            ::SendMessageW(source, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(L"Chrome Live Caption"));
            ::SendMessageW(source, CB_SETCURSEL,
                           state->value.captionSource ==
                                   CaptionSourcePreference::ChromeLiveCaption
                               ? 1
                               : 0,
                           0);

            HWND theme = ::GetDlgItem(dialog, IDC_CBO_THEME);
            ::SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark"));
            ::SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light"));
            ::SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"System"));
            ::SendMessageW(theme, CB_SETCURSEL, static_cast<WPARAM>(state->value.theme), 0);
            ::SendDlgItemMessageW(
                dialog, IDC_EDT_HOTKEY, HKM_SETHOTKEY,
                ToHotkeyControlValue(state->value.hotkeyModifiers,
                                     state->value.hotkeyVirtualKey),
                0);
            ::SetDlgItemTextW(dialog, IDC_EDT_SAVE_FOLDER,
                              state->value.ResolvedSaveFolder().c_str());
            ApplyDialogTheme(dialog, state);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_HOTKEY_CHANGE && HIWORD(wParam) == BN_CLICKED) {
                ::SetFocus(::GetDlgItem(dialog, IDC_EDT_HOTKEY));
                return TRUE;
            }
            if (LOWORD(wParam) == IDC_BTN_SAVE_FOLDER && HIWORD(wParam) == BN_CLICKED) {
                std::wstring folder;
                if (PickSaveFolder(dialog, DialogText(dialog, IDC_EDT_SAVE_FOLDER), folder)) {
                    ::SetDlgItemTextW(dialog, IDC_EDT_SAVE_FOLDER, folder.c_str());
                }
                return TRUE;
            }
            if (LOWORD(wParam) == IDC_CBO_THEME && HIWORD(wParam) == CBN_SELCHANGE) {
                const LRESULT selected =
                    ::SendDlgItemMessageW(dialog, IDC_CBO_THEME, CB_GETCURSEL, 0, 0);
                if (selected != CB_ERR) state->value.theme = static_cast<UiTheme>(selected);
                ApplyDialogTheme(dialog, state);
                return TRUE;
            }
            if (LOWORD(wParam) == IDOK) {
                const LRESULT source =
                    ::SendDlgItemMessageW(dialog, IDC_CBO_SOURCE, CB_GETCURSEL, 0, 0);
                const LRESULT theme =
                    ::SendDlgItemMessageW(dialog, IDC_CBO_THEME, CB_GETCURSEL, 0, 0);
                state->value.captionSource =
                    source == 1 ? CaptionSourcePreference::ChromeLiveCaption
                                : CaptionSourcePreference::WindowsLiveCaptions;
                if (theme != CB_ERR) state->value.theme = static_cast<UiTheme>(theme);
                state->value.saveFolder = DialogText(dialog, IDC_EDT_SAVE_FOLDER);
                const WORD hotkey = static_cast<WORD>(
                    ::SendDlgItemMessageW(dialog, IDC_EDT_HOTKEY, HKM_GETHOTKEY, 0, 0));
                const BYTE key = LOBYTE(hotkey);
                const BYTE flags = HIBYTE(hotkey);
                if (key != 0) {
                    unsigned modifiers = 0;
                    if (flags & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
                    if (flags & HOTKEYF_ALT) modifiers |= MOD_ALT;
                    if (flags & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;
                    state->value.hotkeyModifiers = modifiers;
                    state->value.hotkeyVirtualKey = key;
                }
                ::EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            if (state && IsDark(state->value.theme)) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                ::SetTextColor(dc, kDarkText);
                ::SetBkColor(dc, kDarkBackground);
                ::SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<INT_PTR>(state->darkBrush);
            }
            break;

        case WM_DESTROY:
            if (state && state->darkBrush) {
                ::DeleteObject(state->darkBrush);
                state->darkBrush = nullptr;
            }
            break;
    }
    return FALSE;
}

}  // namespace

bool ShowSettingsDialog(HWND owner, HINSTANCE instance, Settings& settings) {
    DialogState state;
    state.value = settings;
    const INT_PTR result =
        ::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), owner, DialogProc,
                          reinterpret_cast<LPARAM>(&state));
    if (result != IDOK) return false;
    settings = state.value;
    return true;
}
