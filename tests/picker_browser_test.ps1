# Uses a disposable Chrome profile and a local fixture, never a user's tabs.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
if (!(Test-Path -LiteralPath $chrome)) { throw 'Chrome is required for this optional integration test.' }
& (Join-Path $PSScriptRoot 'picker_probe.bat')
if ($LASTEXITCODE -ne 0) { throw 'Could not build the picker probe.' }
$fixture = [Uri]::new((Join-Path $PSScriptRoot 'picker_fixture.html')).AbsoluteUri
$profile = Join-Path $repo ('build\picker-browser-test-' + [Guid]::NewGuid().ToString('N'))
# Retain this isolated test profile under build/ for post-failure diagnostics.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PickerFixtureWindow {
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr w, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@
$cases = @{ delayed = 4; rich = 4; readonly = 3; disabled = 3; password = 3; empty = 3 }
foreach ($scenario in @('delayed','rich','readonly','disabled','password','empty')) {
    $arguments = @('--user-data-dir="' + $profile + '"', '--no-first-run',
                   '--no-default-browser-check', '--disable-background-networking',
                   '--disable-sync', '--disable-extensions',
                   '--app="' + $fixture + '?case=' + $scenario + '"')
    $browser = Start-Process -FilePath $chrome -ArgumentList $arguments -PassThru -WindowStyle Hidden
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $browser.Refresh()
            # Chrome may expose a temporary startup window before its page
            # window. Wait for this specific local fixture, not just any HWND.
            if ($browser.MainWindowHandle -ne 0 -and
                $browser.MainWindowTitle.StartsWith("Picker test fixture $scenario")) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($browser.MainWindowHandle -eq 0 -or
            !$browser.MainWindowTitle.StartsWith("Picker test fixture $scenario")) {
            throw 'The test browser did not finish opening the local fixture.'
        }
        if (![PickerFixtureWindow]::SetWindowPos($browser.MainWindowHandle, [IntPtr](-1),
                                                120, 120, 900, 600, 0x50)) {
            throw 'Could not expose the local fixture window for hit testing.'
        }
        $probeOutput = & (Join-Path $repo 'build\picker_probe.exe') $browser.MainWindowHandle.ToInt64() $cases[$scenario]
        if ($LASTEXITCODE -ne 0) {
            $probeOutput | Write-Output
            throw "Picker scenario failed: $scenario"
        }
        Write-Output "PASS: $scenario"
        $probeOutput | Select-String -SimpleMatch 'maximum preview return time:' | ForEach-Object { $_.Line }
    } finally {
        $browser.Refresh()
        if (!$browser.HasExited) {
            [void]$browser.CloseMainWindow()
            if (!$browser.WaitForExit(2000)) { $browser.Kill() }
        }
        $browser.Dispose()
    }
}
