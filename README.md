# Live Caption App

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

The window is split into a full-height right panel, reserved for control
buttons, and a column beside it holding the caption pane, the toolbar's bottom
panel, and the log view along the foot. The log view reports which source is
attached and where the transcript is being written.

Drag the divider between the caption pane and the bottom panel to give the
captions more or less room; the pane keeps at least 80px and the bottom panel
keeps enough height for its toolbar row. That row stays at the foot of the
panel, so the space a drag opens up lands under the divider and the controls
do not move. The position is remembered across runs as `BottomPanelHeight`.

| Control | Behaviour |
| --- | --- |
| **Send** | Replaces the picked web tab's chat input with the selected caption text. |
| **Press Enter** | Also presses Enter in the picked chat input after inserting the selection. Enter in this app triggers Send when checked. |
| Font / size / spacing | Applied to the caption pane immediately and remembered. |
| **Settings** (right panel) | Opens caption-source, theme, and Send-hotkey preferences. |
| **Pick window** (right panel) | Drag onto a browser/WebView window to select its active tab. The selected window's icon appears beneath the unchanged picker button. |
| Double-click empty pane space | Same as Send. |
| **Shift+Z** | Global shortcut for Send (configurable in Settings). |

### Selecting caption text

The caption pane is a custom Direct2D control rather than an edit box, so
selection is implemented directly on it:

| Gesture | Behaviour |
| --- | --- |
| Drag | Selects across lines. Dragging past the top or bottom edge scrolls. |
| Shift+click | Extends the selection from the last click. |
| Double-click a word | Selects that word. Over empty space the gesture still means Send. |
| Left gutter (75px) | Click or drag to select whole lines, including their line breaks. Arrow cursor; the row under the pointer lights up. |
| Right panel (45px) | Full-height strip to the right of the scrollbar, with Pick window and the selected target icon at the top and Settings at the bottom. |
| Right-click | Copy / Select all / Clear selection. |
| **Ctrl+A** / **Ctrl+C** / **Esc** | Select all, copy, clear the selection. |

A selection is anchored to transcript positions rather than to screen
coordinates, so it survives the recogniser revising the line you are selecting
and old lines scrolling out of the pane's 3000-line window. If the selection's
far end sits at the end of the transcript, it grows as new words arrive —
whether the last line is rewritten in place or a brand-new line is appended. A
selection that stops short of the end is left alone.

`tests\selection_test.bat` covers those cases and leaves a screenshot of the
highlighted pane in `build\`.

### Picking a web input tab

Drag the Pick window button from the top of the right panel onto a browser or
WebView window. A crosshair means the active web tab contains an enabled,
focusable editable field and can be selected; the no-drop cursor means it
cannot. A valid target is also surrounded by a click-through red outline.
Releasing on it remembers that exact UI Automation document and input element,
and displays the target window's icon in a separate tile beneath the picker.
Click Send after selecting text in the caption pane. The app reactivates the
retained browser tab, focuses its chat input, and replaces the input contents
with the selection. When Press Enter is checked it then submits with Enter;
otherwise the populated input is left for review.
If a page contains several editable fields, release directly over the desired
chat composer to select it instead of the automatically preferred field.
To forget a picked target, right-drag its separate icon tile outside the app
window and release. Releasing inside cancels; releasing outside removes the
retained window, tab, input element, accessible label, and icon.

Only web inputs exposed beneath a UI Automation `Document` are accepted.
Ordinary desktop edit controls, browser address bars, disabled or read-only
fields, password fields, and tabs with no visible editable input are rejected.

### About the hotkey

The default global Send shortcut is **Shift+Z**. Use **Settings → Change…** to
focus the hotkey field, then press a new combination. If another application
already owns that shortcut, this app reports that it is unavailable instead of
silently substituting a different one.
The shortcut waits for its keys to be released and then runs the same Send
action as the button, including the current Press Enter checkbox state.

## Settings

The Settings window chooses Windows 11 Live Captions or Chrome Live Caption as
the sole capture source, switches the app between Dark, Light, and System
themes, and configures the Send hotkey. Preferences live in
`LiveCaptionView.ini` next to the executable along with font, size, line
spacing, checkbox states, window position, `BottomPanelHeight` (where the
caption/bottom-panel divider sits, in 96 dpi units), `TranscriptPath` (blank
means `captions.txt` beside the executable), and `PollIntervalMs` (how often the
caption source is re-read; default 8, see [Staying real-time](#staying-real-time)).

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

**Windows 11 Live captions accepts a UI Automation change subscription and then
never raises an event.** `tests\latency_probe.bat` measures this directly: over
12 seconds of continuous speech it recorded 49 text changes and 0 notifications.
Polling is therefore not a fallback for that source, it is the only mechanism
that works, and the poll interval is the entire client-side latency budget.

The app still subscribes to `TextChanged` and `Name`/`Value` property changes,
because providers that do honour them let the capture thread skip its wait
entirely. But the status bar no longer claims `live updates` on the strength of
the subscription being accepted; it reports the poll interval, and only says the
source is pushing notifications once one has actually arrived.

The default interval is **8 ms**, overridable with `PollIntervalMs` in the INI. A
read costs about 0.4 ms against a 2.2 KB buffer, so the tight interval is
affordable: measured cost during continuous captioning is under 2% of one core.
A 2 ms floor between reads stops a genuinely chatty provider from spinning the
thread.

### Getting it on screen

Reading the source quickly is only half of it; the repaint path had the larger
problem. `tests\render_probe.bat` measures it:

| `ID2D1HwndRenderTarget::EndDraw` | per frame | worst |
| --- | --- | --- |
| `D2D1_PRESENT_OPTIONS_NONE` (the default) | 17.73 ms | 32.14 ms |
| `D2D1_PRESENT_OPTIONS_IMMEDIATELY` | 0.61 ms | 6.70 ms |

The default present blocks until the next vertical blank. Captions revise faster
than the 16.9 ms refresh interval, so a vblank-locked repaint could not keep up,
and updates accumulated in the message queue — the view fell steadily further
behind the source and only caught up when the speaker paused. The render target
is now created with `IMMEDIATELY`.

Two supporting changes keep it that way:

- **Updates coalesce into one frame.** On receiving a caption update the window
  drains every other one already queued and applies them all before painting
  once, so the queue cannot grow without bound.
- **Content changes always invalidate.** Repainting used to be left to
  `SetScrollPos`, which skips its redraw when the scroll offset has not moved —
  precisely what happens when the recogniser revises the last line in place
  without changing its height. Such a revision could sit unrepainted
  indefinitely. `CaptionView::Present` now invalidates unconditionally and calls
  `UpdateWindow`, rather than waiting for `WM_PAINT` to arrive as the
  lowest-priority message in the queue.

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
