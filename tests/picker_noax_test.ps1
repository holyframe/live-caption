# Regression test for browsers that expose no page accessibility tree, which is
# what --disable-renderer-accessibility produces. A strict picker must reject
# these windows because it cannot prove that the drop point is an editable input.
# Uses a disposable Chrome profile and a local fixture, never a user's tabs.
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
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr w, uint f);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
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

$fixture = [Uri]::new((Join-Path $PSScriptRoot 'picker_fixture.html')).AbsoluteUri
$profileDir = Join-Path $repo ('build\picker-noax-test-' + [Guid]::NewGuid().ToString('N'))
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

[void][NoAx]::SetThreadDpiAwarenessContext([IntPtr](-4))
$arguments = @('--user-data-dir="' + $profileDir + '"', '--no-first-run', '--no-default-browser-check', '--disable-background-networking', '--disable-sync', '--disable-extensions', '--new-window', '--disable-renderer-accessibility', ($fixture + '?case=fill'))
$browser = Start-Process -FilePath $BrowserPath -ArgumentList $arguments -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $browser.Refresh()
        if ($browser.MainWindowHandle -ne 0 -and $browser.MainWindowTitle.StartsWith('Picker test fixture fill')) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    $browser.Refresh()
    if ($browser.MainWindowHandle -eq 0 -or !$browser.MainWindowTitle.StartsWith('Picker test fixture fill')) {
        throw 'The test browser did not finish opening the local fixture.'
    }

    $window = $browser.MainWindowHandle
    if (![NoAx]::SetWindowPos($window, [IntPtr](-1), 120, 120, 1000, 700, 0x50)) {
        throw 'Could not expose the local fixture window for hit testing.'
    }
    Start-Sleep -Milliseconds 700

    $probe = & (Join-Path $repo 'build\picker_probe.exe') $window.ToInt64()
    $noPage = [bool]($probe | Select-String -SimpleMatch 'documents=0') -or [bool]($probe | Select-String -SimpleMatch 'children=0')
    Check $noPage 'browser exposes no accessible page'
    if (!$noPage) { $probe | Write-Output }

    $content = [NoAx]::Content($window)
    Check (($content.R - $content.L) -gt 0) 'page area child window located'
    $pageX = [int](($content.L + $content.R) / 2)
    $pageY = [int](($content.T + $content.B) / 2)
    $hitPoint = New-Object NoAx+Point
    $hitPoint.X = $pageX
    $hitPoint.Y = $pageY
    if ([NoAx]::GetAncestor([NoAx]::WindowFromPoint($hitPoint), 2) -ne $window) {
        throw 'Another window covered the test fixture page area.'
    }

    # 2 = NoWebDocument. The page is not pickable and no target can be committed.
    $strict = & (Join-Path $repo 'build\picker_send_probe.exe') $window.ToInt64() $pageX $pageY 2
    $strictExit = $LASTEXITCODE
    Check ($strictExit -eq 0) 'inaccessible browser page is rejected'
    Check ([bool]($strict | Select-String -SimpleMatch 'pickable=0 committed=0')) 'no unverified target is retained'
    if ($strictExit -ne 0) { $strict | Write-Output }
} finally {
    $browser.Refresh()
    if (!$browser.HasExited) {
        [void]$browser.CloseMainWindow()
        if (!$browser.WaitForExit(3000)) { $browser.Kill() }
    }
    $browser.Dispose()
    Stop-ProfileBrowsers $profileDir
    Remove-Item -Recurse -Force -LiteralPath $profileDir -ErrorAction SilentlyContinue
}

Write-Output ''
Write-Output "Picker no-accessibility: $failures failures"
if ($failures -ne 0) { exit 1 }
