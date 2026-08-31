# Exercises the real EXE's mouse capture, hover outline, drop, and icon removal.
# Uses only an isolated app copy and disposable browser profile with a local page.
param(
    [string]$BrowserPath = 'C:\Program Files\Google\Chrome\Application\chrome.exe',
    [string[]]$Cases = @('delayed', 'rich', 'readonly'),
    [string]$ExePath = '',
    [switch]$DisableAccessibility
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $repo ('build\picker-ui-test-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $runDir)
if (!$ExePath) { $ExePath = Join-Path $repo 'build\LiveCaptionView.exe' }
Copy-Item -LiteralPath $ExePath -Destination (Join-Path $runDir 'LiveCaptionView.exe')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'picker_ui_fixture.ini') -Destination (Join-Path $runDir 'LiveCaptionView.ini')
$fixture = [Uri]::new((Join-Path $PSScriptRoot 'picker_fixture.html')).AbsoluteUri
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class PickerUI {
    public delegate bool EnumProc(IntPtr w, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr w, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr w, uint command);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr w, StringBuilder text, int length);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr w);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out Rect r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w, IntPtr after, int x,int y,int cx,int cy,uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr w);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr w, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern uint GetPixel(IntPtr dc, int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr w, uint f);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeoutW(IntPtr w,uint m,IntPtr wp,IntPtr lp,uint flags,uint timeout,out UIntPtr result);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeoutW(IntPtr w,uint m,IntPtr wp,StringBuilder text,uint flags,uint timeout,out UIntPtr result);
    public static void Send(IntPtr w, uint m, long wp, long lp) {
        UIntPtr result;
        if (SendMessageTimeoutW(w,m,new IntPtr(wp),new IntPtr(lp),2,5000,out result)==IntPtr.Zero)
            throw new Exception("UI message timed out: " + m);
    }
    public static string Text(IntPtr w) {
        UIntPtr result; var text=new StringBuilder(1024);
        if (SendMessageTimeoutW(w,13,new IntPtr(text.Capacity),text,2,5000,out result)==IntPtr.Zero)
            throw new Exception("UI text query timed out");
        return text.ToString();
    }
    public static IntPtr Child(IntPtr w, int id) {
        IntPtr found=IntPtr.Zero;
        EnumChildWindows(w,(c,p)=> { if(GetDlgCtrlID(c)==id){found=c;return false;} return true;},IntPtr.Zero);
        if(found==IntPtr.Zero) throw new Exception("Missing UI control: " + id);
        return found;
    }
    public static IntPtr Root(uint pid) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((w,p)=> { uint id; GetWindowThreadProcessId(w,out id);
            var name=new StringBuilder(256); GetClassNameW(w,name,name.Capacity);
            if(id==pid && (name.ToString()=="LiveCaptionViewMain" || name.ToString()=="Chrome_WidgetWin_1"))
                {found=w;return false;} return true; },IntPtr.Zero);
        return found;
    }
    public static IntPtr Outline(uint pid) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((w,p)=> { uint id; GetWindowThreadProcessId(w,out id);
            var name=new StringBuilder(256); GetClassNameW(w,name,name.Capacity);
            if(id==pid && name.ToString()=="LiveCaptionViewPickOutline"){found=w;return false;}
            return true; },IntPtr.Zero);
        return found;
    }
    public static bool RedOutline(IntPtr w) {
        if(w==IntPtr.Zero || !IsWindowVisible(w)) return false;
        Rect r; GetWindowRect(w,out r); var dc=GetDC(IntPtr.Zero);
        try { return GetPixel(dc,r.L+1,(r.T+r.B)/2)==0x000000ff; }
        finally { ReleaseDC(IntPtr.Zero,dc); }
    }
    public static Point Center(IntPtr w) {
        Rect r; GetWindowRect(w,out r); return new Point{X=(r.L+r.R)/2,Y=(r.T+r.B)/2};
    }
}
'@
function Wait-Window($process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $process.Refresh()
        if ($process.HasExited) { throw 'Test process exited before creating its window.' }
        $hiddenWindow = [PickerUI]::Root($process.Id)
        if ($hiddenWindow -ne 0) { return $hiddenWindow }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Test process did not create its window.'
}
function Close-TestProcess($process) {
    if (!$process) { return }
    $process.Refresh()
    if (!$process.HasExited) {
        [void]$process.CloseMainWindow()
        if (!$process.WaitForExit(3000)) { $process.Kill() }
    }
    $process.Dispose()
}
$originalCursor = [PickerUI+Point]::new()
[void][PickerUI]::GetCursorPos([ref]$originalCursor)
$originalForeground = [PickerUI]::GetForegroundWindow()
$app = $null
$browser = $null
[void][PickerUI]::SetThreadDpiAwarenessContext([IntPtr](-4))
try {
    $app = Start-Process -FilePath (Join-Path $runDir 'LiveCaptionView.exe') -WorkingDirectory $runDir -PassThru -WindowStyle Hidden
    $appWindow = Wait-Window $app
    [PickerUI]::Send($appWindow, 0x111, 1021, 0) # Stop caption capture in the test copy.
    [void][PickerUI]::SetWindowPos($appWindow, [IntPtr](-1), 20, 20, 640, 480, 0x50)
    $picker = [PickerUI]::Child($appWindow, 1017)
    $status = [PickerUI]::Child($appWindow, 1013)
    $icon = [PickerUI]::Child($appWindow, 1018)
    foreach ($scenario in $Cases) {
        $profile = Join-Path $runDir ('browser-' + $scenario)
        $arguments = @('--user-data-dir="' + $profile + '"', '--no-first-run',
            '--no-default-browser-check', '--disable-background-networking', '--disable-sync',
            '--disable-extensions', '--new-window', '"' + $fixture + '?case=' + $scenario + '"')
        if ($DisableAccessibility) { $arguments += '--disable-renderer-accessibility' }
        $browser = Start-Process -FilePath $BrowserPath -ArgumentList $arguments -PassThru -WindowStyle Hidden
        $browserWindow = Wait-Window $browser
        [void][PickerUI]::SetWindowPos($browserWindow, [IntPtr](-1), 680, 20, 900, 700, 0x50)
        [void][PickerUI]::SetForegroundWindow($appWindow)
        Start-Sleep -Milliseconds 300
        $start = [PickerUI]::Center($picker)
        $target = [PickerUI]::Center($browserWindow)
        if ([PickerUI]::WindowFromPoint($start) -ne $picker) { throw 'Test picker button is occluded.' }
        $moved = [PickerUI]::SetCursorPos($start.X, $start.Y)
        Start-Sleep -Milliseconds 100
        $actual = [PickerUI+Point]::new()
        $cursorRead = [PickerUI]::GetCursorPos([ref]$actual)
        if (!$moved -or !$cursorRead -or $actual.X -ne $start.X -or $actual.Y -ne $start.Y) {
            throw 'Windows did not position the test pointer.'
        }
        [PickerUI]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 100
        if ([PickerUI]::Text($status) -notmatch 'Drag over|This app cannot be selected') {
            throw 'Windows did not deliver the test mouse press to the picker.'
        }
        [void][PickerUI]::SetCursorPos($target.X, $target.Y)
        $lastStatus = ''
        for ($pass=0; $pass -lt 12; $pass++) {
            Start-Sleep -Milliseconds 300
            $hoverPoint = [PickerUI+Point]::new()
            [void][PickerUI]::GetCursorPos([ref]$hoverPoint)
            if ($hoverPoint.X -ne $target.X -or $hoverPoint.Y -ne $target.Y) {
                throw 'UI test interrupted: the pointer moved outside the scripted drag.'
            }
            if ([PickerUI]::GetAncestor([PickerUI]::WindowFromPoint($target), 2) -ne $browserWindow) {
                throw 'UI test interrupted: another window covered the browser fixture.'
            }
            $currentStatus = [PickerUI]::Text($status)
            if ($currentStatus -ne $lastStatus) { Write-Output "$scenario hover: $currentStatus"; $lastStatus=$currentStatus }
        }
        # With renderer accessibility off the page is invisible to UI Automation,
        # so every scenario is picked the same way: by remembering the point.
        # Otherwise the field itself is inspected and unusable ones are refused.
        $expected = if ($DisableAccessibility) { $true }
                    else { $scenario -notin @('readonly','disabled','password','empty') }
        $outline = [PickerUI]::Outline($app.Id)
        if ($expected -and ![PickerUI]::RedOutline($outline)) {
            Write-Output "Failed hover: $([PickerUI]::Text($status)); target=$browserWindow hit=$([PickerUI]::GetAncestor([PickerUI]::WindowFromPoint($target),2))"
            & (Join-Path $repo 'build\picker_probe.exe') $browserWindow.ToInt64()
            throw 'Valid target lacks the red outline.'
        }
        if (!$expected -and [PickerUI]::IsWindowVisible($outline)) { throw 'Invalid target has an outline.' }
        [PickerUI]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 300
        Write-Output "$scenario drop: $([PickerUI]::Text($status))"
        $picked = [PickerUI]::IsWindowVisible($icon)
        if ($picked -ne $expected) { throw "Actual EXE drag failed: $scenario, picked=$picked expected=$expected" }
        if ([PickerUI]::IsWindowVisible($outline)) { throw 'Outline remained visible after drop.' }
        if ($picked) {
            $start = [PickerUI]::Center($icon)
            [void][PickerUI]::SetForegroundWindow($appWindow)
            [void][PickerUI]::SetCursorPos($start.X, $start.Y)
            Start-Sleep -Milliseconds 100
            [PickerUI]::mouse_event(8, 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 100
            [void][PickerUI]::SetCursorPos($target.X, $target.Y)
            Start-Sleep -Milliseconds 100
            [PickerUI]::mouse_event(16, 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 200
            if ([PickerUI]::IsWindowVisible($icon)) { throw 'Right-drag removal failed.' }
            if ([PickerUI]::Text($appWindow) -notlike '*No target selected*') { throw 'Removed target title remains.' }
        }
        Write-Output "PASS: actual EXE drag/drop $scenario"
        Close-TestProcess $browser
        $browser = $null
    }
} finally {
    [PickerUI]::mouse_event(4 -bor 16, 0, 0, 0, [UIntPtr]::Zero)
    Close-TestProcess $browser
    Close-TestProcess $app
    [void][PickerUI]::SetCursorPos($originalCursor.X, $originalCursor.Y)
    [void][PickerUI]::SetForegroundWindow($originalForeground)
}
