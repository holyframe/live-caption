#pragma once

#include <string_view>

#include "CaptionSource.h"

// Pure window-identification rules kept separate from the UI Automation code
// so the source preference can be verified without requiring live caption
// windows to be open.
namespace caption_source_detail {

inline bool IsWindowForChoice(CaptionSourceChoice choice, std::wstring_view processImage,
                              std::wstring_view lowerWindowTitle) {
    switch (choice) {
        case CaptionSourceChoice::WindowsLiveCaptions:
            return processImage == L"livecaptions.exe";

        case CaptionSourceChoice::Chrome:
            return processImage == L"chrome.exe" &&
                   lowerWindowTitle.find(L"live caption") != std::wstring_view::npos;
    }
    return false;
}

}  // namespace caption_source_detail
