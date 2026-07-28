// Diagnostic: finds out where caption latency actually comes from.
//
// Answers three questions that decide which optimisation is worth doing:
//   1. How long does one UI Automation read of the caption text take?
//   2. How often does the source actually change its accessible text? That
//      cadence is a hard floor no client-side change can beat.
//   3. Do UIA change notifications really fire, and do they arrive before a
//      tight poll would have noticed? Subscribing successfully is not the same
//      as receiving useful events.
//
// Build and run with tests\latency_probe.bat, with a caption source open.

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "CaptionSource.h"

using Clock = std::chrono::steady_clock;

namespace {

double MsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

int main() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int exitCode = 0;
    {
        CaptionSource source;
        if (!source.Initialize()) {
            std::printf("UI Automation failed to initialise.\n");
            ::CoUninitialize();
            return 1;
        }

        std::printf("Looking for a caption window...\n");
        for (int i = 0; i < 40 && !source.IsAttached(); ++i) {
            if (source.Attach()) break;
            ::Sleep(250);
        }
        if (!source.IsAttached()) {
            std::printf("No caption source found. Open Windows 11 Live captions or Chrome's\n"
                        "Live Caption, then run this again.\n");
            ::CoUninitialize();
            return 1;
        }
        std::wprintf(L"Attached to: %s\n", SourceKindName(source.Kind()));

        HANDLE changeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool subscribed = source.SubscribeToChanges(changeEvent);
        std::printf("Change subscription: %s\n\n", subscribed ? "ACCEPTED" : "REFUSED");

        // --- 1. cost of a single read -------------------------------------
        std::wstring text;
        source.ReadText(text);
        const size_t bufferChars = text.size();

        constexpr int kReads = 50;
        const auto readStart = Clock::now();
        for (int i = 0; i < kReads; ++i) source.ReadText(text);
        const double perRead = MsBetween(readStart, Clock::now()) / kReads;
        std::printf("1. Read cost      : %.2f ms per read (buffer is %zu chars)\n", perRead,
                    bufferChars);

        // --- 2 & 3. observe the source ------------------------------------
        constexpr int kObserveSeconds = 12;
        std::printf("\n2. Observing for %d s - play audio so captions are being produced...\n",
                    kObserveSeconds);

        std::wstring previous;
        source.ReadText(previous);

        auto lastChangeAt = Clock::now();
        auto lastEventAt = Clock::now();
        bool eventPending = false;

        int changeCount = 0;
        int eventCount = 0;
        int changesPrecededByEvent = 0;
        std::vector<double> changeGaps;
        std::vector<double> eventLead;

        const auto deadline = Clock::now() + std::chrono::seconds(kObserveSeconds);
        while (Clock::now() < deadline) {
            if (::WaitForSingleObject(changeEvent, 0) == WAIT_OBJECT_0) {
                ++eventCount;
                if (!eventPending) {
                    eventPending = true;
                    lastEventAt = Clock::now();
                }
            }

            std::wstring current;
            if (source.ReadText(current) && current != previous) {
                const auto now = Clock::now();
                changeGaps.push_back(MsBetween(lastChangeAt, now));
                lastChangeAt = now;
                previous = std::move(current);
                ++changeCount;

                if (eventPending) {
                    ++changesPrecededByEvent;
                    eventLead.push_back(MsBetween(lastEventAt, now));
                    eventPending = false;
                }
            }
            ::Sleep(2);
        }

        std::printf("\n");
        if (changeCount == 0) {
            std::printf("   No caption activity seen. The source was idle, so the cadence and\n"
                        "   event figures below are not meaningful. Re-run while audio plays.\n");
        }
        std::printf("   Text changes observed : %d over %d s (%.1f per second)\n", changeCount,
                    kObserveSeconds, changeCount / static_cast<double>(kObserveSeconds));
        std::printf("   Median gap between changes : %.0f ms  <-- the source's own floor\n",
                    Median(changeGaps));
        std::printf("   UIA events received   : %d\n", eventCount);
        std::printf("   Changes an event warned us about : %d of %d\n", changesPrecededByEvent,
                    changeCount);
        std::printf("   Median event lead time: %.0f ms\n", Median(eventLead));

        std::printf("\nReading:\n");
        std::printf("  * If the median gap between changes is large, that is the source's\n"
                    "    update cadence and no client-side change can beat it.\n");
        std::printf("  * If events warned us about few changes, we are really running on the\n"
                    "    fallback poll and lowering that interval is what would help.\n");

        source.Unsubscribe();
        ::CloseHandle(changeEvent);
    }
    ::CoUninitialize();
    return exitCode;
}
