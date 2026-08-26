[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [ValidateSet('Available', 'Current', 'Error')]
    [string] $ExpectedStatus = 'Available',
    [string] $ExpectedErrorContains = '',
    [switch] $SimulateRateLimit,
    [string] $ScreenshotPath = (Join-Path $env:TEMP 'LWE-update-check.png'),
    [string] $ResultScreenshotPath =
        (Join-Path $env:TEMP 'LWE-update-result.png')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweUpdateProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w, int command);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w, IntPtr dc, uint flags);

    public static IntPtr Find(uint processId, string targetClass)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            var name = new StringBuilder(128);
            GetClassName(window, name, name.Capacity);
            if (owner == processId && name.ToString() == targetClass) {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static string Text(IntPtr window)
    {
        var text = new StringBuilder(256);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }

    public static IntPtr Point(int x, int y)
    {
        long value = ((long)(y & 0xffff) << 16) | (uint)(x & 0xffff);
        return (IntPtr)value;
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

function Read-SharedBytes([string] $Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $memory = [IO.MemoryStream]::new()
        $stream.CopyTo($memory)
        return $memory.ToArray()
    } finally {
        $stream.Dispose()
    }
}

function Wait-Window([int] $ProcessId, [string] $ClassName, [int] $Attempts = 100) {
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        $window = [LweUpdateProbe]::Find([uint32]$ProcessId, $ClassName)
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 100
    }
    return [IntPtr]::Zero
}

