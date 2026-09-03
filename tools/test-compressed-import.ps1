[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $VideoPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FileSnapshot([string] $Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
    }
    return $null
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "out\x64\$Configuration\LiveWallpaperEngine.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}
if (-not (Test-Path -LiteralPath $VideoPath -PathType Leaf)) {
    throw "Video not found: $VideoPath"
}

$appDataRoot = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$settingsPath = Join-Path $appDataRoot 'settings.json'
$legacySettingsPath = Join-Path $appDataRoot 'settings.v1.json'
$logPath = Join-Path $appDataRoot 'logs\LiveWallpaperEngine.log'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySettingsSnapshot = Get-FileSnapshot $legacySettingsPath
$baselineLogLength = if (Test-Path -LiteralPath $logPath) {
    (Get-Item -LiteralPath $logPath).Length
} else { 0 }

$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) `
    ('LiveWallpaperEngine-import-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $temporaryDirectory)
$testLibrary = Join-Path $temporaryDirectory 'library'
$testName = 'compressed-import-' + [guid]::NewGuid().ToString('N') + '.mp4'
$testSource = Join-Path $temporaryDirectory $testName
$importedPath = Join-Path $testLibrary $testName
$sourceHash = $null
$process = $null

try {
    Copy-Item -LiteralPath $VideoPath -Destination $testSource
    $sourceHash = (Get-FileHash -LiteralPath $testSource -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $importedPath) {
        throw "Unexpected test collision: $importedPath"
    }

    $process = Start-Process -FilePath $executable -ArgumentList @(
        '--test-seconds=15',
        ('--test-compressed-import="{0}"' -f $testSource),
        ('--test-library-root="{0}"' -f $testLibrary)
    ) -PassThru
    if (-not $process.WaitForExit(30000)) {
        throw 'Compressed import integration test timed out.'
    }
    if ($process.ExitCode -ne 0) {
        throw "Compressed import integration test exited with $($process.ExitCode)."
    }
    $process = $null

    if (-not (Test-Path -LiteralPath $importedPath -PathType Leaf)) {
        throw 'The compressed file was not imported as a real library item.'
    }
    if ((Get-FileHash -LiteralPath $testSource -Algorithm SHA256).Hash -ne
        $sourceHash) {
        throw 'The selected source video was modified.'
    }
    if (Test-Path -LiteralPath (Join-Path $testLibrary '.optimized')) {
        throw 'The deprecated .optimized cache was recreated.'
    }

    $logBytes = [IO.File]::ReadAllBytes($logPath)
    $segment = [Text.Encoding]::UTF8.GetString(
        $logBytes, [int]$baselineLogLength,
        $logBytes.Length - [int]$baselineLogLength)
    $expectedLog = "Imported a compressed wallpaper: $importedPath, resolution=1920x1080"
    if (-not $segment.Contains($expectedLog)) {
        throw 'The imported library item was not reported at 1920x1080.'
    }
    if ($segment.Contains('Using a local optimized video copy:')) {
        throw 'Playback still used the deprecated optimized-cache path.'
    }

    'COMPRESSED_IMPORT_LIBRARY_ITEM=True'
    'COMPRESSED_IMPORT_RESOLUTION=1920x1080'
    'COMPRESSED_IMPORT_SOURCE_PRESERVED=True'
    'LEGACY_OPTIMIZED_CACHE_ABSENT=True'
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}

"SETTINGS_UNCHANGED=$((Get-FileSnapshot $settingsPath) -eq $settingsSnapshot)"
"LEGACY_SETTINGS_UNCHANGED=$((Get-FileSnapshot $legacySettingsPath) -eq $legacySettingsSnapshot)"
"TEMP_LIBRARY_REMOVED=$(-not (Test-Path -LiteralPath $testLibrary))"
"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
