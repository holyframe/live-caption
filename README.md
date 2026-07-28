# Live Caption View

A native Windows app that reads the live captions produced by **Google Chrome's
Live Caption** *and* **Windows 11's built-in Live captions**, reassembles them
into a clean transcript, and shows them in its own scrollable viewer while
saving them to a text file.

Neither caption source lets you keep what it transcribes. This app harvests the
text through UI Automation, so there is no microphone access and no speech
recognition here — Windows or Chrome does the recognition, and this reads the
result.

## Requirements

- Windows 10 1809 or later (Windows 11 for the built-in Live captions source)
- Visual Studio 2022 with the **Desktop development with C++** workload
  (MSVC toolset + Windows SDK)

## Building

```bat
build.bat            :: optimised build -> build\LiveCaptionView.exe
build.bat debug      :: unoptimised build with debug info
```

`build.bat` locates `vcvars64.bat` on its own (falling back to `vswhere`), so it
needs no pre-configured shell. A `CMakeLists.txt` is also provided if you prefer
CMake or want IDE integration.

To run the unit checks on the transcript-merging logic:

```bat
tests\run_tests.bat
```

## Using it

Start Chrome's Live Caption or Windows 11's Live captions, then launch
`LiveCaptionView.exe`. It polls for a supported caption window and attaches
automatically; if the source closes, it reconnects on its own.

The status bar reports which source is attached and where the transcript is
being written.

| Control | Behaviour |
| --- | --- |
| **Send** | Placeholder — deliberately does nothing yet. |
| **Press Enter** | When checked, Enter triggers Send. |
| **Normal View / Compact View** | Toggles a borderless always-on-top mode. The toolbar stays visible so you can switch back. |
| Font / size / spacing | Applied to the caption pane immediately and remembered. |
| **Copy real-time if this window is active** | Keeps the clipboard in sync with the transcript while this window is focused, throttled to once every 600 ms. |
| **Front all** | Brings both this window and the caption source forward. |
| **Minimize all** | Minimises both. |
| **Copy** | Copies the whole visible transcript to the clipboard. |
| Double-click the caption area | Same as Send. |
| **Ctrl+Shift+Z** | Global hotkey that shows/hides the window. |

### About the hotkey

The original design called for **Shift+Z**, but Windows refuses to grant a bare
`Shift`+letter global hotkey through `RegisterHotKey`, and taking it anyway
would require a system-wide keyboard hook that stopped you typing a capital Z in
any other program. The default is therefore **Ctrl+Shift+Z**. It is
configurable via `HotkeyModifiers` / `HotkeyVirtualKey` in the INI, and if the
configured combination is unavailable the app falls back through
`Ctrl+Shift`, `Ctrl+Alt`, then `Ctrl+Alt+Shift` and shows whichever one it
actually got in the title bar.

## Settings

Preferences live in `LiveCaptionView.ini` next to the executable: font, size,
line spacing, checkbox states, window position, hotkey, and `TranscriptPath`
(blank means `captions.txt` beside the executable).

## How it works

```
CaptureEngine (background thread, COM MTA)
  CaptionSource   -> finds the caption window, reads its text via UI Automation
  CaptionMerger   -> stitches overlapping snapshots into one transcript
  TranscriptStore -> appends settled lines to captions.txt
        |
        | PostMessage
        v
MainWindow (UI thread, COM STA)
  CaptionView -> Direct2D/DirectWrite rendering, scrolling, live typography
```

### Staying real-time

Captions arrive by **push, not poll**. On attaching, the app subscribes to UI
Automation's `TextChanged` event plus `Name`/`Value` property-change
notifications, and the callback simply signals the capture thread. Latency is
therefore however long the provider takes to raise the event, rather than up to a
full poll interval. The status bar shows `live updates` when the subscription
succeeded, or `polling` if the provider refused it.

A 40 ms timed poll remains purely as a fallback for providers that do not raise
events. Because the wake-up event is auto-reset, a burst of notifications
coalesces into a single read, and a 5 ms floor between reads stops an unusually
chatty provider from monopolising the thread.

Each read costs one or two cross-process calls, because the working accessor
(`TextPattern`, `ValuePattern` or `Name`) is identified once at attach time and
its pattern object cached, rather than re-probing all three every time.

### Stitching the snapshots

A caption window is a scrolling buffer. Each poll returns mostly the same text
as the previous one, shifted along as old words scroll off and new ones arrive —
and the recogniser *revises* its unstable tail, changing casing and punctuation
once it grows more confident (`the java` becomes `The Java.`).

`CaptionMerger` finds the longest suffix of the previous snapshot that matches a
prefix of the new one and lets the new snapshot overwrite that region. Words are
compared after normalisation (lowercased, punctuation stripped) so revisions
still align, and the newer wording always wins. Anything before the overlap is
untouched history.

Cost matters here because the Windows 11 source hands back its entire scrollback
on *every* read. A fast path detects that the new snapshot merely extends or
revises the previous one, reuses the words already tokenised for the unchanged
prefix, and resumes its word count from a memo of the last update, so only the
new text is tokenised and normalised. The overlap search is likewise bounded by
the previous snapshot rather than the whole transcript.

That fast path is only valid when the source did not scroll, since it pins the
new snapshot to where the previous one started. It therefore requires the
unchanged prefix to cover at least 80% of the previous snapshot; a short
coincidental match on a repeated word such as "that's that's" would otherwise
drop a word. Anything less falls back to the full overlap search, which is
always correct, just slower.

Measured on a 40 KB buffer (1000 sentences), one update costs about **0.17 ms**.

### What reaches disk, and when

A line is committed once it can no longer change: either it has scrolled out of
the caption window, or at least three later sentences have completed. The second
rule exists because Windows 11 Live captions keeps its whole buffer visible, so
the scroll rule alone would never commit anything until exit. Whatever remains
is flushed when the window closes.

## Notes and limitations

- Only the last 3000 lines stay in the viewer to keep memory flat over long
  sessions; the full transcript remains in the file.
- Sentence splitting is heuristic. Titles like `Dr.` and initials like `J.` do
  not split, but a sentence ending in a number (`...is 42.`) does.
- Captions from an elevated process cannot be read unless this app is also
  elevated — a UI Automation restriction, not a bug here.
- `dist\captions.txt` is a leftover transcript from the previous Python version
  and can be deleted.
