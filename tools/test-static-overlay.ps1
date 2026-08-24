param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweStaticOverlayProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    public delegate bool EnumChildProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc procedure, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumChildProc procedure,
                                               IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDesktopWindow();

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder name, int count);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam,
                                          IntPtr lParam);

    public static IntPtr FindTopLevel(uint processId, string targetClass)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, unused) =>
        {
            uint ownerProcessId;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId == processId)
            {
                var className = new StringBuilder(128);
                GetClassName(window, className, className.Capacity);
                if (className.ToString() == targetClass)
                {
                    found = window;
                    return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr FindProcessChild(uint processId, string targetClass)
    {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(GetDesktopWindow(), (window, unused) =>
        {
            uint ownerProcessId;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId == processId)
            {
                var className = new StringBuilder(128);
                GetClassName(window, className, className.Capacity);
                if (className.ToString() == targetClass)
                {
                    found = window;
                    return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

function Start-Lwe {
    param(
        [string]$Executable,
        [string]$TestImage
    )

    $arguments = @("--test-seconds=60")
    if (-not [string]::IsNullOrWhiteSpace($TestImage)) {
        $arguments += ('--test-wallpaper="{0}"' -f $TestImage)
    }

    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru
    $control = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweStaticOverlayProbe]::FindTopLevel(
            [uint32]$process.Id, "LiveWallpaperEngine.Control")
        if ($control -ne [IntPtr]::Zero) {
            break
        }
        if ($process.HasExited) {
            break
        }
    }
    if ($control -eq [IntPtr]::Zero) {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id
        }
        throw "Control window was not found. ExitCode=$($process.ExitCode)"
    }
    return @($process, $control)
}

function Exit-Lwe {
    param($Process, [IntPtr]$Control)

    # WM_COMMAND with the exit button identifier exercises the application's
    # normal shutdown path instead of forcefully terminating the process.
    [void][LweStaticOverlayProbe]::PostMessage($Control, 0x0111, [IntPtr]2199,
                                               [IntPtr]::Zero)
    if (-not $Process.WaitForExit(5000)) {
        throw "Application did not exit normally."
    }
    if ($Process.ExitCode -ne 0) {
        throw "Application returned exit code $($Process.ExitCode)."
    }
}

function Get-SettingsSnapshot {
    param([string[]]$Paths)

    $snapshot = foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            "$path=<absent>"
        } else {
            "$path=$([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)))"
        }
    }
    return $snapshot -join "`n"
}

function Get-GpuMemoryUsage {
    param([int]$ProcessId)

    try {
        $instances = @(Get-CimInstance `
            Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
            Where-Object { $_.Name -match "pid_$($ProcessId)_" })
        if ($instances.Count -eq 0) {
            return $null
        }
        return [pscustomobject]@{
            Dedicated = [uint64](($instances | Measure-Object DedicatedUsage -Sum).Sum)
            Shared = [uint64](($instances | Measure-Object SharedUsage -Sum).Sum)
        }
    } catch {
        return $null
    }
}

function Measure-LweIdle {
    param($Process)

    $Process.Refresh()
    $cpuBefore = $Process.TotalProcessorTime
    Start-Sleep -Seconds 3
    $Process.Refresh()
    $gpu = Get-GpuMemoryUsage -ProcessId $Process.Id
    return [pscustomobject]@{
        CpuDeltaMilliseconds =
            ($Process.TotalProcessorTime - $cpuBefore).TotalMilliseconds
        WorkingSetBytes = $Process.WorkingSet64
        DedicatedGpuBytes = if ($null -eq $gpu) { "unavailable" } else { $gpu.Dedicated }
        SharedGpuBytes = if ($null -eq $gpu) { "unavailable" } else { $gpu.Shared }
    }
}

$executable = Join-Path $PSScriptRoot `
    "..\out\x64\$Configuration\LiveWallpaperEngine.exe"
$executable = [IO.Path]::GetFullPath($executable)
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}

$settingsDirectory = Join-Path $env:LOCALAPPDATA "LiveWallpaperEngine"
$currentSettingsPath = Join-Path $settingsDirectory "settings.json"
$legacySettingsPath = Join-Path $settingsDirectory "settings.v1.json"
$settingsPaths = @($currentSettingsPath, $legacySettingsPath)
$temporaryImage = Join-Path $env:TEMP "LiveWallpaperEngine-overlay-test.bmp"
$baselineSettings = Get-SettingsSnapshot -Paths $settingsPaths
$baselineWallpaper = (Get-ItemProperty -LiteralPath "HKCU:\Control Panel\Desktop" `
                                            -Name WallPaper).WallPaper

$bitmap = New-Object System.Drawing.Bitmap 640, 360
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.Clear([System.Drawing.Color]::FromArgb(255, 24, 170, 86))
$bitmap.Save($temporaryImage, [System.Drawing.Imaging.ImageFormat]::Bmp)
$graphics.Dispose()
$bitmap.Dispose()

$activeProcess = $null
try {
    $started = Start-Lwe -Executable $executable -TestImage $temporaryImage
    $activeProcess = $started[0]
    $control = [IntPtr]$started[1]

    $wallpaperWindow = [LweStaticOverlayProbe]::FindProcessChild(
        [uint32]$activeProcess.Id, "LiveWallpaperEngine.Wallpaper")
    if ($wallpaperWindow -eq [IntPtr]::Zero) {
        throw "Wallpaper overlay window was not found."
    }

    $secondInstance = Start-Process -FilePath $executable -PassThru
    if (-not $secondInstance.WaitForExit(5000) -or $secondInstance.ExitCode -ne 0) {
        throw "Second instance did not activate the primary instance cleanly."
    }
    $instanceCount = @(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count
    if ($instanceCount -ne 1) {
        throw "Single-instance regression: expected 1 process, found $instanceCount."
    }

    $firstMetrics = Measure-LweIdle -Process $activeProcess
    if ((Get-ItemProperty -LiteralPath "HKCU:\Control Panel\Desktop" `
                          -Name WallPaper).WallPaper -ne $baselineWallpaper) {
        throw "System wallpaper changed while the overlay was active."
    }

    Exit-Lwe -Process $activeProcess -Control $control
    $activeProcess = $null

    if ((Get-ItemProperty -LiteralPath "HKCU:\Control Panel\Desktop" `
                          -Name WallPaper).WallPaper -ne $baselineWallpaper) {
        throw "System wallpaper changed after overlay exit."
    }
    if ((Get-SettingsSnapshot -Paths $settingsPaths) -ne $baselineSettings) {
        throw "Controlled test unexpectedly changed the saved wallpaper settings."
    }
    if (@($settingsPaths | Where-Object { Test-Path -LiteralPath ($_ + ".tmp") }).Count -ne 0) {
        throw "Atomic settings temporary file was left behind."
    }

    "TEST_IMAGE_CPU_DELTA_MS_3S=$($firstMetrics.CpuDeltaMilliseconds)"
    "TEST_IMAGE_WORKING_SET_BYTES=$($firstMetrics.WorkingSetBytes)"
    "TEST_IMAGE_GPU_DEDICATED_BYTES=$($firstMetrics.DedicatedGpuBytes)"
    "TEST_IMAGE_GPU_SHARED_BYTES=$($firstMetrics.SharedGpuBytes)"
    "SINGLE_INSTANCE_COUNT=$instanceCount"
    "SYSTEM_WALLPAPER_UNCHANGED=True"
    "SETTINGS_UNCHANGED=True"

    $savedPath = $null
    if (Test-Path -LiteralPath $currentSettingsPath -PathType Leaf) {
        $saved = Get-Content -Raw -Encoding utf8 $currentSettingsPath | ConvertFrom-Json
        if ($saved.version -eq 2) {
            $savedPath = $saved.wallpaperPath
        }
    } elseif (Test-Path -LiteralPath $legacySettingsPath -PathType Leaf) {
        $saved = Get-Content -Raw -Encoding utf8 $legacySettingsPath | ConvertFrom-Json
        if ($saved.version -eq 1 -and $saved.wallpaperType -eq "static_image") {
            $savedPath = $saved.staticImagePath
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($savedPath)) {
        if (Test-Path -LiteralPath $savedPath -PathType Leaf) {
            $started = Start-Lwe -Executable $executable
            $activeProcess = $started[0]
            $control = [IntPtr]$started[1]
            $restoredWindow = [LweStaticOverlayProbe]::FindProcessChild(
                [uint32]$activeProcess.Id, "LiveWallpaperEngine.Wallpaper")
            if ($restoredWindow -eq [IntPtr]::Zero) {
                throw "Saved static overlay window was not restored."
            }

            $restoredMetrics = Measure-LweIdle -Process $activeProcess
            Exit-Lwe -Process $activeProcess -Control $control
            $activeProcess = $null

            "SAVED_IMAGE_CPU_DELTA_MS_3S=$($restoredMetrics.CpuDeltaMilliseconds)"
            "SAVED_IMAGE_WORKING_SET_BYTES=$($restoredMetrics.WorkingSetBytes)"
            "SAVED_IMAGE_GPU_DEDICATED_BYTES=$($restoredMetrics.DedicatedGpuBytes)"
            "SAVED_IMAGE_GPU_SHARED_BYTES=$($restoredMetrics.SharedGpuBytes)"
            "SAVED_IMAGE_RESTORED=True"
        } else {
            "SAVED_IMAGE_RESTORED=SkippedInvalidOrMissingImage"
        }
    } else {
        "SAVED_IMAGE_RESTORED=SkippedNoSettings"
    }
} finally {
    if ($null -ne $activeProcess -and -not $activeProcess.HasExited) {
        Stop-Process -Id $activeProcess.Id -ErrorAction SilentlyContinue
    }

    $resolvedTemporary = [IO.Path]::GetFullPath($temporaryImage)
    $resolvedTempDirectory = [IO.Path]::GetFullPath($env:TEMP)
    if ($resolvedTemporary.StartsWith($resolvedTempDirectory,
                                      [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemporary)) {
        Remove-Item -LiteralPath $resolvedTemporary -Force
    }
}

"TEMP_IMAGE_REMOVED=$(-not (Test-Path -LiteralPath $temporaryImage))"
"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
