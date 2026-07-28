// Diagnostic: measures what a CaptionView repaint actually costs.
//
// The suspicion is that ID2D1HwndRenderTarget::EndDraw blocks on the vertical
// blank, because D2D1_PRESENT_OPTIONS_NONE is the default. If so, every repaint
// costs up to a full refresh interval no matter how little drawing it does, and
// caption updates arriving faster than the refresh rate queue up behind it.
//
// Renders the same frame both ways and reports the difference.

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kWidth = 900;
constexpr int kHeight = 520;
constexpr int kFrames = 60;
constexpr int kVisibleLines = 20;

double MsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return ::DefWindowProcW(h, m, w, l);
}

}  // namespace

int main() {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"RenderProbe";
    ::RegisterClassExW(&wc);

    // Visible and topmost: an occluded window lets DWM skip the present, which
    // would hide the very stall we are trying to measure.
    const HWND hwnd = ::CreateWindowExW(WS_EX_TOPMOST, L"RenderProbe", L"Render probe",
                                        WS_POPUP | WS_VISIBLE, 80, 80, kWidth, kHeight, nullptr,
                                        nullptr, instance, nullptr);
    if (!hwnd) {
        std::printf("Could not create the probe window.\n");
        return 1;
    }

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                          reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    if (!d2d || !dwrite) {
        std::printf("Direct2D/DirectWrite unavailable.\n");
        return 1;
    }

    ComPtr<IDWriteTextFormat> format;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"",
                             format.GetAddressOf());

    const std::wstring sample =
        L"So we could learn where the system is folding in the real world and where "
        L"monitoring is missing, and the edge cases that exist in real scenarios.";

    // Cost of building one line's layout, which happens for every revised line.
    {
        const auto start = Clock::now();
        constexpr int kBuilds = 200;
        for (int i = 0; i < kBuilds; ++i) {
            ComPtr<IDWriteTextLayout> layout;
            dwrite->CreateTextLayout(sample.c_str(), static_cast<UINT32>(sample.size()),
                                     format.Get(), 860.0f, 4096.0f, layout.GetAddressOf());
        }
        std::printf("CreateTextLayout : %.3f ms per line\n", MsSince(start) / kBuilds);
    }

    std::vector<ComPtr<IDWriteTextLayout>> layouts(kVisibleLines);
    for (int i = 0; i < kVisibleLines; ++i) {
        dwrite->CreateTextLayout(sample.c_str(), static_cast<UINT32>(sample.size()), format.Get(),
                                 860.0f, 4096.0f, layouts[i].GetAddressOf());
    }

    auto measure = [&](D2D1_PRESENT_OPTIONS options, const char* label) {
        ComPtr<ID2D1HwndRenderTarget> target;
        const D2D1_SIZE_U size = D2D1::SizeU(kWidth, kHeight);
        if (FAILED(d2d->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(hwnd, size, options), target.GetAddressOf()))) {
            std::printf("%-28s : render target creation failed\n", label);
            return;
        }
        target->SetDpi(96.0f, 96.0f);
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.9f, 0.9f), brush.GetAddressOf());

        double worst = 0.0;
        const auto start = Clock::now();
        for (int f = 0; f < kFrames; ++f) {
            const auto frameStart = Clock::now();
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0.17f, 0.17f, 0.17f));
            float y = 8.0f;
            for (int i = 0; i < kVisibleLines; ++i) {
                target->DrawTextLayout(D2D1::Point2F(14.0f, y), layouts[i].Get(), brush.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_NONE);
                y += 24.0f;
            }
            target->EndDraw();
            worst = (std::max)(worst, MsSince(frameStart));
        }
        std::printf("%-28s : %6.2f ms per frame (worst %.2f)\n", label, MsSince(start) / kFrames,
                    worst);
    };

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    ::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    std::printf("Display refresh  : %lu Hz (vblank every %.1f ms)\n\n", dm.dmDisplayFrequency,
                dm.dmDisplayFrequency ? 1000.0 / dm.dmDisplayFrequency : 0.0);

    measure(D2D1_PRESENT_OPTIONS_NONE, "EndDraw, default (current)");
    measure(D2D1_PRESENT_OPTIONS_IMMEDIATELY, "EndDraw, IMMEDIATELY");

    ::DestroyWindow(hwnd);
    return 0;
}
