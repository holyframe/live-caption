#!/usr/bin/env bash
# Runs the CaptionView selection checks on a machine that is not Windows, by
# cross-compiling with mingw-w64 and running the result under wine.
#
# Wine's Direct2D and DirectWrite lay out, measure and paint, but
# IDWriteTextLayout::HitTestPoint and HitTestTextRange are still stubs that
# return E_NOTIMPL, so the checks that select through the text are skipped here
# and the highlight does not paint into the screenshot. Everything driven by
# line metrics — the gutter, the tail-following selection, clamping across live
# updates, the keyboard — is covered.
#
#   sudo apt-get install mingw-w64 wine64 xvfb ffmpeg
#   tests/selection_test_wine.sh
#
# The Windows-native equivalent is tests\selection_test.bat.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build

echo "=== Cross-compiling ==="
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra \
    -static -static-libgcc -static-libstdc++ \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -Isrc -Iresources \
    tests/selection_test.cpp src/CaptionView.cpp \
    -o build/selection_test.exe \
    -ld2d1 -ldwrite -lole32 -lgdi32 -luser32

# Wine needs a display for the pane to present into.
export DISPLAY="${DISPLAY:-:99}"
if ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
    Xvfb "$DISPLAY" -screen 0 1280x800x24 >/dev/null 2>&1 &
    trap 'kill %1 2>/dev/null || true' EXIT
    sleep 2
fi

echo
export WINEDEBUG="${WINEDEBUG:--all}"
status=0
wine build/selection_test.exe --no-text-hittest || status=$?

for shot in selection_text selection_gutter; do
    if [ -f "build/$shot.bmp" ]; then
        ffmpeg -y -loglevel error -i "build/$shot.bmp" "build/$shot.png"
        rm -f "build/$shot.bmp"
        echo "Screenshot: build/$shot.png"
    fi
done

exit "$status"
