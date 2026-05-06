# Orange Code self-screenshot tool.
# Capture the current OrangeCode.exe main window to a PNG so the agent can verify rendering.
#
# Strategy:
#   1. Resolve target HWND from $env:ORANGE_CODE_PID. Fallback: process name "OrangeCode".
#      (Past name "Code" collided with VS Code; renaming the binary made the fallback clean.)
#   2. PrintWindow(PW_RENDERFULLCONTENT=2) so it works even if the window is occluded.
#   3. Save as PNG to -Output (default tools\last-capture.png).

param(
    [string]$Output = "tools\last-capture.png"
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class CaptureNative {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

Add-Type -AssemblyName System.Drawing

# 1) Resolve HWND
$targetPid = $env:ORANGE_CODE_PID
$proc = $null
if ($targetPid) {
    try { $proc = Get-Process -Id ([int]$targetPid) -ErrorAction Stop } catch { $proc = $null }
}
if (-not $proc -or $proc.MainWindowHandle -eq [IntPtr]::Zero) {
    # Fallback by process name. After rename, "OrangeCode" is unambiguous (no clash with VS Code).
    $proc = Get-Process -Name "OrangeCode" -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } |
            Select-Object -First 1
}
if (-not $proc) { throw "OrangeCode.exe with main window not found (ORANGE_CODE_PID=$targetPid)" }
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw "Process $($proc.Id) has no main window" }

# 2) Window rect
$rect = New-Object CaptureNative+RECT
[void][CaptureNative]::GetWindowRect($hwnd, [ref]$rect)
$w = [int]($rect.Right - $rect.Left)
$h = [int]($rect.Bottom - $rect.Top)
if ($w -le 0 -or $h -le 0) { throw "Invalid window rect: ${w}x${h}" }

# 3) Bitmap -> PrintWindow -> save
$bmp = [System.Drawing.Bitmap]::new($w, $h)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
try {
    [void][CaptureNative]::PrintWindow($hwnd, $hdc, 2)
} finally {
    $gfx.ReleaseHdc($hdc)
    $gfx.Dispose()
}

$outDir = Split-Path -Parent $Output
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$bmp.Save($Output, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Output "Captured $hwnd ($w x $h) to $Output"