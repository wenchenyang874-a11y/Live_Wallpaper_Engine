[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $VideoPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweTrayProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w, int command);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);

    public static IntPtr Find(uint processId)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            var name = new StringBuilder(128);
            GetClassName(window, name, name.Capacity);
            if (owner == processId && name.ToString() == "LiveWallpaperEngine.Control") {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr Command(int identifier)
    {
        return (IntPtr)(identifier & 0xffff);
    }
}
'@

function Get-FileSnapshot([string] $Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
    }
    return $null
}

function Restore-FileSnapshot([string] $Path, [AllowNull()][string] $Snapshot) {
    if ($null -eq $Snapshot) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            Remove-Item -LiteralPath $Path -Force
        }
        return
    }
    [IO.File]::WriteAllBytes($Path, [Convert]::FromBase64String($Snapshot))
}

function Read-SharedBytes([string] $Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    try {
        $memory = [IO.MemoryStream]::new()
        $stream.CopyTo($memory)
        return $memory.ToArray()
    } finally {
        $stream.Dispose()
    }
}

function Read-LatestFrameCount([IntPtr] $Control, [string] $LogPath,
                               [long] $BaselineBytes) {
    [void][LweTrayProbe]::PostMessage(
        $Control, 0x0111, [LweTrayProbe]::Command(2196), [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    $bytes = Read-SharedBytes $LogPath
    $segment = [Text.Encoding]::UTF8.GetString(
        $bytes, [int]$BaselineBytes, $bytes.Length - [int]$BaselineBytes)
    $matches = [regex]::Matches($segment, 'CONTROLLED_VIDEO_TRANSFER_COUNT=(\d+)')
    if ($matches.Count -eq 0) { throw 'No controlled frame-count sample was logged.' }
    return [uint64]$matches[$matches.Count - 1].Groups[1].Value
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = [IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
$resolvedVideo = (Resolve-Path -LiteralPath $VideoPath).Path
if (@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'Close the existing Live Wallpaper Engine process before this controlled test.'
}
$settingsDirectory = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySettingsSnapshot = Get-FileSnapshot $legacySettingsPath
$logPath = Join-Path $settingsDirectory 'logs\LiveWallpaperEngine.log'
$baselineLogBytes = if (Test-Path -LiteralPath $logPath) {
    (Get-Item -LiteralPath $logPath).Length
} else { 0 }
$process = $null

try {
    $arguments = @('--test-seconds=30', ('--test-wallpaper="{0}"' -f $resolvedVideo))
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
    $control = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweTrayProbe]::Find([uint32]$process.Id)
        if ($control -ne [IntPtr]::Zero) { break }
    }
    if ($control -eq [IntPtr]::Zero) { throw 'The main window was not created.' }
    Start-Sleep -Seconds 2

    $beforePause = Read-LatestFrameCount $control $logPath $baselineLogBytes
    [void][LweTrayProbe]::PostMessage(
        $control, 0x0111, [LweTrayProbe]::Command(2105), [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    $pauseSettled = Read-LatestFrameCount $control $logPath $baselineLogBytes
    Start-Sleep -Seconds 2
    $whilePaused = Read-LatestFrameCount $control $logPath $baselineLogBytes
    if ($whilePaused -gt $pauseSettled + 1) {
        throw "Tray pause did not stop frame transfers: $pauseSettled -> $whilePaused"
    }
    [void][LweTrayProbe]::PostMessage(
        $control, 0x0111, [LweTrayProbe]::Command(2105), [IntPtr]::Zero)
    Start-Sleep -Seconds 2
    $afterResume = Read-LatestFrameCount $control $logPath $baselineLogBytes
    if ($afterResume -le $whilePaused) {
        throw "Tray resume did not restart frame transfers: $whilePaused -> $afterResume"
    }

    [void][LweTrayProbe]::ShowWindow($control, 0)
    if ([LweTrayProbe]::IsWindowVisible($control)) {
        throw 'The main window could not be hidden before the tray test.'
    }
    [void][LweTrayProbe]::PostMessage(
        $control, 0x8001, [IntPtr]1, [IntPtr]0x007B)
    Start-Sleep -Milliseconds 500
    if ([LweTrayProbe]::IsWindowVisible($control)) {
        throw 'Right-clicking the tray callback opened the main window.'
    }
    Start-Sleep -Milliseconds 300

    [void][LweTrayProbe]::PostMessage(
        $control, 0x8001, [IntPtr]1, [IntPtr]0x0202)
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        if ([LweTrayProbe]::IsWindowVisible($control)) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not [LweTrayProbe]::IsWindowVisible($control)) {
        throw 'Left-clicking the tray callback did not open the main window.'
    }
    $trayLogBytes = Read-SharedBytes $logPath
    $trayLogSegment = [Text.Encoding]::UTF8.GetString(
        $trayLogBytes, [int]$baselineLogBytes,
        $trayLogBytes.Length - [int]$baselineLogBytes)
    if (-not $trayLogSegment.Contains(
            'CONTROLLED_TRAY_MENU=show,import,sound,pause,cancel,exit')) {
        throw 'The controlled tray menu did not expose all expected commands.'
    }

    $shutdownTimer = [Diagnostics.Stopwatch]::StartNew()
    [void][LweTrayProbe]::PostMessage(
        $control, 0x8008, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit(3000)) {
        throw 'The installer shutdown message did not exit the process within 3 seconds.'
    }
    $shutdownTimer.Stop()
    if ($process.ExitCode -ne 0) {
        throw "The tray test process exited with code $($process.ExitCode)."
    }
    $shutdownLogBytes = Read-SharedBytes $logPath
    $shutdownLogSegment = [Text.Encoding]::UTF8.GetString(
        $shutdownLogBytes, [int]$baselineLogBytes,
        $shutdownLogBytes.Length - [int]$baselineLogBytes)
    if (-not $shutdownLogSegment.Contains(
            'Installer requested application shutdown.')) {
        throw 'The installer shutdown request was not recorded in the log.'
    }
    $process = $null

    "FRAME_COUNT_BEFORE_PAUSE=$beforePause"
    "FRAME_COUNT_PAUSE_SETTLED=$pauseSettled"
    "FRAME_COUNT_WHILE_PAUSED=$whilePaused"
    "FRAME_COUNT_AFTER_RESUME=$afterResume"
    'TRAY_PAUSE_RESUME=True'
    'RIGHT_CLICK_KEPT_WINDOW_HIDDEN=True'
    'LEFT_CLICK_OPENED_WINDOW=True'
    "INSTALLER_SHUTDOWN_ELAPSED_MS=$($shutdownTimer.ElapsedMilliseconds)"
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySettingsSnapshot
}

"SETTINGS_RESTORED=$((Get-FileSnapshot $settingsPath) -eq $settingsSnapshot)"
"LEGACY_SETTINGS_RESTORED=$((Get-FileSnapshot $legacySettingsPath) -eq $legacySettingsSnapshot)"
"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
