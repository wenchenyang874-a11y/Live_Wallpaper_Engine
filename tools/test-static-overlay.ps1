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
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

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

    [DllImport("user32.dll")]
    public static extern IntPtr GetWindowLongPtr(IntPtr window, int index);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr parent, int identifier);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rectangle);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);

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

    public static string ReadWindowText(IntPtr window)
    {
        var text = new StringBuilder(256);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
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

    $controlStyle = [LweStaticOverlayProbe]::GetWindowLongPtr($control, -16).ToInt64()
    $requiredClippingStyles = 0x02000000 -bor 0x04000000
    if (($controlStyle -band $requiredClippingStyles) -ne $requiredClippingStyles) {
        throw "Control window is missing child/sibling clipping styles."
    }

    $searchControl = [LweStaticOverlayProbe]::GetDlgItem($control, 1100)
    $filterControl = [LweStaticOverlayProbe]::GetDlgItem($control, 1101)
    if ($searchControl -eq [IntPtr]::Zero -or $filterControl -eq [IntPtr]::Zero) {
        throw "Search or filter selector control was not created."
    }
    $filterText = [LweStaticOverlayProbe]::ReadWindowText($filterControl)
    # Windows PowerShell 5 reads UTF-8 scripts without a BOM using the active
    # ANSI code page. Build the expected Unicode label from code points so this
    # regression script remains portable without changing the repository-wide
    # text encoding convention.
    $expectedFilterText = -join @(
        [char]0x5206, [char]0x7C7B, [char]0xFF1A,
        [char]0x5168, [char]0x90E8, ' ', ' ', [char]0x25BC)
    if ($filterText -ne $expectedFilterText) {
        throw "Unexpected filter selector text: '$filterText'."
    }
    $legacyFilterCount = @(1109, 1110, 1111 | Where-Object {
        [LweStaticOverlayProbe]::GetDlgItem($control, $_) -ne [IntPtr]::Zero
    }).Count
    if ($legacyFilterCount -ne 0) {
        throw "Legacy segmented filter controls are still present."
    }
    $searchRectangle = New-Object LweStaticOverlayProbe+Rect
    $filterRectangle = New-Object LweStaticOverlayProbe+Rect
    if (-not [LweStaticOverlayProbe]::GetWindowRect(
            $searchControl, [ref]$searchRectangle) -or
        -not [LweStaticOverlayProbe]::GetWindowRect(
            $filterControl, [ref]$filterRectangle)) {
        throw "Unable to inspect search/filter geometry."
    }
    $searchCenter = ($searchRectangle.Top + $searchRectangle.Bottom) / 2.0
    $filterCenter = ($filterRectangle.Top + $filterRectangle.Bottom) / 2.0
    $searchCenterDelta = [Math]::Abs($searchCenter - $filterCenter)
    if ($searchCenterDelta -gt 1.0) {
        throw "Search text client is not vertically centered: delta=$searchCenterDelta px."
    }

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
    "CONTROL_WINDOW_CHILD_CLIPPING=True"
    "FILTER_DROPDOWN_TEXT=$filterText"
    "LEGACY_FILTER_SEGMENT_COUNT=$legacyFilterCount"
    "SEARCH_CENTER_DELTA_PX=$searchCenterDelta"
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
