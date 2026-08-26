[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string] $ScreenshotPath =
        (Join-Path $env:TEMP 'LWE-wallpaper-groups.png')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweGroupUiProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SetWindowText(IntPtr w, string text);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, ref RECT b);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr w, out RECT r);
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
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return [pscustomobject]@{
        Data = [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
        Attributes = [IO.File]::GetAttributes($Path)
    }
}

function Restore-FileSnapshot([string] $Path, [AllowNull()][object] $Snapshot) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        [IO.File]::SetAttributes($Path, [IO.FileAttributes]::Normal)
        Remove-Item -LiteralPath $Path -Force
    }
    if ($null -eq $Snapshot) { return }
    [IO.File]::WriteAllBytes(
        $Path, [Convert]::FromBase64String([string]$Snapshot.Data))
    [IO.File]::SetAttributes($Path, [IO.FileAttributes]$Snapshot.Attributes)
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
$libraryDirectory = Join-Path $settingsDirectory 'library'
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$groupsPath = Join-Path $libraryDirectory '.wallpaper-groups.v1'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySettingsSnapshot = Get-FileSnapshot $legacySettingsPath
$groupsSnapshot = Get-FileSnapshot $groupsPath
$process = $null

try {
    [IO.Directory]::CreateDirectory($settingsDirectory) | Out-Null
    [IO.File]::WriteAllText(
        $settingsPath,
        ([ordered]@{
            version = 4
            soundEnabled = $false
            selectedDisplayTargets = ''
            spanSelection = $true
            assignments = @()
        } | ConvertTo-Json -Depth 5),
        [Text.UTF8Encoding]::new($false))

    $process = Start-Process -FilePath $executable `
        -ArgumentList '--test-seconds=45' -PassThru
    $control = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweGroupUiProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.Control')
        if ($control -ne [IntPtr]::Zero -or $process.HasExited) { break }
    }
    if ($control -eq [IntPtr]::Zero) { throw 'The main window was not created.' }
    [void][LweGroupUiProbe]::ShowWindow($control, 5)

    $all = [LweGroupUiProbe]::GetDlgItem($control, 1120)
    $favorites = [LweGroupUiProbe]::GetDlgItem($control, 1121)
    $groups = [LweGroupUiProbe]::GetDlgItem($control, 1122)
    $create = [LweGroupUiProbe]::GetDlgItem($control, 1123)
    $groupRename = [LweGroupUiProbe]::GetDlgItem($control, 1126)
    $library = [LweGroupUiProbe]::GetDlgItem($control, 1102)
    $export = [LweGroupUiProbe]::GetDlgItem($control, 1104)
    $batch = [LweGroupUiProbe]::GetDlgItem($control, 1117)
    foreach ($required in @($all, $favorites, $groups, $create, $groupRename,
                             $library, $export, $batch)) {
        if ($required -eq [IntPtr]::Zero) { throw 'A group UI control is missing.' }
    }
    $allLabel = -join [char[]](0x5168, 0x90E8, 0x58C1, 0x7EB8)
    $favoritesLabel = -join [char[]](0x6700, 0x7231, 0x58C1, 0x7EB8)
    if ([LweGroupUiProbe]::Text($all) -ne $allLabel -or
        [LweGroupUiProbe]::Text($favorites) -ne $favoritesLabel) {
        throw 'The fixed wallpaper groups are missing or mislabeled.'
    }
    $allCount = [int][LweGroupUiProbe]::SendMessage(
        $library, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($allCount -lt 1) { throw 'The local library must contain at least one item.' }

    $initialGroupCount = [int][LweGroupUiProbe]::SendMessage(
        $groups, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][LweGroupUiProbe]::SendMessage(
        $control, 0x0111, [LweGroupUiProbe]::Command(1123, 0), $create)
    if (-not [LweGroupUiProbe]::IsWindowVisible($groupRename)) {
        throw 'A new group did not enter inline rename mode.'
    }
    $automationGroupLabel = -join [char[]](0x81EA, 0x52A8, 0x5316, 0x5206, 0x7EC4)
    [void][LweGroupUiProbe]::SetWindowText($groupRename, $automationGroupLabel)
    [void][LweGroupUiProbe]::SendMessage(
        $control, 0x0111, [LweGroupUiProbe]::Command(1126, 0), $groupRename)
    $nextGroupCount = [int][LweGroupUiProbe]::SendMessage(
        $groups, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($nextGroupCount -ne $initialGroupCount + 1) {
        throw 'Creating a custom group did not append it to the sidebar.'
    }
    $emptyGroupCount = [int][LweGroupUiProbe]::SendMessage(
        $library, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($emptyGroupCount -ne 0) {
        throw 'A new empty group did not scope the wallpaper library.'
    }

    [void][LweGroupUiProbe]::SendMessage(
        $control, 0x0111, [LweGroupUiProbe]::Command(1120, 0), $all)
    $restoredAllCount = [int][LweGroupUiProbe]::SendMessage(
        $library, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($restoredAllCount -ne $allCount) {
        throw 'Switching back to all wallpapers did not restore the full library.'
    }

    [void][LweGroupUiProbe]::SendMessage(
        $control, 0x0111, [LweGroupUiProbe]::Command(1104, 0), $export)
    $first = New-Object LweGroupUiProbe+RECT
    [void][LweGroupUiProbe]::SendMessage(
        $library, 0x0198, [IntPtr]::Zero, [ref]$first)
    $x = ($first.Left + $first.Right) / 2
    $y = ($first.Top + $first.Bottom) / 2
    [void][LweGroupUiProbe]::SendMessage(
        $library, 0x0201, [IntPtr]1, [LweGroupUiProbe]::Point($x, $y))
    [void][LweGroupUiProbe]::SendMessage(
        $library, 0x0202, [IntPtr]::Zero, [LweGroupUiProbe]::Point($x, $y))
    $batchOneLabel = -join [char[]](
        0x6279, 0x91CF, 0x64CD, 0x4F5C, 0xFF08, 0x31, 0xFF09)
    if ([LweGroupUiProbe]::Text($batch) -ne $batchOneLabel) {
        throw 'Clicking a wallpaper row did not enter generic multi-selection.'
    }

    $bounds = New-Object LweGroupUiProbe+RECT
    [void][LweGroupUiProbe]::GetWindowRect($control, [ref]$bounds)
    $bitmap = [Drawing.Bitmap]::new(
        $bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [LweGroupUiProbe]::PrintWindow($control, $dc, 2)) {
            throw 'PrintWindow failed for group UI screenshot.'
        }
    } finally {
        $graphics.ReleaseHdc($dc)
    }
    $bitmap.Save($ScreenshotPath, [Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()

    Write-Output 'WALLPAPER_GROUP_FIXED_ENTRIES=True'
    Write-Output 'WALLPAPER_GROUP_CREATE_RENAME=True'
    Write-Output 'WALLPAPER_GROUP_SCOPED_LIBRARY=True'
    Write-Output 'WALLPAPER_GROUP_GENERIC_MULTISELECT=True'
    Write-Output "WALLPAPER_GROUP_SCREENSHOT=$ScreenshotPath"
}
finally {
    if ($null -ne $process -and -not $process.HasExited) {
        $control = [LweGroupUiProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.Control')
        if ($control -ne [IntPtr]::Zero) {
            [void][LweGroupUiProbe]::PostMessage(
                $control, 0x0111,
                [LweGroupUiProbe]::Command(2199, 0), [IntPtr]::Zero)
        }
        if (-not $process.WaitForExit(5000)) {
            $process.Kill()
            $process.WaitForExit()
        }
    }
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySettingsSnapshot
    Restore-FileSnapshot $groupsPath $groupsSnapshot
}
