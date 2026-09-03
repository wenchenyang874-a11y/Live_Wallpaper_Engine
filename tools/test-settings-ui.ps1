[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $VideoPath,
    [string] $ScreenshotPath =
        (Join-Path $env:TEMP 'LWE-performance-settings.png'),
    [string] $TooltipScreenshotPath =
        (Join-Path $env:TEMP 'LWE-performance-settings-tooltip.png')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class LweSettingsUiProbe {
    public delegate bool EnumWindowsProc(IntPtr w, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr w, int i);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w, int c);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w, IntPtr dc, uint flags);
    public static IntPtr Find(uint pid, string targetClass, bool visibleOnly) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((w, p) => {
            uint owner; GetWindowThreadProcessId(w, out owner);
            var name = new StringBuilder(128); GetClassName(w, name, name.Capacity);
            if (owner == pid && name.ToString() == targetClass &&
                (!visibleOnly || IsWindowVisible(w))) { found = w; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindChild(IntPtr parent, string targetClass, bool visibleOnly) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (w, p) => {
            var name = new StringBuilder(128); GetClassName(w, name, name.Capacity);
            if (name.ToString() == targetClass &&
                (!visibleOnly || IsWindowVisible(w))) { found = w; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string Text(IntPtr w) {
        var text = new StringBuilder(256); GetWindowText(w, text, text.Capacity);
        return text.ToString();
    }
    public static IntPtr Point(int x, int y) {
        return (IntPtr)(((long)(y & 0xffff) << 16) | (uint)(x & 0xffff));
    }
    public static IntPtr Command(int identifier, int notification) {
        return (IntPtr)((notification << 16) | (identifier & 0xffff));
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

function Wait-Window([int] $ProcessId, [string] $ClassName, [bool] $VisibleOnly = $false) {
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $window = [LweSettingsUiProbe]::Find(
            [uint32]$ProcessId, $ClassName, $VisibleOnly)
        if ($window -ne [IntPtr]::Zero) { return $window }
    }
    return [IntPtr]::Zero
}

function Click-Window([IntPtr] $Window) {
    $bounds = New-Object LweSettingsUiProbe+RECT
    [void][LweSettingsUiProbe]::GetWindowRect($Window, [ref]$bounds)
    $x = [Math]::Max(1, ($bounds.Right - $bounds.Left) / 2)
    $y = [Math]::Max(1, ($bounds.Bottom - $bounds.Top) / 2)
    [void][LweSettingsUiProbe]::PostMessage(
        $Window, 0x0201, [IntPtr]1, [LweSettingsUiProbe]::Point($x, $y))
    [void][LweSettingsUiProbe]::PostMessage(
        $Window, 0x0202, [IntPtr]::Zero, [LweSettingsUiProbe]::Point($x, $y))
}

$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root "out\x64\$Configuration\LiveWallpaperEngine.exe"
$settingsDirectory = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$logPath = Join-Path $settingsDirectory 'logs\LiveWallpaperEngine.log'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySnapshot = Get-FileSnapshot $legacySettingsPath
$baselineLogLength = if (Test-Path -LiteralPath $logPath) {
    (Get-Item -LiteralPath $logPath).Length
} else { 0 }
$process = $null

try {
    $process = Start-Process -FilePath $executable -ArgumentList @(
        '--test-seconds=60', ('--test-wallpaper="{0}"' -f $VideoPath)) -PassThru
    $control = Wait-Window $process.Id 'LiveWallpaperEngine.Control'
    if ($control -eq [IntPtr]::Zero) { throw 'Main window was not created.' }
    [void][LweSettingsUiProbe]::ShowWindow($control, 5)
    [void][LweSettingsUiProbe]::SetForegroundWindow($control)
    Start-Sleep -Milliseconds 500
    $settingsButton = Wait-Window $process.Id 'LiveWallpaperEngine.SettingsButton' $true
    $settingsButtonText = [LweSettingsUiProbe]::Text($settingsButton)
    $expectedSettings = -join @([char]0x8BBE, [char]0x7F6E)
    if ($settingsButton -eq [IntPtr]::Zero -or $settingsButtonText -ne $expectedSettings) {
        throw 'The title-bar settings entry is missing or mislabeled.'
    }
    if ([LweSettingsUiProbe]::GetDlgItem($control, 1107) -ne [IntPtr]::Zero) {
        throw 'The old sidebar resource-release control is still present.'
    }
    $updateButton = Wait-Window $process.Id 'LiveWallpaperEngine.UpdateButton' $true
    [void][LweSettingsUiProbe]::PostMessage(
        $control, 0x0111, [LweSettingsUiProbe]::Command(2197, 0),
        [IntPtr]::Zero)

    Click-Window $settingsButton
    $dialog = Wait-Window $process.Id 'LiveWallpaperEngine.Settings' $true
    if ($dialog -eq [IntPtr]::Zero) { throw 'The settings window did not open.' }
    [void][LweSettingsUiProbe]::SetForegroundWindow($dialog)
    Start-Sleep -Milliseconds 200
    if (-not [LweSettingsUiProbe]::IsWindowVisible($updateButton) -or
        -not [LweSettingsUiProbe]::IsWindowVisible($settingsButton)) {
        throw 'The title-bar update/settings entries disappeared while settings was open.'
    }
    $navigation = [LweSettingsUiProbe]::GetDlgItem($dialog, 3300)
    $option = [LweSettingsUiProbe]::GetDlgItem($dialog, 3301)
    $navigationText = [LweSettingsUiProbe]::Text($navigation)
    $optionText = [LweSettingsUiProbe]::Text($option)
    $expectedNavigation = -join @(
        [char]0x6027, [char]0x80FD, [char]0x4F18, [char]0x5316)
    $expectedOption = -join @(
        [char]0x9501, [char]0x5C4F, '/', [char]0x7184, [char]0x5C4F,
        [char]0x65F6, [char]0x91CA, [char]0x653E, [char]0x89C6,
        [char]0x9891, [char]0x8D44, [char]0x6E90)
    if ($navigationText -ne $expectedNavigation -or
        $optionText -ne $expectedOption) {
        throw 'The performance settings controls are missing or mislabeled.'
    }
    $optionBounds = New-Object LweSettingsUiProbe+RECT
    [void][LweSettingsUiProbe]::GetWindowRect($option, [ref]$optionBounds)

    $eraseResult = [LweSettingsUiProbe]::SendMessage(
        $option, 0x0014, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($eraseResult -eq [IntPtr]::Zero) {
        throw 'The settings option still delegates background erasing to the system button class.'
    }

    for ($pass = 0; $pass -lt 16; $pass++) {
        [void][LweSettingsUiProbe]::SetCursorPos(
            $optionBounds.Left + 260, $optionBounds.Top + 24)
        [void][LweSettingsUiProbe]::SendMessage(
            $option, 0x0200, [IntPtr]::Zero,
            [LweSettingsUiProbe]::Point(260, 24))
        Start-Sleep -Milliseconds 25
        [void][LweSettingsUiProbe]::SetCursorPos(
            $optionBounds.Left - 12, $optionBounds.Top - 12)
        [void][LweSettingsUiProbe]::SendMessage(
            $option, 0x02A3, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 25
        if ([LweSettingsUiProbe]::FindChild(
                $dialog, 'LiveWallpaperEngine.SettingsTooltip', $true) -ne
            [IntPtr]::Zero) {
            throw "A fast settings flyby flashed the tooltip at iteration $pass."
        }
    }
    [void][LweSettingsUiProbe]::SetCursorPos(
        $optionBounds.Left - 12, $optionBounds.Top - 12)
    Start-Sleep -Milliseconds 150
    [void][LweSettingsUiProbe]::SetCursorPos(
        $optionBounds.Left + 260, $optionBounds.Top + 24)
    [void][LweSettingsUiProbe]::SendMessage(
        $option, 0x0200, [IntPtr]::Zero,
        [LweSettingsUiProbe]::Point(260, 24))
    Start-Sleep -Milliseconds 500
    $tooltip = [LweSettingsUiProbe]::FindChild(
        $dialog, 'LiveWallpaperEngine.SettingsTooltip', $true)
    if ($tooltip -eq [IntPtr]::Zero) {
        $tooltipAny = [LweSettingsUiProbe]::FindChild(
            $dialog, 'LiveWallpaperEngine.SettingsTooltip', $false)
        Write-Output "SETTINGS_TOOLTIP_HANDLE=$tooltipAny"
        if ($tooltipAny -ne [IntPtr]::Zero) {
            $tooltipBounds = New-Object LweSettingsUiProbe+RECT
            [void][LweSettingsUiProbe]::GetWindowRect($tooltipAny, [ref]$tooltipBounds)
            $tooltipStyle = [LweSettingsUiProbe]::GetWindowLongPtr($tooltipAny, -16)
            Write-Output ("SETTINGS_TOOLTIP_RECT={0},{1},{2},{3}" -f
                $tooltipBounds.Left, $tooltipBounds.Top,
                $tooltipBounds.Right, $tooltipBounds.Bottom)
            Write-Output "SETTINGS_TOOLTIP_STYLE=$tooltipStyle"
        }
        throw 'The resource-release explanation tooltip did not appear.'
    }

    $tooltipBounds = New-Object LweSettingsUiProbe+RECT
    [void][LweSettingsUiProbe]::GetWindowRect($tooltip, [ref]$tooltipBounds)
    $tooltipBitmap = [Drawing.Bitmap]::new(
        $tooltipBounds.Right - $tooltipBounds.Left,
        $tooltipBounds.Bottom - $tooltipBounds.Top)
    $tooltipGraphics = [Drawing.Graphics]::FromImage($tooltipBitmap)
    $tooltipDc = $tooltipGraphics.GetHdc()
    [void][LweSettingsUiProbe]::PrintWindow($tooltip, $tooltipDc, 2)
    $tooltipGraphics.ReleaseHdc($tooltipDc)
    $tooltipBitmap.Save(
        $TooltipScreenshotPath, [Drawing.Imaging.ImageFormat]::Png)
    $tooltipGraphics.Dispose(); $tooltipBitmap.Dispose()

    $dialogBounds = New-Object LweSettingsUiProbe+RECT
    [void][LweSettingsUiProbe]::GetWindowRect($dialog, [ref]$dialogBounds)
    $bitmap = [Drawing.Bitmap]::new(
        $dialogBounds.Right - $dialogBounds.Left,
        $dialogBounds.Bottom - $dialogBounds.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    [void][LweSettingsUiProbe]::PrintWindow($dialog, $dc, 2)
    $graphics.ReleaseHdc($dc)
    $bitmap.Save($ScreenshotPath, [Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()

    [void][LweSettingsUiProbe]::SetCursorPos(
        $optionBounds.Left - 12, $optionBounds.Top - 12)
    Start-Sleep -Milliseconds 300
    if ([LweSettingsUiProbe]::FindChild(
            $dialog, 'LiveWallpaperEngine.SettingsTooltip', $true) -ne
        [IntPtr]::Zero) {
        throw 'The resource-release explanation tooltip did not close.'
    }

    [void][LweSettingsUiProbe]::SendMessage(
        $option, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][LweSettingsUiProbe]::SendMessage(
        [LweSettingsUiProbe]::GetDlgItem($dialog, 3302),
        0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    [void][LweSettingsUiProbe]::PostMessage(
        $control, 0x0111, [LweSettingsUiProbe]::Command(2197, 0),
        [IntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    Click-Window $settingsButton
    $dialog = Wait-Window $process.Id 'LiveWallpaperEngine.Settings' $true
    $option = [LweSettingsUiProbe]::GetDlgItem($dialog, 3301)
    [void][LweSettingsUiProbe]::SendMessage(
        $option, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][LweSettingsUiProbe]::SendMessage(
        [LweSettingsUiProbe]::GetDlgItem($dialog, 3302),
        0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300

    [void][LweSettingsUiProbe]::PostMessage(
        $control, 0x8008, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'The settings test process did not exit normally.'
    }
    $process = $null

    $logBytes = [IO.File]::ReadAllBytes($logPath)
    $segment = [Text.Encoding]::UTF8.GetString(
        $logBytes, [int]$baselineLogLength,
        $logBytes.Length - [int]$baselineLogLength)
    foreach ($required in @(
        'Deep-pause resource release setting disabled.',
        'Deep-pause resource release setting enabled.')) {
        if (-not $segment.Contains($required)) {
            throw "Settings change was not logged: $required"
        }
    }
    $paintMatches = [regex]::Matches(
        $segment, 'CONTROLLED_MAIN_FULL_PAINT_COUNT=(\d+)')
    if ($paintMatches.Count -lt 2) {
        throw 'The main full-paint counter did not produce two samples.'
    }
    $paintBefore = [uint64]$paintMatches[0].Groups[1].Value
    $paintAfter = [uint64]$paintMatches[$paintMatches.Count - 1].Groups[1].Value
    if ($paintAfter -ne $paintBefore) {
        throw "Opening and closing settings forced a full main-window repaint: $paintBefore -> $paintAfter"
    }
    'SETTINGS_TITLE_ENTRY=True'
    'SETTINGS_TITLE_ENTRIES_STAY_VISIBLE=True'
    'SETTINGS_PERFORMANCE_CATEGORY=True'
    'SETTINGS_FAST_FLYBY_STABLE=True'
    'SETTINGS_DARK_ERASE_BACKGROUND=True'
    'SETTINGS_NO_FULL_MAIN_REPAINT=True'
    'SETTINGS_RELEASE_RESOURCE_TOOLTIP=True'
    'SETTINGS_RELEASE_RESOURCE_TOGGLE=True'
    "SETTINGS_SCREENSHOT=$ScreenshotPath"
    "SETTINGS_TOOLTIP_SCREENSHOT=$TooltipScreenshotPath"
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
