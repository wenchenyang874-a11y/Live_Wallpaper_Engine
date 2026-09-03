[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $VideoPath,
    [switch] $ExpectNoCompression
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repositoryRoot `
    "out\x64\$Configuration\LiveWallpaperEngine.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}
if (-not (Test-Path -LiteralPath $VideoPath -PathType Leaf)) {
    throw "Video not found: $VideoPath"
}

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testDirectory = Join-Path $temporaryRoot `
    ("LiveWallpaperEngine-optimizer-" + [guid]::NewGuid().ToString('N'))
$resolvedTestDirectory = [IO.Path]::GetFullPath($testDirectory)
if (-not $resolvedTestDirectory.StartsWith(
        $temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The optimizer test directory did not resolve inside the temp directory.'
}
[void](New-Item -ItemType Directory -Path $resolvedTestDirectory)
$output = Join-Path $resolvedTestDirectory 'optimized.mp4'
try {
    $arguments = @(
        ('--test-video-optimizer="{0}"' -f $VideoPath),
        ('--test-video-optimizer-output="{0}"' -f $output)
    )
    if ($ExpectNoCompression) {
        $arguments += '--test-video-optimizer-expect-skip'
    }
    $process = Start-Process -FilePath $executable -ArgumentList $arguments `
        -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit(300000)) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        throw 'Video optimizer self-test timed out.'
    }
    if ($process.ExitCode -ne 0) {
        throw "Video optimizer self-test exited with code $($process.ExitCode)."
    }
    if ($ExpectNoCompression) {
        if (Test-Path -LiteralPath $output -PathType Leaf) {
            throw 'A compression output was created for a source at or below the target size.'
        }
        'VIDEO_OPTIMIZER_SKIP_SELF_TEST=True'
        'ORIGINAL_PRESERVED=True'
        return
    }
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw 'The optimized video was not created.'
    }
    $sourceBytes = (Get-Item -LiteralPath $VideoPath).Length
    $outputBytes = (Get-Item -LiteralPath $output).Length
    if ($outputBytes -le 0) {
        throw 'The optimized video is empty.'
    }
    "VIDEO_OPTIMIZER_SELF_TEST=True"
    "VIDEO_OPTIMIZER_LIBRARY_IMPORT=True"
    "VIDEO_OPTIMIZER_LEGACY_CACHE_CLEANUP=True"
    "SOURCE_BYTES=$sourceBytes"
    "OPTIMIZED_BYTES=$outputBytes"
    "ORIGINAL_PRESERVED=$((Get-Item -LiteralPath $VideoPath).Length -eq $sourceBytes)"
} finally {
    if (Test-Path -LiteralPath $resolvedTestDirectory) {
        Remove-Item -LiteralPath $resolvedTestDirectory -Recurse -Force
    }
}
