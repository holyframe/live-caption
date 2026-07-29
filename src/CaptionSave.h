#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace caption_save {

// Makes a browser tab name safe as one component of a Windows filename.
std::wstring SanitizeTabName(std::wstring_view tabName);

// Produces: YYYY-MM-DD HH-mm-ss-fff - <selected tab name>.txt
std::wstring BuildFileName(const SYSTEMTIME& saveTime, std::wstring_view tabName);

}  // namespace caption_save
