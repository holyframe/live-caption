# Regression test for browsers that expose no page accessibility tree, which is
# what --disable-renderer-accessibility produces. Several privacy and
# anti-fingerprinting Chromium builds (for example ixBrowser) always launch with
# that switch, so a UI Automation-only picker can never see their inputs.
#
# Uses a disposable Chrome profile and a local fixture, never a user's tabs. The
# fixture mirrors what it receives into its window title, which is how this test
# confirms typing arrived without being able to read the page.
param(
    [string]$BrowserPath = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!(Test-Path -LiteralPath $BrowserPath)) {
    throw "Chrome is required for this optional integration test: $BrowserPath"
}

& (Join-Path $PSScriptRoot 'picker_probe.bat')
if ($LASTEXITCODE -ne 0) { throw 'Could not build the picker probe.' }
& (Join-Path $PSScriptRoot 'picker_send_probe.bat')
if ($LASTEXITCODE -ne 0) { throw 'Could not build the picker send probe.' }

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NoAx {
    public delegate bool EnumProc(IntPtr w, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out Rect r);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr w, EnumProc cb, IntPtr p);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr w, StringBuilder t, int l);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr w, StringBuilder t, int l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr w, uint f);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    public static string Title(IntPtr w) {
        var text = new StringBuilder(512);
        GetWindowTextW(w, text, text.Capacity);
        return text.ToString();
    }
    // Chromium draws page content into this child window. Its rectangle is the
    // page area, excluding the tab strip and the address bar.
    public static Rect Content(IntPtr root) {
        Rect found = new Rect();
        EnumChildWindows(root, (child, p) => {
            var name = new StringBuilder(256);
            GetClassNameW(child, name, name.Capacity);
            if (name.ToString() != "Chrome_RenderWidgetHostHWND" || !IsWindowVisible(child)) return true;
            GetWindowRect(child, out found);
            return false;
        }, IntPtr.Zero);
        return found;
    }
}
'@

$fixture = [Uri]::new((Join-Path $PSScriptRoot 'picker_fixture.html')).AbsoluteUri
$profileDir = Join-Path $repo ('build\picker-noax-test-' + [Guid]::NewGuid().ToString('N'))
$sent = 'live caption regression line'
$failures = 0
function Check([bool]$passed, [string]$name) {
    Write-Output ("{0,-70} {1}" -f $name, $(if ($passed) { 'PASS' } else { 'FAIL' }))
    if (!$passed) { $script:failures++ }
}

[void][NoAx]::SetThreadDpiAwarenessContext([IntPtr](-4))
$entryCursor = New-Object NoAx+Point
[void][NoAx]::GetCursorPos([ref]$entryCursor)

$arguments = @('--user-data-dir="' + $profileDir + '"', '--no-first-run',
               '--no-default-browser-check', '--disable-background-networking',
               '--disable-sync', '--disable-extensions', '--new-window',
               '--disable-renderer-accessibility',
               ($fixture + '?case=fill'))
$browser = Start-Process -FilePath $BrowserPath -ArgumentList $arguments -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $browser.Refresh()
        if ($browser.MainWindowHandle -ne 0 -and
            $browser.MainWindowTitle.StartsWith('Picker test fixture fill')) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    $browser.Refresh()
    if ($browser.MainWindowHandle -eq 0 -or
        !$browser.MainWindowTitle.StartsWith('Picker test fixture fill')) {
        throw 'The test browser did not finish opening the local fixture.'
    }
    $window = $browser.MainWindowHandle
    if (![NoAx]::SetWindowPos($window, [IntPtr](-1), 120, 120, 1000, 700, 0x50)) {
        throw 'Could not expose the local fixture window for hit testing.'
    }
    Start-Sleep -Milliseconds 700

    # The picker must see no accessible page at all; that is the condition this
    # whole test exists for.
    $probe = & (Join-Path $repo 'build\picker_probe.exe') $window.ToInt64()
    Check ([bool]($probe | Select-String -SimpleMatch 'documents=0')) `
          'browser exposes no accessible document'

    $content = [NoAx]::Content($window)
    Check (($content.R - $content.L) -gt 0) 'page area child window located'
    $pageX = [int](($content.L + $content.R) / 2)
    $pageY = [int](($content.T + $content.B) / 2)

    $hitPoint = New-Object NoAx+Point
    $hitPoint.X = $pageX; $hitPoint.Y = $pageY
    if ([NoAx]::GetAncestor([NoAx]::WindowFromPoint($hitPoint), 2) -ne $window) {
        throw 'Another window covered the test fixture page area.'
    }

    # 7 = NoWebContent: the toolbar and tab strip must stay unpickable, because
    # typing into the address bar would navigate.
    $toolbar = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $pageX ($content.T - 30) 7
    Check ($LASTEXITCODE -eq 0) 'browser toolbar is refused rather than picked'
    if ($LASTEXITCODE -ne 0) { $toolbar | Write-Output }

    Write-Output "title before send: $([NoAx]::Title($window))"

    # Park the pointer somewhere known so the restore can be checked without
    # depending on where it happened to be when this script started.
    $park = New-Object NoAx+Point
    $park.X = $content.L + 5
    $park.Y = $content.T + 5
    [void][NoAx]::SetCursorPos($park.X, $park.Y)

    # 6 = ValidByPoint: the page area is pickable by remembering the point.
    $send = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $pageX $pageY 6 $sent 1
    Check ($LASTEXITCODE -eq 0) 'page area is picked and typed into by clicking'
    $send | Write-Output
    Check ([bool]($send | Select-String -SimpleMatch 'clicksPoint=1')) `
          'target reports that it types by clicking'

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $title = [NoAx]::Title($window)
        if ($title.Contains($sent)) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    Write-Output "title after send: $title"
    # Typing into the address bar would also put the text in the window title,
    # via a search results page, so require the fixture to still be loaded.
    Check ($title.StartsWith('Picker test fixture fill |')) 'the browser did not navigate away'
    Check ($title.Contains('[focus=1]')) 'the click focused the page input'
    Check ($title.Contains($sent)) 'typed text reached the page input'
    Check ($title.Contains('[enter=1]')) 'the requested Enter key reached the page input'

    $cursor = New-Object NoAx+Point
    [void][NoAx]::GetCursorPos([ref]$cursor)
    if ($cursor.X -eq $park.X -and $cursor.Y -eq $park.Y) {
        Check $true 'the pointer was returned to where the user left it'
    } elseif ($cursor.X -eq $pageX -and $cursor.Y -eq $pageY) {
        Check $false 'the pointer was returned to where the user left it'
    } else {
        # Someone used the mouse during the send, so the restore is unprovable.
        Write-Output ("{0,-70} SKIP (pointer moved externally to {1},{2})" -f `
                      'the pointer was returned to where the user left it', $cursor.X, $cursor.Y)
    }
} finally {
    $browser.Refresh()
    if (!$browser.HasExited) {
        [void]$browser.CloseMainWindow()
        if (!$browser.WaitForExit(3000)) { $browser.Kill() }
    }
    $browser.Dispose()
    [void][NoAx]::SetCursorPos($entryCursor.X, $entryCursor.Y)
}

Write-Output ''
Write-Output "Picker no-accessibility: $failures failures"
if ($failures -ne 0) { exit 1 }
