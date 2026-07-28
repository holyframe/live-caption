# End-to-end check: runs the viewer, speaks through the default output device so
# Windows 11 Live captions has something to transcribe, then reports CPU cost and
# captures a screenshot of the result.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\LiveCaptionView.exe'
$shot = Join-Path $root 'build\verify_shot.png'
$transcript = Join-Path $root 'build\captions.txt'

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Win {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp == pid && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

if (Test-Path $transcript) { Remove-Item $transcript }

Write-Host "Starting viewer..."
$proc = Start-Process -FilePath $exe -WorkingDirectory (Join-Path $root 'build') -PassThru
Start-Sleep -Seconds 2

$hwnd = [Win]::Find($proc.Id)
if ($hwnd -eq [IntPtr]::Zero) { throw "Viewer window not found." }
Write-Host ("Window handle: 0x{0:X}" -f $hwnd.ToInt64())

$cpuBefore = $proc.TotalProcessorTime
$wallBefore = Get-Date

Add-Type -AssemblyName System.Speech
$speaker = New-Object System.Speech.Synthesis.SpeechSynthesizer
$speaker.Volume = 100
Write-Host "Speaking..."
$speaker.Speak("Testing the tightened polling interval. The viewer should now track the Live Captions panel almost exactly. Counting to verify continuous capture. One. Two. Three. Four. Five. Six. Seven. Eight. Nine. Ten. That concludes the measurement.")
$speaker.Dispose()

Start-Sleep -Milliseconds 500
$proc.Refresh()
$cpuAfter = $proc.TotalProcessorTime
$elapsed = ((Get-Date) - $wallBefore).TotalSeconds
$cpuSec = ($cpuAfter - $cpuBefore).TotalSeconds
Write-Host ""
Write-Host ("CPU: {0:N2}% of one core over {1:N1} s of active captioning" -f (100 * $cpuSec / $elapsed), $elapsed)

[Win]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE
[Win]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 700

$r = New-Object Win+RECT
[Win]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
Write-Host ("Window rect: {0}x{1}" -f $w, $h)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "Screenshot: $shot"

[Win]::SendMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
$proc.WaitForExit(8000) | Out-Null
Write-Host ("Exited cleanly: {0}" -f $proc.HasExited)

if (Test-Path $transcript) {
    Write-Host "`n--- captions.txt ---"
    Get-Content $transcript
} else {
    Write-Host "`nNo transcript written."
}
