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

public static class LweFailureProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);

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
}
'@

function Get-FileSnapshot([string] $Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
    }
    return $null
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = [IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
$resolvedVideo = (Resolve-Path -LiteralPath $VideoPath).Path
foreach ($required in @($executable, $resolvedVideo)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}
if (@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'Close the existing Live Wallpaper Engine process before this controlled test.'
}

$settingsDirectory = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySettingsSnapshot = Get-FileSnapshot $legacySettingsPath
$baselineWallpaper =
    (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
$logPath = Join-Path $settingsDirectory 'logs\LiveWallpaperEngine.log'
$baselineLogBytes = if (Test-Path -LiteralPath $logPath) {
    (Get-Item -LiteralPath $logPath).Length
} else { 0 }
$process = $null

try {
    $arguments = @(
        '--test-seconds=8',
        ('--test-wallpaper="{0}"' -f $resolvedVideo)
    )
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
    $control = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweFailureProbe]::Find([uint32]$process.Id)
        if ($control -ne [IntPtr]::Zero -or $process.HasExited) { break }
    }
    if ($control -eq [IntPtr]::Zero) {
        throw 'The video test control window was not created.'
    }
    Start-Sleep -Seconds 2

    # A fresh process starts its first wallpaper session and Media Engine
    # generation at 1. Inject the same asynchronous ERROR event that previously
    # made Run() return 1 and looked like a probabilistic application crash.
    $packedEvent = [int64](([uint64]1 -shl 32) -bor [uint64]5)
    [void][LweFailureProbe]::PostMessage(
        $control, 0x8002, [IntPtr]$packedEvent, [IntPtr]1)
    Start-Sleep -Seconds 2
    $process.Refresh()
    if ($process.HasExited) {
        throw "Injected video failure exited the process with code $($process.ExitCode)."
    }
    if (-not $process.WaitForExit(7000) -or $process.ExitCode -ne 0) {
        throw 'The failure-containment process did not complete normally.'
    }
    $process = $null

    $logBytes = [IO.File]::ReadAllBytes($logPath)
    $logSegment = [Text.Encoding]::UTF8.GetString(
        $logBytes, [int]$baselineLogBytes,
        $logBytes.Length - [int]$baselineLogBytes)
    foreach ($requiredLog in @(
        'Media Engine reported a video playback failure.',
        'Contained a video playback failure without exiting the application.',
        'Controlled test duration completed.')) {
        if (-not $logSegment.Contains($requiredLog)) {
            throw "Expected containment evidence was not logged: $requiredLog"
        }
    }
    if ((Get-FileSnapshot $settingsPath) -ne $settingsSnapshot -or
        (Get-FileSnapshot $legacySettingsPath) -ne $legacySettingsSnapshot) {
        throw 'Controlled failure injection changed the saved settings.'
    }
    if ((Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' `
            -Name WallPaper).WallPaper -ne $baselineWallpaper) {
        throw 'Controlled failure injection changed the Windows system wallpaper.'
    }

    'FAILURE_INJECTED=True'
    'PROCESS_SURVIVED=True'
    'FAILURE_CONTAINED=True'
    'NORMAL_EXIT=True'
    'SETTINGS_UNCHANGED=True'
    'SYSTEM_WALLPAPER_UNCHANGED=True'
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
}

"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
