[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string] $ScreenshotPath = (Join-Path $env:TEMP 'LWE-display-selection.png'),
    [string] $MainScreenshotPath = (Join-Path $env:TEMP 'LWE-display-mode-main.png')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweDisplayModeProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w, IntPtr after, int x, int y, int width, int height, uint flags);

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
        var text = new StringBuilder(512);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }

    public static IntPtr Command(int identifier, int notification)
    {
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

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = [IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}
if (@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count -ne 0) {
    throw 'Close the existing Live Wallpaper Engine process before this controlled test.'
}

$screens = @([System.Windows.Forms.Screen]::AllScreens |
    Sort-Object { $_.Bounds.X }, { $_.Bounds.Y })
if ($screens.Count -lt 2) {
    throw "At least two active displays are required; found $($screens.Count)."
}
$settingsDirectory = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$libraryDirectory = Join-Path $settingsDirectory 'library'
if (@(Get-ChildItem -LiteralPath $libraryDirectory -File -ErrorAction SilentlyContinue).Count -eq 0) {
    throw 'The local wallpaper library must contain at least one item.'
}
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySettingsSnapshot = Get-FileSnapshot $legacySettingsPath
$baselineWallpaper =
    (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
$process = $null

try {
    [IO.Directory]::CreateDirectory($settingsDirectory) | Out-Null
    $testSettings = [ordered]@{
        version = 4
        soundEnabled = $false
        selectedDisplayTargets = $screens[0].DeviceName
        spanSelection = $false
        assignments = @()
    }
    [IO.File]::WriteAllText(
        $settingsPath, ($testSettings | ConvertTo-Json -Depth 5),
        [Text.UTF8Encoding]::new($false))

    $process = Start-Process -FilePath $executable `
        -ArgumentList '--test-seconds=30' -PassThru
    $control = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweDisplayModeProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.Control')
        if ($control -ne [IntPtr]::Zero -or $process.HasExited) { break }
    }
    if ($control -eq [IntPtr]::Zero) {
        throw 'The main control window was not created.'
    }

    $displayMode = [LweDisplayModeProbe]::GetDlgItem($control, 1199)
    $filter = [LweDisplayModeProbe]::GetDlgItem($control, 1101)
    $expectedModeText = -join @(
        [char]0x663E, [char]0x793A, [char]0x65B9, [char]0x5F0F,
        [char]0xFF1A, [char]0x5206, [char]0x5C4F, [char]0x663E,
        [char]0x793A)
    $modeText = ''
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        $modeText = [LweDisplayModeProbe]::Text($displayMode)
        if ($modeText -eq $expectedModeText) { break }
        Start-Sleep -Milliseconds 100
    }
    $filterText = [LweDisplayModeProbe]::Text($filter)
    if ($modeText -ne $expectedModeText) {
        throw "Unexpected display mode text: $modeText"
    }
    $oldTriangleCharacter = [char]0x25BC
    if ($modeText.Contains($oldTriangleCharacter) -or
        $filterText.Contains($oldTriangleCharacter)) {
        throw 'A dropdown button still embeds the old triangle text character.'
    }

    # The control is created before initial wallpaper restoration completes;
    # wait for Run() to reach ShowControlWindow before capturing it.
    Start-Sleep -Seconds 1
    [void][LweDisplayModeProbe]::SetWindowPos(
        $control, [IntPtr](-1), 0, 0, 0, 0, 0x0043)
    Start-Sleep -Milliseconds 250
    $mainBounds = New-Object LweDisplayModeProbe+RECT
    [void][LweDisplayModeProbe]::GetWindowRect($control, [ref]$mainBounds)
    $mainBitmap = [System.Drawing.Bitmap]::new(
        $mainBounds.Right - $mainBounds.Left,
        $mainBounds.Bottom - $mainBounds.Top)
    $mainGraphics = [System.Drawing.Graphics]::FromImage($mainBitmap)
    $mainGraphics.CopyFromScreen(
        $mainBounds.Left, $mainBounds.Top, 0, 0, $mainBitmap.Size)
    $mainBitmap.Save(
        $MainScreenshotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $mainGraphics.Dispose()
    $mainBitmap.Dispose()
    [void][LweDisplayModeProbe]::SetWindowPos(
        $control, [IntPtr](-2), 0, 0, 0, 0, 0x0043)

    $library = [LweDisplayModeProbe]::GetDlgItem($control, 1102)
    [void][LweDisplayModeProbe]::SendMessage(
        $library, 0x0186, [IntPtr]::Zero, [IntPtr]::Zero)
    if ([LweDisplayModeProbe]::GetDlgItem($control, 1105) -ne [IntPtr]::Zero) {
        throw 'The removed Apply button is still present.'
    }
    # Applying now uses the wallpaper row's double-click action instead of a
    # separate Apply button. Exercise the same LBN_DBLCLK command emitted by
    # the library control so this test follows the current user workflow.
    [void][LweDisplayModeProbe]::PostMessage(
        $control, 0x0111, [LweDisplayModeProbe]::Command(1102, 2), $library)

    $dialog = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        Start-Sleep -Milliseconds 100
        $dialog = [LweDisplayModeProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.ScreenSelection')
        if ($dialog -ne [IntPtr]::Zero -or $process.HasExited) { break }
    }
    if ($dialog -eq [IntPtr]::Zero) {
        throw 'Split mode did not show the screen-selection dialog when applying.'
    }
    $screenList = [LweDisplayModeProbe]::GetDlgItem($dialog, 3100)
    $screenCount = [int][LweDisplayModeProbe]::SendMessage(
        $screenList, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($screenCount -ne $screens.Count) {
        throw "The selector showed $screenCount screens; expected $($screens.Count)."
    }

    # Temporarily keep the owned dialog above unrelated foreground windows so
    # the screenshot records the actual user-visible prompt, then remove the
    # topmost flag immediately after capture.
    [void][LweDisplayModeProbe]::SetWindowPos(
        $dialog, [IntPtr](-1), 0, 0, 0, 0, 0x0043)
    Start-Sleep -Milliseconds 250
    $dialogBounds = New-Object LweDisplayModeProbe+RECT
    [void][LweDisplayModeProbe]::GetWindowRect($dialog, [ref]$dialogBounds)
    $bitmap = [System.Drawing.Bitmap]::new(
        $dialogBounds.Right - $dialogBounds.Left,
        $dialogBounds.Bottom - $dialogBounds.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen(
        $dialogBounds.Left, $dialogBounds.Top, 0, 0, $bitmap.Size)
    $bitmap.Save($ScreenshotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    [void][LweDisplayModeProbe]::SetWindowPos(
        $dialog, [IntPtr](-2), 0, 0, 0, 0, 0x0043)

    for ($index = 0; $index -lt $screenCount; $index++) {
        [void][LweDisplayModeProbe]::SendMessage(
            $screenList, 0x0185, [IntPtr]1, [IntPtr]$index)
    }
    $selectedCount = [int][LweDisplayModeProbe]::SendMessage(
        $screenList, 0x0190, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($selectedCount -ne $screenCount) {
        throw "Multi-select selected $selectedCount of $screenCount screens."
    }
    $dialogApply = [LweDisplayModeProbe]::GetDlgItem($dialog, 3101)
    [void][LweDisplayModeProbe]::PostMessage(
        $dialog, 0x0111, [LweDisplayModeProbe]::Command(3101, 0), $dialogApply)
    Start-Sleep -Seconds 2
    $process.Refresh()
    if ($process.HasExited) {
        throw "Applying to multiple screens exited early with code $($process.ExitCode)."
    }

    $saved = Get-Content -Raw -Encoding UTF8 $settingsPath | ConvertFrom-Json
    if (@($saved.assignments).Count -ne 1 -or
        $saved.assignments[0].spanAcrossDisplays -ne $false) {
        throw 'The selected wallpaper was not saved as one split assignment.'
    }
    $savedTargets = @($saved.assignments[0].displayTargets -split '\|')
    if ($savedTargets.Count -ne $screens.Count) {
        throw "The split assignment saved $($savedTargets.Count) targets; expected $($screens.Count)."
    }

    [void][LweDisplayModeProbe]::PostMessage(
        $control, 0x0111, [LweDisplayModeProbe]::Command(2199, 0),
        [IntPtr]::Zero)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'The display-mode test process did not exit normally.'
    }
    $process = $null
    if ((Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' `
            -Name WallPaper).WallPaper -ne $baselineWallpaper) {
        throw 'The display-mode test changed the Windows system wallpaper.'
    }

    "MODE_TEXT=$modeText"
    "FILTER_TEXT=$filterText"
    'TEXT_TRIANGLE_PRESENT=False'
    "SCREEN_OPTION_COUNT=$screenCount"
    "MULTI_SELECTED_COUNT=$selectedCount"
    "SAVED_TARGET_COUNT=$($savedTargets.Count)"
    'EARLY_EXIT=False'
    'NORMAL_EXIT=True'
    'SYSTEM_WALLPAPER_UNCHANGED=True'
    "MAIN_SCREENSHOT=$MainScreenshotPath"
    "SCREENSHOT=$ScreenshotPath"
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySettingsSnapshot
}

"SETTINGS_RESTORED=$((Get-FileSnapshot $settingsPath) -eq $settingsSnapshot)"
"LEGACY_SETTINGS_RESTORED=$((Get-FileSnapshot $legacySettingsPath) -eq $legacySettingsSnapshot)"
"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
