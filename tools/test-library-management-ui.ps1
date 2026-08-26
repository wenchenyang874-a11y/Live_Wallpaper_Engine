[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string] $ScreenshotPath =
        (Join-Path $env:TEMP 'LWE-library-management.png')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class LweLibraryManagementProbe
{
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc p, IntPtr x);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr w, StringBuilder n, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr w);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, ref RECT b);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr w);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w, int command);

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

    public static IntPtr Point(int x, int y)
    {
        return (IntPtr)((y << 16) | (x & 0xffff));
    }
}
'@

function Get-FileSnapshot([string] $Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [pscustomobject]@{
            Data = [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
            Attributes = [IO.File]::GetAttributes($Path)
        }
    }
    return $null
}

function Restore-FileSnapshot([string] $Path, [AllowNull()][object] $Snapshot) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        [IO.File]::SetAttributes($Path, [IO.FileAttributes]::Normal)
        Remove-Item -LiteralPath $Path -Force
    }
    if ($null -eq $Snapshot) {
        return
    }
    [IO.File]::WriteAllBytes(
        $Path, [Convert]::FromBase64String([string]$Snapshot.Data))
    [IO.File]::SetAttributes(
        $Path, [IO.FileAttributes]$Snapshot.Attributes)
}

function Assert-Visible([IntPtr] $Control, [bool] $Expected, [string] $Name) {
    if ($Control -eq [IntPtr]::Zero -or
        [LweLibraryManagementProbe]::IsWindowVisible($Control) -ne $Expected) {
        throw "Unexpected visibility for $Name."
    }
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
$orderPath = Join-Path $libraryDirectory '.library-order.v1'
$groupsPath = Join-Path $libraryDirectory '.wallpaper-groups.v1'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySettingsSnapshot = Get-FileSnapshot $legacySettingsPath
$orderSnapshot = Get-FileSnapshot $orderPath
$groupsSnapshot = Get-FileSnapshot $groupsPath
$baselineWallpaper =
    (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
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
        -ArgumentList '--test-seconds=30' -PassThru
    $control = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 100
        $control = [LweLibraryManagementProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.Control')
        if ($control -ne [IntPtr]::Zero -or $process.HasExited) { break }
    }
    if ($control -eq [IntPtr]::Zero) {
        throw 'The main control window was not created.'
    }
    [void][LweLibraryManagementProbe]::ShowWindow($control, 5)
    [void][LweLibraryManagementProbe]::SetForegroundWindow($control)
    Start-Sleep -Milliseconds 200

    $library = [LweLibraryManagementProbe]::GetDlgItem($control, 1102)
    $import = [LweLibraryManagementProbe]::GetDlgItem($control, 1103)
    $export = [LweLibraryManagementProbe]::GetDlgItem($control, 1104)
    $display = [LweLibraryManagementProbe]::GetDlgItem($control, 1199)
    $selectAll = [LweLibraryManagementProbe]::GetDlgItem($control, 1115)
    $clearAll = [LweLibraryManagementProbe]::GetDlgItem($control, 1116)
    $confirm = [LweLibraryManagementProbe]::GetDlgItem($control, 1117)
    $cancel = [LweLibraryManagementProbe]::GetDlgItem($control, 1118)
    $count = [int][LweLibraryManagementProbe]::SendMessage(
        $library, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($count -lt 2) {
        throw 'The local wallpaper library needs at least two items for this test.'
    }

    [void][LweLibraryManagementProbe]::SendMessage(
        $control, 0x0111,
        [LweLibraryManagementProbe]::Command(1104, 0), $export)
    Assert-Visible $import $false 'import button in export mode'
    Assert-Visible $export $false 'export button in export mode'
    Assert-Visible $display $false 'display button in export mode'
    Assert-Visible $selectAll $true 'select-all button'
    Assert-Visible $clearAll $true 'clear-all button'
    Assert-Visible $confirm $true 'confirm export button'
    Assert-Visible $cancel $true 'cancel export button'

    $exportPrefix = -join @(
        [char]0x6279, [char]0x91CF, [char]0x64CD, [char]0x4F5C,
        [char]0xFF08)
    $exportSuffix = [char]0xFF09
    $first = New-Object LweLibraryManagementProbe+RECT
    [void][LweLibraryManagementProbe]::SendMessage(
        $library, 0x0198, [IntPtr]::Zero, [ref]$first)
    $scale = [LweLibraryManagementProbe]::GetDpiForWindow($control) / 96.0
    $checkboxX = $first.Left + [int][Math]::Round(26 * $scale)
    $checkboxY = ($first.Top + $first.Bottom) / 2
    [void][LweLibraryManagementProbe]::SendMessage(
        $library, 0x0201, [IntPtr]1,
        [LweLibraryManagementProbe]::Point($checkboxX, $checkboxY))
    [void][LweLibraryManagementProbe]::SendMessage(
        $library, 0x0202, [IntPtr]::Zero,
        [LweLibraryManagementProbe]::Point($checkboxX, $checkboxY))
    if ([LweLibraryManagementProbe]::Text($confirm) -ne
            ($exportPrefix + '1' + $exportSuffix)) {
        throw 'Clicking a wallpaper checkbox did not select it for export.'
    }
    [void][LweLibraryManagementProbe]::SendMessage(
        $control, 0x0111,
        [LweLibraryManagementProbe]::Command(1116, 0), $clearAll)

    [void][LweLibraryManagementProbe]::SendMessage(
        $control, 0x0111,
        [LweLibraryManagementProbe]::Command(1115, 0), $selectAll)
    if ([LweLibraryManagementProbe]::Text($confirm) -ne
            ($exportPrefix + $count + $exportSuffix) -or
        -not [LweLibraryManagementProbe]::IsWindowEnabled($confirm)) {
        throw 'Select-all did not update the export selection count.'
    }
    [void][LweLibraryManagementProbe]::SendMessage(
        $control, 0x0111,
        [LweLibraryManagementProbe]::Command(1116, 0), $clearAll)
    if ([LweLibraryManagementProbe]::Text($confirm) -ne
            ($exportPrefix + '0' + $exportSuffix) -or
        [LweLibraryManagementProbe]::IsWindowEnabled($confirm)) {
        throw 'Clear-all did not clear and disable export confirmation.'
    }
    [void][LweLibraryManagementProbe]::SendMessage(
        $control, 0x0111,
        [LweLibraryManagementProbe]::Command(1118, 0), $cancel)
    Assert-Visible $import $true 'import button after export mode'

    # Move the first card below the second. The app persists the new order; the
    # exact original order file is restored in finally.
    $first = New-Object LweLibraryManagementProbe+RECT
    $second = New-Object LweLibraryManagementProbe+RECT
    $firstRectResult = [LweLibraryManagementProbe]::SendMessage(
        $library, 0x0198, [IntPtr]::Zero, [ref]$first)
    $secondRectResult = [LweLibraryManagementProbe]::SendMessage(
        $library, 0x0198, [IntPtr]1, [ref]$second)
    if ($firstRectResult -eq [IntPtr](-1) -or
        $secondRectResult -eq [IntPtr](-1) -or
        $first.Bottom -le $first.Top -or $second.Bottom -le $second.Top) {
        throw 'The test could not resolve the first two wallpaper card bounds.'
    }
    $startX = [Math]::Max(20, ($first.Left + $first.Right) / 2)
    $startY = ($first.Top + $first.Bottom) / 2
    $dropY = $second.Bottom - 4
    # Drive the list control through the same mouse messages Windows delivers.
    # This remains deterministic even when an automated desktop prevents the
    # test process from taking foreground ownership for global mouse_event.
    [void][LweLibraryManagementProbe]::SendMessage(
        $library, 0x0201, [IntPtr]1,
        [LweLibraryManagementProbe]::Point($startX, $startY))
    for ($step = 1; $step -le 5; $step++) {
        $nextY = $startY + [int](($dropY - $startY) * $step / 5)
        [void][LweLibraryManagementProbe]::SendMessage(
            $library, 0x0200, [IntPtr]1,
            [LweLibraryManagementProbe]::Point($startX, $nextY))
        Start-Sleep -Milliseconds 20
    }
    [void][LweLibraryManagementProbe]::SendMessage(
        $library, 0x0202, [IntPtr]::Zero,
        [LweLibraryManagementProbe]::Point($startX, $dropY))
    Start-Sleep -Milliseconds 250
    $nextOrderSnapshot = Get-FileSnapshot $orderPath
    if ($null -eq $nextOrderSnapshot -or
        ($null -ne $orderSnapshot -and
         $nextOrderSnapshot.Data -eq $orderSnapshot.Data)) {
        throw ("Drag reorder did not persist a changed library order. " +
               "first=$($first.Left),$($first.Top),$($first.Right),$($first.Bottom); " +
               "second=$($second.Left),$($second.Top),$($second.Right),$($second.Bottom)")
    }

    [void][LweLibraryManagementProbe]::PostMessage(
        $control, 0x0111,
        [LweLibraryManagementProbe]::Command(1103, 0), $import)
    $choice = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        Start-Sleep -Milliseconds 100
        $choice = [LweLibraryManagementProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.ImportChoice')
        if ($choice -ne [IntPtr]::Zero) { break }
    }
    if ($choice -eq [IntPtr]::Zero) {
        throw 'The import source choice window did not open.'
    }
    $mediaChoice = [LweLibraryManagementProbe]::GetDlgItem($choice, 3200)
    $packageChoice = [LweLibraryManagementProbe]::GetDlgItem($choice, 3201)
    $expectedMediaChoice = -join @(
        [char]0x5BFC, [char]0x5165, [char]0x56FE, [char]0x7247,
        ' / ', [char]0x89C6, [char]0x9891)
    $expectedPackageChoice = -join @(
        [char]0x5BFC, [char]0x5165, [char]0x5206, [char]0x4EAB,
        [char]0x5305)
    if ([LweLibraryManagementProbe]::Text($mediaChoice) -ne
            $expectedMediaChoice -or
        [LweLibraryManagementProbe]::Text($packageChoice) -ne
            $expectedPackageChoice) {
        throw 'The import source choices are missing or mislabeled.'
    }

    $bounds = New-Object LweLibraryManagementProbe+RECT
    [void][LweLibraryManagementProbe]::GetWindowRect($choice, [ref]$bounds)
    $bitmap = [System.Drawing.Bitmap]::new(
        $bounds.Right - $bounds.Left, $bounds.Bottom - $bounds.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
    $bitmap.Save($ScreenshotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    [void][LweLibraryManagementProbe]::SendMessage(
        $choice, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)

    Write-Output "LIBRARY_UI_EXPORT_SELECTION=True"
    Write-Output "LIBRARY_UI_DRAG_REORDER=True"
    Write-Output "LIBRARY_UI_IMPORT_CHOICE=True"
    Write-Output "LIBRARY_UI_SCREENSHOT=$ScreenshotPath"
}
finally {
    if ($null -ne $process -and -not $process.HasExited) {
        $control = [LweLibraryManagementProbe]::Find(
            [uint32]$process.Id, 'LiveWallpaperEngine.Control')
        if ($control -ne [IntPtr]::Zero) {
            [void][LweLibraryManagementProbe]::PostMessage(
                $control, 0x0111,
                [LweLibraryManagementProbe]::Command(2199, 0), [IntPtr]::Zero)
        }
        if (-not $process.WaitForExit(5000)) {
            $process.Kill()
            $process.WaitForExit()
        }
    }
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySettingsSnapshot
    Restore-FileSnapshot $orderPath $orderSnapshot
    Restore-FileSnapshot $groupsPath $groupsSnapshot
    $wallpaperAfter =
        (Get-ItemProperty -LiteralPath 'HKCU:\Control Panel\Desktop' -Name WallPaper).WallPaper
    if ($wallpaperAfter -ne $baselineWallpaper) {
        throw 'The controlled test changed the Windows wallpaper registry value.'
    }
}
