[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $GifPath,
    [Parameter(Mandatory = $true)]
    [string] $VideoPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweMediaProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    public delegate bool EnumChildProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr w, EnumChildProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);

    public static IntPtr Find(uint processId, string targetClass, bool child)
    {
        IntPtr found = IntPtr.Zero;
        Func<IntPtr, bool> inspect = window => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            var name = new StringBuilder(128); GetClassName(window, name, name.Capacity);
            if (owner == processId && name.ToString() == targetClass) { found = window; return false; }
            return true;
        };
        if (child) EnumChildWindows(GetDesktopWindow(), (w, x) => inspect(w), IntPtr.Zero);
        else EnumWindows((w, x) => inspect(w), IntPtr.Zero);
        return found;
    }
}
'@

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"
foreach ($requiredPath in @($executable, $GifPath, $VideoPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file not found: $requiredPath"
    }
}

$settingsPaths = @(
    (Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine\settings.json'),
    (Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine\settings.v1.json')
)
function Get-SettingsState {
    ($settingsPaths | ForEach-Object {
        if (Test-Path -LiteralPath $_ -PathType Leaf) {
            "$_=$([Convert]::ToBase64String([IO.File]::ReadAllBytes($_)))"
        } else { "$_=<absent>" }
    }) -join "`n"
}
function Get-GpuSample([int] $processId) {
    try {
        $engines = @(Get-CimInstance `
            Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine |
            Where-Object { $_.Name -match "pid_$($processId)_" })
        $memory = @(Get-CimInstance `
            Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
            Where-Object { $_.Name -match "pid_$($processId)_" })
        return [pscustomobject]@{
            ThreeDPercent = [double](($engines | Where-Object Name -Match 'engtype_3D' |
                Measure-Object UtilizationPercentage -Sum).Sum)
            VideoDecodePercent = [double](($engines | Where-Object Name -Match 'engtype_VideoDecode' |
                Measure-Object UtilizationPercentage -Sum).Sum)
            VideoProcessingPercent = [double](($engines | Where-Object Name -Match 'engtype_VideoProcessing' |
                Measure-Object UtilizationPercentage -Sum).Sum)
            DedicatedBytes = [uint64](($memory | Measure-Object DedicatedUsage -Sum).Sum)
            SharedBytes = [uint64](($memory | Measure-Object SharedUsage -Sum).Sum)
        }
    } catch {
        return $null
    }
}
function Start-MediaTest([string] $path) {
    $arguments = @('--test-seconds=60', ('--test-wallpaper="{0}"' -f $path))
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
    $control = [IntPtr]::Zero
    $wallpaper = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweMediaProbe]::Find([uint32]$process.Id, 'LiveWallpaperEngine.Control', $false)
        $wallpaper = [LweMediaProbe]::Find([uint32]$process.Id, 'LiveWallpaperEngine.Wallpaper', $true)
        if ($control -ne [IntPtr]::Zero -and $wallpaper -ne [IntPtr]::Zero) { break }
        if ($process.HasExited) { break }
    }
    if ($control -eq [IntPtr]::Zero -or $wallpaper -eq [IntPtr]::Zero) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id }
        throw "Media wallpaper windows were not found for '$path'."
    }
    @($process, $control)
}
function Stop-MediaTest($process, [IntPtr] $control) {
    [void][LweMediaProbe]::PostMessage($control, 0x0111, [IntPtr]1108, [IntPtr]::Zero)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'Media wallpaper process did not exit normally.'
    }
}