function Save-WindowScreenshot([IntPtr] $Window, [string] $Path) {
    $bounds = New-Object LweUpdateProbe+RECT
    if (-not [LweUpdateProbe]::GetWindowRect($Window, [ref]$bounds)) {
        throw 'Unable to read the main-window bounds.'
    }
    $bitmap = [System.Drawing.Bitmap]::new(
        $bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size,
            [System.Drawing.CopyPixelOperation]::SourceCopy)
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Save-RenderedWindowScreenshot([IntPtr] $Window, [string] $Path) {
    $bounds = New-Object LweUpdateProbe+RECT
    [void][LweUpdateProbe]::GetWindowRect($Window, [ref]$bounds)
    $bitmap = [System.Drawing.Bitmap]::new(
        $bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $deviceContext = $graphics.GetHdc()
    try {
        if (-not [LweUpdateProbe]::PrintWindow($Window, $deviceContext, 2)) {
            throw 'PrintWindow failed while capturing the update result.'
        }
    } finally {
        $graphics.ReleaseHdc($deviceContext)
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = [IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}
if (@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'Close the existing Live Wallpaper Engine process before this controlled test.'
}

$settingsDirectory = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$logPath = Join-Path $settingsDirectory 'logs\LiveWallpaperEngine.log'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySettingsSnapshot = Get-FileSnapshot $legacySettingsPath
$baselineWallpaper =
    (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
$logLength = if (Test-Path -LiteralPath $logPath) {
    (Get-Item -LiteralPath $logPath).Length
} else { 0L }
$process = $null
$dialog = [IntPtr]::Zero

try {
    [IO.Directory]::CreateDirectory($settingsDirectory) | Out-Null
    $testSettings = [ordered]@{
        version = 4
        soundEnabled = $false
        selectedDisplayTargets = ''
        spanSelection = $true
        assignments = @()
    }
    [IO.File]::WriteAllText(
        $settingsPath, ($testSettings | ConvertTo-Json -Depth 5),
        [Text.UTF8Encoding]::new($false))

    $arguments = @('--test-seconds=45')
    if ($SimulateRateLimit) {
        $arguments += '--test-update-result=rate-limit'
    }
    $process = Start-Process -FilePath $executable `
        -ArgumentList $arguments -PassThru
    $control = Wait-Window $process.Id 'LiveWallpaperEngine.Control'
    if ($control -eq [IntPtr]::Zero) {
        throw 'The main control window was not created.'
    }
    [void][LweUpdateProbe]::ShowWindow($control, 5)
    Start-Sleep -Seconds 1
    $updateButton = Wait-Window $process.Id 'LiveWallpaperEngine.UpdateButton'
    if ($updateButton -eq [IntPtr]::Zero) {
        throw 'The Check for updates title-bar button was not created.'
    }

    $newLog = if (Test-Path -LiteralPath $logPath) {
        $bytes = Read-SharedBytes $logPath
        if ($bytes.Length -gt $logLength) {
            [Text.Encoding]::UTF8.GetString($bytes, [int]$logLength,
                $bytes.Length - [int]$logLength)
        } else { '' }
    } else { '' }
    if ($newLog.Contains('Manual update check started.')) {
        throw 'The application checked for updates without a user click.'
    }

    $bounds = New-Object LweUpdateProbe+RECT
    $buttonBounds = New-Object LweUpdateProbe+RECT
    [void][LweUpdateProbe]::GetWindowRect($control, [ref]$bounds)
    [void][LweUpdateProbe]::GetWindowRect($updateButton, [ref]$buttonBounds)
    if ($buttonBounds.Top -lt $bounds.Top -or
        $buttonBounds.Bottom -gt ($bounds.Top + 40) -or
        $buttonBounds.Left -le ($bounds.Left + 100)) {
        throw 'The Check for updates button is not positioned in the title bar.'
    }

    Save-WindowScreenshot $control $ScreenshotPath
    $buttonX = [Math]::Max(1, ($buttonBounds.Right - $buttonBounds.Left) / 2)
    $buttonY = [Math]::Max(1, ($buttonBounds.Bottom - $buttonBounds.Top) / 2)
    [void][LweUpdateProbe]::PostMessage(
        $updateButton, 0x0201, [IntPtr]1, [LweUpdateProbe]::Point($buttonX, $buttonY))
    [void][LweUpdateProbe]::PostMessage(
        $updateButton, 0x0202, [IntPtr]::Zero,
        [LweUpdateProbe]::Point($buttonX, $buttonY))

    $dialog = Wait-Window $process.Id 'LiveWallpaperEngine.UpdateResult' 160
    if ($dialog -eq [IntPtr]::Zero) {
        throw 'The update result dialog did not appear after the user click.'
    }
    $updateWindowTitle = -join @(
        [char]0x68C0, [char]0x67E5, [char]0x66F4, [char]0x65B0)
    if ([LweUpdateProbe]::Text($dialog) -ne $updateWindowTitle) {
        throw 'The update result window title is incorrect.'
    }
    $primary = [LweUpdateProbe]::GetDlgItem($dialog, 2300)
    $primaryText = [LweUpdateProbe]::Text($primary)
    $availableText = -join @(
        [char]0x524D, [char]0x5F80, [char]0x4E0B, [char]0x8F7D)
    $currentText = -join @([char]0x77E5, [char]0x9053, [char]0x4E86)
    $releaseText = (-join @([char]0x67E5, [char]0x770B)) + ' Release'
    $expectedText = switch ($ExpectedStatus) {
        'Available' { $availableText }
        'Current' { $currentText }
        'Error' { $releaseText }
    }
    if ($primaryText -ne $expectedText) {
        throw "Unexpected update action '$primaryText'; expected '$expectedText'."
    }
    [void][LweUpdateProbe]::ShowWindow($dialog, 5)
    Start-Sleep -Milliseconds 150
    Save-RenderedWindowScreenshot $dialog $ResultScreenshotPath

    $bytes = Read-SharedBytes $logPath
    $newLog = [Text.Encoding]::UTF8.GetString(
        $bytes, [int]$logLength, $bytes.Length - [int]$logLength)
    if (-not $newLog.Contains('Manual update check started.') -or
        -not $newLog.Contains('Manual update check completed:')) {
        throw 'The manual update-check lifecycle was not recorded in the log.'
    }
    $failed = $newLog.Contains('Manual update check failed:')
    if ($ExpectedStatus -eq 'Error') {
        if (-not $failed) {
            throw 'The expected update error was not recorded in the log.'
        }
        if (-not [string]::IsNullOrEmpty($ExpectedErrorContains) -and
            -not $newLog.Contains($ExpectedErrorContains)) {
            throw "The update error did not contain '$ExpectedErrorContains'."
        }
    } elseif ($failed) {
        throw "The '$ExpectedStatus' result was actually an update-check failure."
    } elseif ($newLog -notmatch 'latest=v[0-9]+\.[0-9]+\.[0-9]+\.') {
        throw "The '$ExpectedStatus' result did not include a valid latest version."
    }
    Write-Output "Manual update check passed: status=$ExpectedStatus screenshots=$ScreenshotPath,$ResultScreenshotPath"
} finally {
    if ($dialog -ne [IntPtr]::Zero) {
        [void][LweUpdateProbe]::PostMessage(
            $dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if ($null -ne $process -and -not $process.HasExited) {
        $control = [LweUpdateProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.Control')
        if ($control -ne [IntPtr]::Zero) {
            [void][LweUpdateProbe]::PostMessage(
                $control, 0x0111, [IntPtr]2199, [IntPtr]::Zero)
            [void]$process.WaitForExit(5000)
        }
    }
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySettingsSnapshot
    $currentWallpaper =
        (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
    if ($currentWallpaper -ne $baselineWallpaper) {
        throw 'The controlled update check changed the Windows wallpaper setting.'
    }
}
