[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string] $VideoPath,
    [string] $DropdownScreenshot = (Join-Path $env:TEMP 'LWE-modern-dropdown.png'),
    [string] $ActiveDrawerScreenshot = (Join-Path $env:TEMP 'LWE-active-drawer.png')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweUiPlaybackProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr w, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
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

    public static IntPtr Command(int identifier, int notification)
    {
        return (IntPtr)((notification << 16) | (identifier & 0xffff));
    }

    public static IntPtr Point(int x, int y)
    {
        return (IntPtr)((y << 16) | (x & 0xffff));
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
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    try {
        $memory = [IO.MemoryStream]::new()
        $stream.CopyTo($memory)
        return $memory.ToArray()
    } finally {
        $stream.Dispose()
    }
}

function Wait-Window([int] $ProcessId, [string] $ClassName, [int] $Attempts = 80) {
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        $window = [LweUiPlaybackProbe]::Find([uint32]$ProcessId, $ClassName)
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 100
    }
    return [IntPtr]::Zero
}

function Save-WindowScreenshot([IntPtr] $Window, [string] $Path) {
    [void][LweUiPlaybackProbe]::SetWindowPos(
        $Window, [IntPtr](-1), 0, 0, 0, 0, 0x0043)
    Start-Sleep -Milliseconds 200
    $bounds = New-Object LweUiPlaybackProbe+RECT
    [void][LweUiPlaybackProbe]::GetWindowRect($Window, [ref]$bounds)
    $bitmap = [System.Drawing.Bitmap]::new(
        $bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $deviceContext = $graphics.GetHdc()
    try {
        if (-not [LweUiPlaybackProbe]::PrintWindow($Window, $deviceContext, 2)) {
            throw 'PrintWindow failed while capturing the UI.'
        }
    } finally {
        $graphics.ReleaseHdc($deviceContext)
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    [void][LweUiPlaybackProbe]::SetWindowPos(
        $Window, [IntPtr](-2), 0, 0, 0, 0, 0x0043)
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = [IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\x64\$Configuration\LiveWallpaperEngine.exe"))
$resolvedVideo = (Resolve-Path -LiteralPath $VideoPath).Path
foreach ($required in @($executable, $resolvedVideo)) {
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
    $arguments = @('--test-seconds=30', ('--test-wallpaper="{0}"' -f $resolvedVideo))
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
    $control = Wait-Window $process.Id 'LiveWallpaperEngine.Control'
    if ($control -eq [IntPtr]::Zero) { throw 'The main window was not created.' }
    Start-Sleep -Seconds 2
    [void][LweUiPlaybackProbe]::ShowWindow($control, 5)
    [void][LweUiPlaybackProbe]::SetForegroundWindow($control)
    Start-Sleep -Milliseconds 200

    $displayMode = [LweUiPlaybackProbe]::GetDlgItem($control, 1199)
    $dropdown = [LweUiPlaybackProbe]::GetDlgItem($control, 1109)
    $library = [LweUiPlaybackProbe]::GetDlgItem($control, 1102)
    $applyButtonRemoved =
        [LweUiPlaybackProbe]::GetDlgItem($control, 1105) -eq [IntPtr]::Zero
    $globalCancelButtonRemoved =
        [LweUiPlaybackProbe]::GetDlgItem($control, 1107) -eq [IntPtr]::Zero
    $activeDrawerHeaderRemoved =
        [LweUiPlaybackProbe]::GetDlgItem($control, 1112) -eq [IntPtr]::Zero
    if (-not $applyButtonRemoved -or -not $globalCancelButtonRemoved -or
        -not $activeDrawerHeaderRemoved) {
        throw 'A removed legacy action/header control is still present.'
    }
    [void][LweUiPlaybackProbe]::SendMessage(
        $displayMode, 0x0201, [IntPtr]1, [LweUiPlaybackProbe]::Point(100, 20))
    [void][LweUiPlaybackProbe]::SendMessage(
        $displayMode, 0x0202, [IntPtr]::Zero, [LweUiPlaybackProbe]::Point(100, 20))
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if ([LweUiPlaybackProbe]::IsWindowVisible($dropdown)) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not [LweUiPlaybackProbe]::IsWindowVisible($dropdown)) {
        $process.Refresh()
        throw "The in-app display-mode dropdown did not open; control=$control, display=$displayMode, dropdown=$dropdown, exited=$($process.HasExited)."
    }

    # Ask the UI thread for the render-thread counter on both sides of a long
    # open-menu interval. The dropdown remains open and no UI timer is relied
    # upon; a growing value proves frame submission is independently scheduled.
    [void][LweUiPlaybackProbe]::PostMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(2196, 0), [IntPtr]::Zero)
    Start-Sleep -Seconds 3
    [void][LweUiPlaybackProbe]::PostMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(2196, 0), [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    [void][LweUiPlaybackProbe]::ShowWindow($library, 0)
    Save-WindowScreenshot $control $DropdownScreenshot
    [void][LweUiPlaybackProbe]::ShowWindow($library, 5)

    $logBytes = Read-SharedBytes $logPath
    $logSegment = [Text.Encoding]::UTF8.GetString(
        $logBytes, [int]$baselineLogBytes,
        $logBytes.Length - [int]$baselineLogBytes)
    $frameMatches = [regex]::Matches(
        $logSegment, 'CONTROLLED_VIDEO_TRANSFER_COUNT=(\d+)')
    if ($frameMatches.Count -lt 2) {
        throw 'The controlled frame counter did not produce two samples.'
    }
    $before = [uint64]$frameMatches[$frameMatches.Count - 2].Groups[1].Value
    $after = [uint64]$frameMatches[$frameMatches.Count - 1].Groups[1].Value
    if ($after -le $before) {
        throw "Video frames stopped while the dropdown was open: $before -> $after"
    }
    if (-not $logSegment.Contains('Playback render thread started;')) {
        throw 'The independent playback render thread did not start.'
    }

    # A rapid second click is delivered by Win32 as BN_DBLCLK. It must close the
    # list, and the next regular click must open it again rather than being
    # swallowed. Close once more before exercising the classification list.
    [void][LweUiPlaybackProbe]::SendMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(1199, 5), $displayMode)
    if ([LweUiPlaybackProbe]::IsWindowVisible($dropdown)) {
        throw 'A rapid second display-mode click did not close the dropdown.'
    }
    [void][LweUiPlaybackProbe]::SendMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(1199, 0), $displayMode)
    if (-not [LweUiPlaybackProbe]::IsWindowVisible($dropdown)) {
        throw 'The display-mode dropdown did not reopen on the next click.'
    }
    [void][LweUiPlaybackProbe]::SendMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(1199, 5), $displayMode)
    if ([LweUiPlaybackProbe]::IsWindowVisible($dropdown)) {
        throw 'The reopened display-mode dropdown did not close rapidly.'
    }

    # Exercise the filter dropdown through the same down/up messages produced
    # by a real mouse click. Selecting its first row must close it without
    # relying on LBN_SELCHANGE timing.
    $filter = [LweUiPlaybackProbe]::GetDlgItem($control, 1101)
    [void][LweUiPlaybackProbe]::SendMessage(
        $filter, 0x0201, [IntPtr]1, [LweUiPlaybackProbe]::Point(100, 20))
    [void][LweUiPlaybackProbe]::SendMessage(
        $filter, 0x0202, [IntPtr]::Zero, [LweUiPlaybackProbe]::Point(100, 20))
    if (-not [LweUiPlaybackProbe]::IsWindowVisible($dropdown)) {
        throw 'The classification dropdown did not open through a mouse click.'
    }
    [void][LweUiPlaybackProbe]::SendMessage(
        $dropdown, 0x0201, [IntPtr]1, [LweUiPlaybackProbe]::Point(80, 20))
    [void][LweUiPlaybackProbe]::SendMessage(
        $dropdown, 0x0202, [IntPtr]::Zero, [LweUiPlaybackProbe]::Point(80, 20))
    if ([LweUiPlaybackProbe]::IsWindowVisible($dropdown)) {
        throw 'Selecting a classification row did not close the dropdown.'
    }

    # Once the hover transition has settled, moving inside the same wallpaper
    # row must not repaint it again. This catches the visible flash caused by
    # invalidating the whole list on every WM_MOUSEMOVE.
    $libraryBounds = New-Object LweUiPlaybackProbe+RECT
    [void][LweUiPlaybackProbe]::GetWindowRect($library, [ref]$libraryBounds)
    [void][LweUiPlaybackProbe]::SetCursorPos(
        $libraryBounds.Left + 120, $libraryBounds.Top + 38)
    [void][LweUiPlaybackProbe]::SendMessage(
        $library, 0x0200, [IntPtr]::Zero, [LweUiPlaybackProbe]::Point(120, 38))
    Start-Sleep -Milliseconds 500
    [void][LweUiPlaybackProbe]::SendMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(2195, 0), [IntPtr]::Zero)
    for ($offset = 0; $offset -lt 40; $offset++) {
        [void][LweUiPlaybackProbe]::SendMessage(
            $library, 0x0200, [IntPtr]::Zero,
            [LweUiPlaybackProbe]::Point(100 + $offset, 38))
    }
    [void][LweUiPlaybackProbe]::SendMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(2195, 0), [IntPtr]::Zero)
    $updatedLogBytes = Read-SharedBytes $logPath
    $updatedLogSegment = [Text.Encoding]::UTF8.GetString(
        $updatedLogBytes, [int]$baselineLogBytes,
        $updatedLogBytes.Length - [int]$baselineLogBytes)
    $drawMatches = [regex]::Matches(
        $updatedLogSegment, 'CONTROLLED_LIBRARY_DRAW_COUNT=(\d+)')
    if ($drawMatches.Count -lt 2) {
        throw 'The controlled library draw counter did not produce two samples.'
    }
    $drawBefore = [uint64]$drawMatches[$drawMatches.Count - 2].Groups[1].Value
    $drawAfter = [uint64]$drawMatches[$drawMatches.Count - 1].Groups[1].Value
    if ($drawAfter - $drawBefore -gt 1) {
        throw "Stationary-row mouse movement repainted the wallpaper list: $drawBefore -> $drawAfter"
    }

    # Locate the active library badge through the same hit-test path used by a
    # real click. Rejecting the confirmation must keep the wallpaper active.
    $libraryClient = New-Object LweUiPlaybackProbe+RECT
    [void][LweUiPlaybackProbe]::GetClientRect($library, [ref]$libraryClient)
    $libraryCount = [int][LweUiPlaybackProbe]::SendMessage(
        $library, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    $itemHeight = [int][LweUiPlaybackProbe]::SendMessage(
        $library, 0x01A1, [IntPtr]::Zero, [IntPtr]::Zero)
    $activeBadgeRow = -1
    for ($index = 0; $index -lt $libraryCount; $index++) {
        [void][LweUiPlaybackProbe]::PostMessage(
            $library, 0x0202, [IntPtr]::Zero,
            [LweUiPlaybackProbe]::Point(
                $libraryClient.Right - 48, $index * $itemHeight + $itemHeight / 2))
        $badgeConfirmation = Wait-Window $process.Id '#32770' 4
        if ($badgeConfirmation -ne [IntPtr]::Zero) {
            $activeBadgeRow = $index
            [void][LweUiPlaybackProbe]::PostMessage(
                $badgeConfirmation, 0x0111, [IntPtr]7, [IntPtr]::Zero)
            break
        }
    }
    if ($activeBadgeRow -lt 0) {
        throw 'Clicking the active library badge did not show a confirmation.'
    }
    Start-Sleep -Milliseconds 300

    $activeStatus = [LweUiPlaybackProbe]::GetDlgItem($control, 1110)
    $activeList = [LweUiPlaybackProbe]::GetDlgItem($control, 1111)
    # Rapid synchronous toggles must alternate deterministically. An even
    # number leaves the drawer closed before the final click below opens it.
    for ($toggle = 0; $toggle -lt 10; $toggle++) {
        [void][LweUiPlaybackProbe]::SendMessage(
            $control, 0x0111,
            [LweUiPlaybackProbe]::Command(
                1110, $(if (($toggle % 2) -eq 0) { 0 } else { 5 })),
            $activeStatus)
        $expectedVisible = ($toggle % 2) -eq 0
        if ([LweUiPlaybackProbe]::IsWindowVisible($activeList) -ne
            $expectedVisible) {
            throw "Rapid active-drawer toggle failed at iteration $toggle."
        }
    }
    [void][LweUiPlaybackProbe]::SendMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(1110, 0), $activeStatus)
    Start-Sleep -Milliseconds 300
    $activeCount = [int][LweUiPlaybackProbe]::SendMessage(
        $activeList, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not [LweUiPlaybackProbe]::IsWindowVisible($activeList) -or
        $activeCount -ne 1) {
        throw "The active drawer did not show its one wallpaper; count=$activeCount"
    }
    [void][LweUiPlaybackProbe]::PostMessage(
        $activeList, 0x0200, [IntPtr]::Zero,
        [LweUiPlaybackProbe]::Point(120, 38))
    [void][LweUiPlaybackProbe]::ShowWindow($library, 0)
    Save-WindowScreenshot $control $ActiveDrawerScreenshot
    [void][LweUiPlaybackProbe]::ShowWindow($library, 5)

    # Click the per-item cancel action and keep its confirmation open. Frames
    # must continue even while the modal confirmation owns the UI thread.
    $activeClient = New-Object LweUiPlaybackProbe+RECT
    [void][LweUiPlaybackProbe]::GetClientRect($activeList, [ref]$activeClient)
    [void][LweUiPlaybackProbe]::PostMessage(
        $activeList, 0x0202, [IntPtr]::Zero,
        [LweUiPlaybackProbe]::Point($activeClient.Right - 48, 38))
    $confirmation = Wait-Window $process.Id '#32770' 50
    if ($confirmation -eq [IntPtr]::Zero) {
        throw 'Per-wallpaper cancel did not show its confirmation.'
    }
    [void][LweUiPlaybackProbe]::PostMessage(
        $confirmation, 0x0111, [IntPtr]6, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    if ([int][LweUiPlaybackProbe]::SendMessage(
            $activeList, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero) -ne 0) {
        throw 'Confirmed per-wallpaper cancel left the wallpaper active.'
    }

    [void][LweUiPlaybackProbe]::PostMessage(
        $control, 0x0111, [LweUiPlaybackProbe]::Command(2199, 0), [IntPtr]::Zero)
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'The UI/playback test process did not exit normally.'
    }
    $process = $null

    "DROPDOWN_FRAME_COUNT_BEFORE=$before"
    "DROPDOWN_FRAME_COUNT_AFTER=$after"
    'DROPDOWN_PLAYBACK_CONTINUED=True'
    'CLASSIFICATION_DROPDOWN_MOUSE_CLICK=True'
    'INDEPENDENT_RENDER_THREAD=True'
    "LIBRARY_DRAW_COUNT_BEFORE_MOVES=$drawBefore"
    "LIBRARY_DRAW_COUNT_AFTER_MOVES=$drawAfter"
    'LIBRARY_MOUSE_MOVE_REPAINT_STABLE=True'
    'ACTIVE_DRAWER_RAPID_TOGGLE=True'
    "ACTIVE_DRAWER_COUNT=$activeCount"
    "APPLY_BUTTON_REMOVED=$applyButtonRemoved"
    "GLOBAL_CANCEL_BUTTON_REMOVED=$globalCancelButtonRemoved"
    "ACTIVE_DRAWER_HEADER_REMOVED=$activeDrawerHeaderRemoved"
    "ACTIVE_BADGE_ROW=$activeBadgeRow"
    'ACTIVE_BADGE_CONFIRMATION=True'
    'PER_ITEM_CANCEL_CONFIRMED=True'
    "DROPDOWN_SCREENSHOT=$DropdownScreenshot"
    "ACTIVE_DRAWER_SCREENSHOT=$ActiveDrawerScreenshot"
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySettingsSnapshot
}

if ((Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' `
        -Name WallPaper).WallPaper -ne $baselineWallpaper) {
    throw 'The UI/playback test changed the Windows system wallpaper.'
}
"SETTINGS_RESTORED=$((Get-FileSnapshot $settingsPath) -eq $settingsSnapshot)"
"LEGACY_SETTINGS_RESTORED=$((Get-FileSnapshot $legacySettingsPath) -eq $legacySettingsSnapshot)"
'SYSTEM_WALLPAPER_UNCHANGED=True'
"RESIDUAL_COUNT=$(@(Get-Process LiveWallpaperEngine -ErrorAction SilentlyContinue).Count)"