$baselineSettings = Get-SettingsState
$baselineWallpaper = (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
$logPath = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine\logs\LiveWallpaperEngine.log'
$baselineLogBytes = if (Test-Path -LiteralPath $logPath) { (Get-Item -LiteralPath $logPath).Length } else { 0 }
$activeProcess = $null
try {
    $started = Start-MediaTest $GifPath
    $activeProcess = $started[0]; $control = [IntPtr]$started[1]
    $activeProcess.Refresh(); $cpuStart = $activeProcess.TotalProcessorTime
    Start-Sleep -Seconds 3
    $activeProcess.Refresh(); $gifCpuMs = ($activeProcess.TotalProcessorTime - $cpuStart).TotalMilliseconds
    $gifWorkingSet = $activeProcess.WorkingSet64
    $gifGpu = Get-GpuSample $activeProcess.Id
    Stop-MediaTest $activeProcess $control; $activeProcess = $null

    $started = Start-MediaTest $VideoPath
    $activeProcess = $started[0]; $control = [IntPtr]$started[1]
    Start-Sleep -Seconds 2
    [void][LweMediaProbe]::PostMessage($control, 0x0111, [IntPtr]1106, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    [void][LweMediaProbe]::PostMessage($control, 0x0111, [IntPtr]1106, [IntPtr]::Zero)
    # Simulate the documented WTS lock/unlock notifications without locking
    # the interactive test machine. This exercises the same message handler.
    [void][LweMediaProbe]::PostMessage($control, 0x02B1, [IntPtr]7, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    [void][LweMediaProbe]::PostMessage($control, 0x02B1, [IntPtr]8, [IntPtr]::Zero)
    $activeProcess.Refresh(); $cpuStart = $activeProcess.TotalProcessorTime
    Start-Sleep -Seconds 3
    $activeProcess.Refresh(); $videoCpuMs = ($activeProcess.TotalProcessorTime - $cpuStart).TotalMilliseconds
    $videoWorkingSet = $activeProcess.WorkingSet64
    $videoGpu = Get-GpuSample $activeProcess.Id
    Stop-MediaTest $activeProcess $control; $activeProcess = $null

    $logBytes = [IO.File]::ReadAllBytes($logPath)
    $logSegment = [Text.Encoding]::UTF8.GetString($logBytes, [int]$baselineLogBytes, $logBytes.Length - [int]$baselineLogBytes)
    foreach ($required in @('Loaded an animated GIF with', 'Animated GIF overlay mode activated.',
        'GIF playback statistics: rendered=',
        'Media Engine opened a looping video; audio is muted:', 'Media Engine video playback started.',
        'Video playback statistics: rendered=',
        'Video audio enabled.', 'Video audio muted.', 'Dynamic wallpaper paused:',
        'Dynamic wallpaper resumed after pause policy cleared.')) {
        if (-not $logSegment.Contains($required)) { throw "Expected runtime evidence was not logged: $required" }
    }
    if ((Get-SettingsState) -ne $baselineSettings) { throw 'Controlled tests changed saved settings.' }
    if ((Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper -ne $baselineWallpaper) {
        throw 'Media overlays changed the Windows system wallpaper.'
    }
    "GIF_CPU_DELTA_MS_3S=$gifCpuMs"
    "GIF_WORKING_SET_BYTES=$gifWorkingSet"
    "VIDEO_CPU_DELTA_MS_3S=$videoCpuMs"
    "VIDEO_WORKING_SET_BYTES=$videoWorkingSet"
    if ($null -eq $gifGpu) {
        'GIF_GPU_METRICS=unavailable'
    } else {
        "GIF_GPU_3D_PERCENT=$($gifGpu.ThreeDPercent)"
        "GIF_GPU_VIDEO_DECODE_PERCENT=$($gifGpu.VideoDecodePercent)"
        "GIF_GPU_SHARED_BYTES=$($gifGpu.SharedBytes)"
    }
    if ($null -eq $videoGpu) {
        'VIDEO_GPU_METRICS=unavailable'
    } else {
        "VIDEO_GPU_3D_PERCENT=$($videoGpu.ThreeDPercent)"
        "VIDEO_GPU_VIDEO_DECODE_PERCENT=$($videoGpu.VideoDecodePercent)"
        "VIDEO_GPU_VIDEO_PROCESSING_PERCENT=$($videoGpu.VideoProcessingPercent)"
        "VIDEO_GPU_SHARED_BYTES=$($videoGpu.SharedBytes)"
    }
    'GIF_PLAYBACK=True'; 'VIDEO_PLAYBACK=True'; 'VIDEO_DEFAULT_MUTED=True'
    'VIDEO_SOUND_TOGGLE=True'; 'SESSION_PAUSE_RESUME=True'
    'SETTINGS_UNCHANGED=True'; 'SYSTEM_WALLPAPER_UNCHANGED=True'
} finally {
    if ($null -ne $activeProcess -and -not $activeProcess.HasExited) {
        Stop-Process -Id $activeProcess.Id -ErrorAction SilentlyContinue
    }
}

"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
