#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include "MainWindow.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int showCommand) {
    // The UI thread stays in an STA; the capture thread joins the MTA, which is
    // what UI Automation clients want.
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comInit)) return 1;

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&controls);

    int exitCode = 0;
    {
        if (!MainWindow::RegisterWindowClass(instance)) {
            ::MessageBoxW(nullptr, L"Failed to register the window class.", L"Live Caption App",
                          MB_ICONERROR | MB_OK);
            ::CoUninitialize();
            return 1;
        }

        MainWindow window;
        if (!window.Create(instance, showCommand)) {
            ::MessageBoxW(nullptr, L"Failed to create the main window.", L"Live Caption App",
                          MB_ICONERROR | MB_OK);
            ::CoUninitialize();
            return 1;
        }

        MSG message;
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!::IsDialogMessageW(window.Handle(), &message)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }
        exitCode = static_cast<int>(message.wParam);
    }

    ::CoUninitialize();
    return exitCode;
}
