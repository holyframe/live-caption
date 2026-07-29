#pragma once

#include <windows.h>

#include "Settings.h"

// Shows a modal settings window. `settings` is updated only when OK is pressed.
bool ShowSettingsDialog(HWND owner, HINSTANCE instance, Settings& settings);

