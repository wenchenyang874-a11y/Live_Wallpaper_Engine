[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Version,

    [switch] $Unreleased
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^(?<major>0|[1-9]\d*)\.(?<minor>0|[1-9]\d*)\.(?<patch>0|[1-9]\d*)$') {
    throw "Version must contain exactly three numeric components, for example 1.2.3."
}

# Save regex captures immediately because later PowerShell matches overwrite $Matches.
$versionMajor = $Matches.major
$versionMinor = $Matches.minor
$versionPatch = $Matches.patch
$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Find-MSBuild {
    $command = Get-Command 'MSBuild.exe' -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vsWhere) {
        $result = & $vsWhere -latest -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($result) {
            return $result
        }
    }
    throw 'MSBuild.exe was not found. Install Visual Studio 2022 with Desktop development with C++.'
}

function Find-InnoCompiler {
    $command = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    throw 'ISCC.exe was not found. Install Inno Setup 6 before building an installer.'
}

function Get-ChineseMessagesFile {
    $translationCommit = '69a2554fc9551f1d3da8df8ba659007dea3f906f'
    $expectedHash = 'E0B0B350E2245F3C5E65586DFE43D574F6E7F06F2261149ABA284954B3FC9A8D'
    $translationPath = Join-Path $env:TEMP `
        "LiveWallpaperEngine-ChineseSimplified-$translationCommit.isl"
    $downloadPath = $translationPath + '.download'

    if (-not (Test-Path -LiteralPath $translationPath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $translationPath -Algorithm SHA256).Hash -ne
            $expectedHash) {
        $url = "https://raw.githubusercontent.com/jrsoftware/issrc/$translationCommit/Files/Languages/ChineseSimplified.isl"
        Invoke-WebRequest -Uri $url -OutFile $downloadPath
        $actualHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash
        if ($actualHash -ne $expectedHash) {
            Remove-Item -LiteralPath $downloadPath -Force
            throw "Chinese installer translation SHA-256 mismatch: $actualHash"
        }
        Move-Item -LiteralPath $downloadPath -Destination $translationPath -Force
    }
    return $translationPath
}

$msBuild = Find-MSBuild
$innoCompiler = Find-InnoCompiler
$chineseMessagesFile = Get-ChineseMessagesFile
$solution = Join-Path $repositoryRoot 'LiveWallpaperEngine.sln'
$installerScript = Join-Path $repositoryRoot 'installer\LiveWallpaperEngine.iss'
$releaseExecutable = Join-Path $repositoryRoot 'out\x64\Release\LiveWallpaperEngine.exe'
$distributionDirectory = Join-Path $repositoryRoot 'dist'
if ($Unreleased) {
    # Keep development packages visibly separate from immutable tagged assets.
    # The numeric base version remains suitable for Windows version resources.
    $displayVersion = "$Version-unreleased"
    $outputBaseFilename = 'LiveWallpaperEngine-Unreleased-x64-setup'
    $checksumName = 'SHA256SUMS-Unreleased.txt'
}
else {
    $displayVersion = $Version
    $outputBaseFilename = "LiveWallpaperEngine-$Version-x64-setup"
    $checksumName = 'SHA256SUMS.txt'
}
$installerName = "$outputBaseFilename.exe"
$installerPath = Join-Path $distributionDirectory $installerName
$checksumPath = Join-Path $distributionDirectory $checksumName

& $msBuild $solution /m /p:Configuration=Release /p:Platform=x64 `
    "/p:LweVersionMajor=$versionMajor" "/p:LweVersionMinor=$versionMinor" `
    "/p:LweVersionPatch=$versionPatch" /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$versionInfo = (Get-Item -LiteralPath $releaseExecutable).VersionInfo
if ($versionInfo.ProductVersion -ne "$Version.0") {
    throw "Executable product version '$($versionInfo.ProductVersion)' does not match '$Version.0'."
}

[void](New-Item -ItemType Directory -Path $distributionDirectory -Force)
foreach ($artifact in @($installerPath, $checksumPath)) {
    if (Test-Path -LiteralPath $artifact) {
        Remove-Item -LiteralPath $artifact -Force
    }
}

& $innoCompiler "/DMyAppVersion=$Version" `
    "/DMyAppDisplayVersion=$displayVersion" `
    "/DMyOutputBaseFilename=$outputBaseFilename" `
    "/DChineseMessagesFile=$chineseMessagesFile" $installerScript
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $installerPath)) {
    throw "Inno Setup failed to create '$installerName'."
}

$hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $checksumPath -Value "$hash *$installerName" -Encoding ascii

Write-Host "Installer: $installerPath"
Write-Host "SHA-256 manifest: $checksumPath"
