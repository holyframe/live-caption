#pragma once

// ---- Child control identifiers -------------------------------------------
#define IDC_CAPTION_VIEW      1001
#define IDC_BTN_SEND          1002
#define IDC_CHK_PRESS_ENTER   1003
#define IDC_LBL_HINT          1004
#define IDC_BTN_VIEW_MODE     1005
#define IDC_CBO_FONT          1006
#define IDC_CBO_SIZE          1007
#define IDC_CBO_SPACING       1008
#define IDC_CHK_COPY_LIVE     1009
#define IDC_BTN_FRONT_ALL     1010
#define IDC_BTN_MINIMIZE_ALL  1011
#define IDC_BTN_COPY          1012
#define IDC_STATUSBAR         1013
#define IDC_RIGHT_PANEL       1014
#define IDC_BOTTOM_PANEL      1015
#define IDC_BTN_SETTINGS      1016
#define IDC_BTN_PICK_WINDOW   1017
#define IDC_SELECTED_WINDOW   1018

// ---- Global hotkey --------------------------------------------------------
#define HOTKEY_TOGGLE_VIEW    0xB001

// ---- Private window messages ---------------------------------------------
// WM_APP_CAPTION_UPDATE : wParam unused, lParam = CaptionUpdate* (receiver owns)
#define WM_APP_CAPTION_UPDATE (WM_APP + 1)
// WM_APP_STATUS         : wParam unused, lParam = std::wstring* (receiver owns)
#define WM_APP_STATUS         (WM_APP + 2)
// WM_APP_SOURCE_CHANGED : wParam = SourceKind, lParam = HWND of the source window
#define WM_APP_SOURCE_CHANGED (WM_APP + 3)

#define IDI_APPICON           101
