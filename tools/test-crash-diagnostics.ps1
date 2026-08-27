[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string] $ExecutablePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
    [IO.Path]::GetFullPath(
        (Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
} else {
    [IO.Path]::GetFullPath($ExecutablePath)
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}
if (@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'Close the existing Live Wallpaper Engine process before this controlled test.'
}

$productVersion = [Version](Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
$expectedVersion = '{0}.{1}.{2}' -f $productVersion.Major,
    $productVersion.Minor, $productVersion.Build
$temporaryBase = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
$diagnosticRoot = [IO.Path]::GetFullPath(
    (Join-Path $temporaryBase ("LWE-crash-diagnostics-" + [guid]::NewGuid())))
if (-not ($diagnosticRoot + '\').StartsWith(
        $temporaryBase, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a diagnostic root outside the temporary directory: $diagnosticRoot"
}

$diagnosticsDirectory = Join-Path $diagnosticRoot 'diagnostics'
$crashDirectory = Join-Path $diagnosticRoot 'crashes'
$activeSessionPath = Join-Path $diagnosticsDirectory 'active-session.v1.json'
$lastSessionPath = Join-Path $diagnosticsDirectory 'last-session.v1.json'

function Invoke-DiagnosticMode([string] $Mode) {
    $arguments = @(
        "--test-crash-diagnostics=$Mode",
        "--test-crash-directory=`"$diagnosticRoot`""
    )
    $process = Start-Process -FilePath $executable -ArgumentList $arguments `
        -PassThru -Wait
    return $process.ExitCode
}

function Read-SessionRecord {
    if (-not (Test-Path -LiteralPath $lastSessionPath -PathType Leaf)) {
        throw "Session record was not created: $lastSessionPath"
    }
    return Get-Content -LiteralPath $lastSessionPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
}

try {
    [IO.Directory]::CreateDirectory($diagnosticRoot) | Out-Null
    [IO.Directory]::CreateDirectory($crashDirectory) | Out-Null
    for ($index = 0; $index -lt 12; $index++) {
        $fixture = Join-Path $crashDirectory (
            'LiveWallpaperEngine-v0.0.0-20000101T0000{0:D2}000Z-pid1.dmp' -f $index)
        [IO.File]::WriteAllBytes($fixture, [byte[]](0x4D, 0x44, 0x4D, 0x50))
        [IO.File]::SetLastWriteTimeUtc(
            $fixture, [datetime]::UtcNow.AddMinutes(-100 - $index))
    }

    $cleanExitCode = Invoke-DiagnosticMode 'clean'
    $clean = Read-SessionRecord
    if ($cleanExitCode -ne 0 -or $clean.status -ne 'clean' -or
        $clean.version -ne $expectedVersion -or
        (Test-Path -LiteralPath $activeSessionPath)) {
        throw 'A clean exit was not recorded with the expected version.'
    }
    if ((Invoke-DiagnosticMode 'verify-clean') -ne 0) {
        throw 'The next process did not recognize the previous clean exit.'
    }

    $crashExitCode = Invoke-DiagnosticMode 'crash'
    if ($crashExitCode -eq 0) {
        throw 'The controlled crash unexpectedly returned a successful exit code.'
    }
    $crashed = Read-SessionRecord
    if ($crashed.status -ne 'crashed' -or
        $crashed.version -ne $expectedVersion -or
        $crashed.exceptionCode -ne '0xE0424C57' -or
        [string]::IsNullOrWhiteSpace([string]$crashed.dumpFile)) {
        throw 'The controlled crash record is incomplete or has unexpected values.'
    }
    $dumpPath = Join-Path $crashDirectory ([string]$crashed.dumpFile)
    $expectedPrefix = "LiveWallpaperEngine-v$expectedVersion-"
    if (-not ([string]$crashed.dumpFile).StartsWith(
            $expectedPrefix, [StringComparison]::Ordinal) -or
        -not (Test-Path -LiteralPath $dumpPath -PathType Leaf)) {
        throw 'The versioned crash dump was not created at the recorded path.'
    }
    $dumpBytes = [IO.File]::ReadAllBytes($dumpPath)
    if ($dumpBytes.Length -lt 4 -or
        [Text.Encoding]::ASCII.GetString($dumpBytes, 0, 4) -ne 'MDMP') {
        throw 'The generated crash file does not contain a MiniDump signature.'
    }
    if (Test-Path -LiteralPath $activeSessionPath) {
        throw 'The active marker remained after a recorded crash.'
    }
    $retainedDumps = @(Get-ChildItem -LiteralPath $crashDirectory -Filter `
            'LiveWallpaperEngine-v*.dmp' -File)
    if ($retainedDumps.Count -gt 10) {
        throw "Crash dump retention exceeded the limit: $($retainedDumps.Count)"
    }
    if ((Invoke-DiagnosticMode 'verify-crash') -ne 0) {
        throw 'The next process did not recognize the previous crash.'
    }

    $uncleanExitCode = Invoke-DiagnosticMode 'leave-unclean'
    if ($uncleanExitCode -ne 77 -or
        -not (Test-Path -LiteralPath $activeSessionPath -PathType Leaf)) {
        throw 'The controlled forced exit did not leave an active-session marker.'
    }
    if ((Invoke-DiagnosticMode 'verify-unclean') -ne 0) {
        throw 'The next process did not recognize the previous unclean exit.'
    }
    $final = Read-SessionRecord
    if ($final.status -ne 'clean' -or
        (Test-Path -LiteralPath $activeSessionPath)) {
        throw 'The verification process did not finish with a clean session record.'
    }

    Write-Output 'CLEAN_EXIT_RECORDED=True'
    Write-Output 'VERSIONED_DUMP=True'
    Write-Output 'MINIDUMP_SIGNATURE=True'
    Write-Output 'DUMP_RETENTION_BOUNDED=True'
    Write-Output 'CRASH_STATUS_RECOGNIZED=True'
    Write-Output 'UNCLEAN_STATUS_RECOGNIZED=True'
    Write-Output 'ACTIVE_MARKER_CLEARED=True'
    Write-Output "CRASH_DIAGNOSTICS_VERSION=$expectedVersion"
}
finally {
    if (Test-Path -LiteralPath $diagnosticRoot) {
        Remove-Item -LiteralPath $diagnosticRoot -Recurse -Force
    }
}
