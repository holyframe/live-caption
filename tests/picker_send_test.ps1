# End-to-end tests for verified insertion, confirmed Enter, retained-tab
# reactivation, and rejection of a replaced input. Uses disposable Chrome
# profiles and the local fixture; it never touches a user's tabs.
param(
    [string]$BrowserPath = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!(Test-Path -LiteralPath $BrowserPath)) {
    throw "Chrome is required for this optional integration test: $BrowserPath"
}

& (Join-Path $PSScriptRoot 'picker_send_probe.bat')
if ($LASTEXITCODE -ne 0) { throw 'Could not build the picker send probe.' }

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class VerifiedSend {
    public delegate bool EnumProc(IntPtr w, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out Rect r);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr w, EnumProc cb, IntPtr p);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr w, StringBuilder t, int l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr w, uint f);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    public static void SelectTab(IntPtr window, byte digit) {
        SetForegroundWindow(window);
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        keybd_event(digit, 0, 0, UIntPtr.Zero);
        keybd_event(digit, 0, 2, UIntPtr.Zero);
        keybd_event(0x11, 0, 2, UIntPtr.Zero);
    }
    public static Rect Content(IntPtr root) {
        Rect found = new Rect();
        long best = 0;
        EnumChildWindows(root, (child, p) => {
            var name = new StringBuilder(256);
            GetClassNameW(child, name, name.Capacity);
            if (name.ToString() != "Chrome_RenderWidgetHostHWND" || !IsWindowVisible(child)) return true;
            Rect r;
            if (!GetWindowRect(child, out r)) return true;
            long area = (long)(r.R - r.L) * (r.B - r.T);
            if (area > best) { best = area; found = r; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

$failures = 0
function Check([bool]$passed, [string]$name) {
    Write-Output ("{0,-70} {1}" -f $name, $(if ($passed) { 'PASS' } else { 'FAIL' }))
    if (!$passed) { $script:failures++ }
}

function Stop-ProfileBrowsers([string]$profilePath) {
    Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine.Contains($profilePath) } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Wait-BrowserWindow($browser, [string]$titlePrefix) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $browser.Refresh()
        if ($browser.MainWindowHandle -ne 0 -and $browser.MainWindowTitle.StartsWith($titlePrefix)) {
            return $browser.MainWindowHandle
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "The test browser did not show '$titlePrefix'."
}

function Wait-TitleContains($browser, [string]$text) {
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        $browser.Refresh()
        if ($browser.MainWindowTitle.Contains($text)) { return $true }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    [Console]::WriteLine("Last browser title: $($browser.MainWindowTitle)")
    return $false
}

function Page-Center([IntPtr]$window) {
    $content = [VerifiedSend]::Content($window)
    if (($content.R - $content.L) -le 0) { throw 'Could not locate the browser page area.' }
    $point = New-Object VerifiedSend+Point
    $point.X = [int](($content.L + $content.R) / 2)
    $point.Y = [int](($content.T + $content.B) / 2)
    if ([VerifiedSend]::GetAncestor([VerifiedSend]::WindowFromPoint($point), 2) -ne $window) {
        throw 'Another window covered the test fixture.'
    }
    return $point
}

[void][VerifiedSend]::SetThreadDpiAwarenessContext([IntPtr](-4))
$fixture = [Uri]::new((Join-Path $PSScriptRoot 'picker_fixture.html')).AbsoluteUri
$profileDir = Join-Path $repo ('build\picker-send-test-' + [Guid]::NewGuid().ToString('N'))
$browser = $null
try {
    $arguments = @('--user-data-dir="' + $profileDir + '"', '--no-first-run', '--no-default-browser-check', '--disable-background-mode', '--disable-sync', '--disable-extensions', '--new-window', ($fixture + '?case=fill'), ($fixture + '?case=empty'), ($fixture + '?case=rich'))
    $browser = Start-Process -FilePath $BrowserPath -ArgumentList $arguments -PassThru
    Start-Sleep -Milliseconds 700
    $browser.Refresh()
    if ($browser.MainWindowHandle -eq 0) { throw 'The test browser did not create a window.' }
    $window = $browser.MainWindowHandle
    [void][VerifiedSend]::SetWindowPos($window, [IntPtr](-1), 120, 120, 1000, 700, 0x50)
    [VerifiedSend]::SelectTab($window, 0x31)
    $window = Wait-BrowserWindow $browser 'Picker test fixture fill'
    Start-Sleep -Milliseconds 500
    $point = Page-Center $window

    # 4 = Valid, 1 = Inserted.
    $insert = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $point.X $point.Y 4 'verified insert' 0
    $insertExit = $LASTEXITCODE
    Check ($insertExit -eq 0) 'verified keyboard insertion succeeds'
    Check ([bool]($insert | Select-String -SimpleMatch 'outcome=1 sent=1')) 'insertion reports the Inserted outcome'
    Check (Wait-TitleContains $browser 'live=verified insert') 'appended text is observable in the exact composer'

    # 2 = Submitted.
    $submit = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $point.X $point.Y 4 'verified submit' 1
    $submitExit = $LASTEXITCODE
    Check ($submitExit -eq 0) 'verified Enter submission succeeds'
    Check ([bool]($submit | Select-String -SimpleMatch 'outcome=2 sent=1')) 'submission reports the Submitted outcome'
    Check (Wait-TitleContains $browser 'live= last=verified insertverified submit') 'submission preserves existing text and clears the composer'

    [VerifiedSend]::SelectTab($window, 0x31)
    [void](Wait-BrowserWindow $browser 'Picker test fixture fill')
    $point = Page-Center $window
    $tabSend = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $point.X $point.Y 4 'retained tab' 1 1 0 0 1
    $tabExit = $LASTEXITCODE
    Check ($tabExit -eq 0) 'send returns from another tab to the retained tab'
    Check ([bool]($tabSend | Select-String -SimpleMatch 'outcome=2 sent=1')) 'retained-tab submission is confirmed'
    Check (Wait-TitleContains $browser 'last=retained tab') 'text reaches the originally picked tab only'

    [VerifiedSend]::SelectTab($window, 0x33)
    $window = Wait-BrowserWindow $browser 'Picker test fixture rich'
    $point = Page-Center $window
    $richSend = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $point.X $point.Y 4 'rich submit' 1
    $richExit = $LASTEXITCODE
    Check ($richExit -eq 0) 'contenteditable insertion and submission are verified'
    Check ([bool]($richSend | Select-String -SimpleMatch 'outcome=2 sent=1')) 'contenteditable send reports the Submitted outcome'
    Check (Wait-TitleContains $browser 'live= last=rich draft|rich submit') 'contenteditable submission preserves its draft and clears the composer'
} finally {
    if ($browser) {
        $browser.Refresh()
        if (!$browser.HasExited) {
            [void]$browser.CloseMainWindow()
            if (!$browser.WaitForExit(3000)) { $browser.Kill() }
        }
        $browser.Dispose()
    }
    Stop-ProfileBrowsers $profileDir
    Remove-Item -Recurse -Force -LiteralPath $profileDir -ErrorAction SilentlyContinue
}

$replaceProfile = Join-Path $repo ('build\picker-replace-test-' + [Guid]::NewGuid().ToString('N'))
$browser = $null
try {
    $arguments = @('--user-data-dir="' + $replaceProfile + '"', '--no-first-run', '--no-default-browser-check', '--disable-background-mode', '--disable-sync', '--disable-extensions', '--new-window', ($fixture + '?case=replace'))
    $browser = Start-Process -FilePath $BrowserPath -ArgumentList $arguments -PassThru
    $window = Wait-BrowserWindow $browser 'Picker test fixture replace'
    [void][VerifiedSend]::SetWindowPos($window, [IntPtr](-1), 120, 120, 1000, 700, 0x50)
    Start-Sleep -Milliseconds 500
    $point = Page-Center $window
    $replaced = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $point.X $point.Y 4 'must not arrive' 0
    $replaceExit = $LASTEXITCODE
    Check ($replaceExit -ne 0) 'a composer replaced after pickup is rejected'
    Check ([bool]($replaced | Select-String -Pattern 'exact picked|exact input')) 'stale-input failure explains that the exact target changed'
    $browser.Refresh()
    Check (!$browser.MainWindowTitle.Contains('must not arrive')) 'text is not redirected into the replacement composer'
} finally {
    if ($browser) {
        $browser.Refresh()
        if (!$browser.HasExited) {
            [void]$browser.CloseMainWindow()
            if (!$browser.WaitForExit(3000)) { $browser.Kill() }
        }
        $browser.Dispose()
    }
    Stop-ProfileBrowsers $replaceProfile
    Remove-Item -Recurse -Force -LiteralPath $replaceProfile -ErrorAction SilentlyContinue
}

Write-Output ''
Write-Output "Picker verified send: $failures failures"
if ($failures -ne 0) { exit 1 }
