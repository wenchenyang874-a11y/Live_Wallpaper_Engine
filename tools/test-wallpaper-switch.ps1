[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $ImagePath,
    [Parameter(Mandatory = $true)]
    [string] $VideoPath,
    [ValidateRange(6, 60)]
    [int] $TestSeconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FileSnapshot([string] $Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
    }
    return $null
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = [IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
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
        "--test-seconds=$TestSeconds",
        ('--test-wallpaper="{0}"' -f $resolvedImage),
        ('--test-wallpaper="{0}"' -f $resolvedVideo)
    )
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
    Start-Sleep -Seconds ($TestSeconds - 2)
    $process.Refresh()
    if ($process.HasExited) {
        throw "Wallpaper switching exited early with code $($process.ExitCode)."
    }
    if (-not $process.WaitForExit(7000) -or $process.ExitCode -ne 0) {
        throw 'Wallpaper switching did not finish with a normal zero exit code.'
    }
    $process = $null

    $logBytes = [IO.File]::ReadAllBytes($logPath)
    $logSegment = [Text.Encoding]::UTF8.GetString(
        $logBytes, [int]$baselineLogBytes,
        $logBytes.Length - [int]$baselineLogBytes)
    $switchCount = [regex]::Matches(
        $logSegment, 'Controlled wallpaper switch completed; count=').Count
    if ($switchCount -lt 20) {
        throw "Only $switchCount controlled switches completed; expected at least 20."
    }
    foreach ($requiredLog in @(
        'Static wallpaper session presented one frame.',
        'Media Engine frame server opened a looping video; audio is muted:',
        'Controlled test duration completed.')) {
        if (-not $logSegment.Contains($requiredLog)) {
            throw "Expected switching evidence was not logged: $requiredLog"
        }
    }
    if ((Get-FileSnapshot $settingsPath) -ne $settingsSnapshot -or
        (Get-FileSnapshot $legacySettingsPath) -ne $legacySettingsSnapshot) {
        throw 'Controlled wallpaper switching changed the saved settings.'
    }
    if ((Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' `
            -Name WallPaper).WallPaper -ne $baselineWallpaper) {
        throw 'Controlled wallpaper switching changed the Windows system wallpaper.'
    }

    "SWITCH_COUNT=$switchCount"
    'EARLY_EXIT=False'
    'NORMAL_EXIT=True'
    'SETTINGS_UNCHANGED=True'
    'SYSTEM_WALLPAPER_UNCHANGED=True'
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
}

"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
