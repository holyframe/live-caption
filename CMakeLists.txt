cmake_minimum_required(VERSION 3.21)
project(LiveCaptionView LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "LiveCaptionView targets Windows only (Win32 + UI Automation).")
endif()

add_executable(LiveCaptionView WIN32
    src/main.cpp
    src/Util.cpp
    src/Settings.cpp
    src/SettingsDialog.cpp
    src/CaptionMerger.cpp
    src/TranscriptStore.cpp
    src/CaptionSource.cpp
    src/CaptureEngine.cpp
    src/CaptionView.cpp
    src/WebInputPicker.cpp
    src/MainWindow.cpp
    resources/app.rc
)

target_include_directories(LiveCaptionView PRIVATE src resources)

target_compile_definitions(LiveCaptionView PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(LiveCaptionView PRIVATE
    user32
    gdi32
    comctl32
    d2d1
    dwrite
    ole32
    oleaut32
    uuid
    dwmapi
    shlwapi
    uxtheme
    advapi32
)

if(MSVC)
    target_compile_options(LiveCaptionView PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # Static CRT keeps the output a single self-contained .exe.
    set_property(TARGET LiveCaptionView PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(LiveCaptionView PROPERTIES
    OUTPUT_NAME "LiveCaptionView"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
