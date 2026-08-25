[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $ImagePath,
    [Parameter(Mandatory = $true)]
    [string] $VideoPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweMultiDisplayProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    public delegate bool EnumChildProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr w, EnumChildProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);

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

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
$resolvedImage = (Resolve-Path -LiteralPath $ImagePath).Path
$resolvedVideo = (Resolve-Path -LiteralPath $VideoPath).Path
foreach ($required in @($executable, $resolvedImage, $resolvedVideo)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}
if (@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'Close the existing Live Wallpaper Engine process before this controlled test.'
}

$screens = @([System.Windows.Forms.Screen]::AllScreens | Sort-Object { $_.Bounds.X }, { $_.Bounds.Y })
if ($screens.Count -lt 2) {
    throw "At least two active displays are required; found $($screens.Count)."
}
$firstDisplay = $screens[0].DeviceName
$secondDisplay = $screens[1].DeviceName
$settingsDirectory = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySnapshot = Get-FileSnapshot $legacySettingsPath
$baselineWallpaper = (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
$logPath = Join-Path $settingsDirectory 'logs\LiveWallpaperEngine.log'
$baselineLogBytes = if (Test-Path -LiteralPath $logPath) { (Get-Item -LiteralPath $logPath).Length } else { 0 }
$process = $null
$normalExit = $false

try {
    [IO.Directory]::CreateDirectory($settingsDirectory) | Out-Null
    $testSettings = [ordered]@{
        version = 4
        soundEnabled = $false
        selectedDisplayTargets = $secondDisplay
        spanSelection = $false
        assignments = @(
            [ordered]@{
                wallpaperType = 'static_image'
                wallpaperPath = $resolvedImage
                # Deliberately overlaps the second display. The later video
                # assignment must win there while the image stays on display 1.
                displayTargets = "$firstDisplay|$secondDisplay"
                spanAcrossDisplays = $false
            },
            [ordered]@{
                wallpaperType = 'video'
                wallpaperPath = $resolvedVideo
                displayTargets = $secondDisplay
                spanAcrossDisplays = $false
            }
        )
    }
    $json = $testSettings | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText($settingsPath, $json, [Text.UTF8Encoding]::new($false))

    $process = Start-Process -FilePath $executable -ArgumentList '--test-seconds=60' -PassThru
    $control = [IntPtr]::Zero
    $wallpaper = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweMultiDisplayProbe]::Find([uint32]$process.Id, 'LiveWallpaperEngine.Control', $false)
        $wallpaper = [LweMultiDisplayProbe]::Find([uint32]$process.Id, 'LiveWallpaperEngine.Wallpaper', $true)
        if ($control -ne [IntPtr]::Zero -and $wallpaper -ne [IntPtr]::Zero) { break }
        if ($process.HasExited) { break }
    }
    if ($control -eq [IntPtr]::Zero -or $wallpaper -eq [IntPtr]::Zero) {
        throw 'The control or wallpaper window was not created.'
    }
    Start-Sleep -Seconds 4
    if (-not [LweMultiDisplayProbe]::IsWindowVisible($wallpaper)) {
        throw 'The multi-display wallpaper window is not visible.'
    }

    # The save hook exists only in controlled mode. It verifies that the app's
    # own v4 serializer round-trips both assignments before this script restores
    # the user's exact original bytes.
    [void][LweMultiDisplayProbe]::PostMessage(
        $control, 0x0111, [IntPtr]2198, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    $saved = Get-Content -Raw -Encoding UTF8 $settingsPath | ConvertFrom-Json
    if ($saved.version -ne 4 -or @($saved.assignments).Count -ne 2) {
        throw 'The overlapping v4 settings were not normalized to two assignments.'
    }
    $normalizedTargets = @{}
    foreach ($assignment in @($saved.assignments)) {
        foreach ($identifier in @($assignment.displayTargets -split '\|')) {
            if ($normalizedTargets.ContainsKey($identifier)) {
                throw "Display remained assigned to multiple wallpapers: $identifier"
            }
            $normalizedTargets[$identifier] = $assignment.wallpaperPath
        }
    }
    $imageAssignment = @($saved.assignments | Where-Object wallpaperPath -EQ $resolvedImage)
    $videoAssignment = @($saved.assignments | Where-Object wallpaperPath -EQ $resolvedVideo)
    if ($imageAssignment.Count -ne 1 -or
        $imageAssignment[0].displayTargets -ne $firstDisplay -or
        $videoAssignment.Count -ne 1 -or
        $videoAssignment[0].displayTargets -ne $secondDisplay) {
        throw 'The later assignment did not exclusively replace its overlapping screen.'
    }
    $process.Refresh()
    $workingSet = $process.WorkingSet64
    [void][LweMultiDisplayProbe]::PostMessage($control, 0x0111, [IntPtr]2199, [IntPtr]::Zero)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'The multi-display test process did not exit normally.'
    }
    $normalExit = $true
    $process = $null
    $logBytes = [IO.File]::ReadAllBytes($logPath)
    $logSegment = [Text.Encoding]::UTF8.GetString(
        $logBytes, [int]$baselineLogBytes, $logBytes.Length - [int]$baselineLogBytes)
    foreach ($required in @(
        'Loaded local wallpaper settings.',
        'CONTROLLED_SETTINGS_SAVE=True',
        "targets=$firstDisplay",
        "targets=$secondDisplay",
        'Static wallpaper session presented one frame.',
        'Media Engine frame server opened a looping video; audio is muted:',
        'Per-video transfer surface created at')) {
        if (-not $logSegment.Contains($required)) {
            throw "Expected multi-display evidence was not logged: $required"
        }
    }
    $transfers = [regex]::Matches(
        $logSegment, 'Video frame-server transfers presented=(\d+)\.')
    if ($transfers.Count -ne 1 -or [uint64]$transfers[0].Groups[1].Value -eq 0) {
        throw 'The independently assigned video did not present decoded frames.'
    }
    if ((Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper -ne $baselineWallpaper) {
        throw 'The multi-display overlay changed the Windows system wallpaper.'
    }

    "FIRST_DISPLAY=$firstDisplay"
    "SECOND_DISPLAY=$secondDisplay"
    'DISTINCT_WALLPAPER_SESSIONS=2'
    'OVERLAPPING_ASSIGNMENTS_NORMALIZED=True'
    'STATIC_AND_VIDEO_CONCURRENT=True'
    "WORKING_SET_BYTES=$workingSet"
    'SYSTEM_WALLPAPER_UNCHANGED=True'
    "NORMAL_EXIT=$normalExit"
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySnapshot
}

"SETTINGS_RESTORED=$((Get-FileSnapshot $settingsPath) -eq $settingsSnapshot)"
"LEGACY_SETTINGS_RESTORED=$((Get-FileSnapshot $legacySettingsPath) -eq $legacySnapshot)"
"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
