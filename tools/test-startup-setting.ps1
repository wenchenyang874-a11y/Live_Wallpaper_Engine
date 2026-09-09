[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string] $ExecutablePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class LweStartupProbe {
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(
        EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(
        IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(
        IntPtr window, StringBuilder name, int capacity);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(
        IntPtr window, int identifier);
    [DllImport("user32.dll")] public static extern bool PostMessage(
        IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(
        IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    public static IntPtr Find(uint processId, string targetClass) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            var className = new StringBuilder(128);
            GetClassName(window, className, className.Capacity);
            if (owner == processId && className.ToString() == targetClass) {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

function Get-FileSnapshot([string] $Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path))
    }
    return $null
}

function Restore-FileSnapshot(
    [string] $Path,
    [AllowNull()][string] $Snapshot
) {
    if ($null -eq $Snapshot) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            Remove-Item -LiteralPath $Path -Force
        }
        return
    }
    $directory = Split-Path -Parent $Path
    [void](New-Item -ItemType Directory -Path $directory -Force)
    [IO.File]::WriteAllBytes($Path, [Convert]::FromBase64String($Snapshot))
}

function Wait-Window(
    [int] $ProcessId,
    [string] $ClassName,
    [int] $Attempts = 100
) {
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        Start-Sleep -Milliseconds 100
        $window = [LweStartupProbe]::Find([uint32]$ProcessId, $ClassName)
        if ($window -ne [IntPtr]::Zero) {
            return $window
        }
    }
    return [IntPtr]::Zero
}

function Stop-ControlledApplication(
    [Diagnostics.Process] $Process,
    [IntPtr] $ControlWindow
) {
    if ($Process.HasExited) {
        if ($Process.ExitCode -ne 0) {
            throw "Live Wallpaper Engine exited with code $($Process.ExitCode)."
        }
        return
    }
    [void][LweStartupProbe]::PostMessage(
        $ControlWindow, 0x8008, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $Process.WaitForExit(10000)) {
        throw 'Live Wallpaper Engine did not exit after the controlled shutdown request.'
    }
    if ($Process.ExitCode -ne 0) {
        throw "Live Wallpaper Engine exited with code $($Process.ExitCode)."
    }
}

function Open-Settings([Diagnostics.Process] $Process, [IntPtr] $ControlWindow) {
    [void][LweStartupProbe]::PostMessage(
        $ControlWindow, 0x800A, [IntPtr]::Zero, [IntPtr]::Zero)
    $dialog = Wait-Window $Process.Id 'LiveWallpaperEngine.Settings'
    if ($dialog -eq [IntPtr]::Zero) {
        throw 'The settings window did not open.'
    }
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        if ([LweStartupProbe]::IsWindowVisible($dialog)) {
            return $dialog
        }
        Start-Sleep -Milliseconds 50
    }
    throw 'The settings window was created but did not become visible.'
}

function Save-StartupSetting([IntPtr] $Dialog) {
    $startup = [LweStartupProbe]::GetDlgItem($Dialog, 3306)
    $save = [LweStartupProbe]::GetDlgItem($Dialog, 3302)
    if ($startup -eq [IntPtr]::Zero -or $save -eq [IntPtr]::Zero) {
        throw 'The startup setting or Save button is missing.'
    }
    [void][LweStartupProbe]::SendMessage(
        $startup, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][LweStartupProbe]::SendMessage(
        $save, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Get-StartupValue {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runKeyPath)
    if ($null -eq $key) {
        return $null
    }
    try {
        return $key.GetValue(
            $runValueName, $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    }
    finally {
        $key.Dispose()
    }
}

function Wait-StartupValue([AllowNull()][string] $ExpectedValue) {
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $actual = Get-StartupValue
        if (($null -eq $ExpectedValue -and $null -eq $actual) -or
            ($null -ne $ExpectedValue -and $actual -ceq $ExpectedValue)) {
            return $actual
        }
        Start-Sleep -Milliseconds 50
    }
    return Get-StartupValue
}

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
    $ExecutablePath = Join-Path $root `
        "out\x64\$Configuration\LiveWallpaperEngine.exe"
}
$ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
$runKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
$runValueName = 'LiveWallpaperEngine'
$runKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
    $runKeyPath, $true)
$runValueExisted = $false
$runValue = $null
$runValueKind = $null
if ($null -ne $runKey) {
    $runValueExisted = $runKey.GetValueNames() -contains $runValueName
    if ($runValueExisted) {
        $runValue = $runKey.GetValue(
            $runValueName, $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $runValueKind = $runKey.GetValueKind($runValueName)
    }
}

$settingsDirectory = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine'
$settingsPath = Join-Path $settingsDirectory 'settings.json'
$legacySettingsPath = Join-Path $settingsDirectory 'settings.v1.json'
$settingsSnapshot = Get-FileSnapshot $settingsPath
$legacySnapshot = Get-FileSnapshot $legacySettingsPath
$startedProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
$startupCommand = '"{0}" --startup' -f $ExecutablePath
$enabled = $false
$quoted = $false
$hidden = $false
$disabled = $false
$registryRestored = $false
$settingsRestored = $false
$residualCount = 0

try {
    if ($null -eq $runKey) {
        $runKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runKeyPath)
    }
    $runKey.DeleteValue($runValueName, $false)

    $process = Start-Process -FilePath $ExecutablePath `
        -ArgumentList '--test-seconds=30' -PassThru
    $startedProcesses.Add($process)
    $control = Wait-Window $process.Id 'LiveWallpaperEngine.Control'
    if ($control -eq [IntPtr]::Zero) {
        throw 'The main window was not created for the enable test.'
    }
    $dialog = Open-Settings $process $control
    $general = [LweStartupProbe]::GetDlgItem($dialog, 3305)
    $performance = [LweStartupProbe]::GetDlgItem($dialog, 3300)
    $release = [LweStartupProbe]::GetDlgItem($dialog, 3301)
    if ($general -eq [IntPtr]::Zero -or $performance -eq [IntPtr]::Zero -or
        -not [LweStartupProbe]::IsWindowVisible(
            [LweStartupProbe]::GetDlgItem($dialog, 3306)) -or
        [LweStartupProbe]::IsWindowVisible($release)) {
        throw 'The General settings category was not the visible default.'
    }
    Save-StartupSetting $dialog
    $writtenCommand = Wait-StartupValue $startupCommand
    $enabled = $null -ne $writtenCommand
    $quoted = $writtenCommand -ceq $startupCommand
    if (-not $enabled -or -not $quoted) {
        throw "Unexpected startup command: '$writtenCommand'."
    }
    Stop-ControlledApplication $process $control

    $process = Start-Process -FilePath $ExecutablePath `
        -ArgumentList @('--startup', '--test-seconds=3') -PassThru
    $startedProcesses.Add($process)
    $control = Wait-Window $process.Id 'LiveWallpaperEngine.Control'
    if ($control -eq [IntPtr]::Zero) {
        throw 'The startup-mode process did not create its control window.'
    }
    Start-Sleep -Milliseconds 300
    $hidden = -not [LweStartupProbe]::IsWindowVisible($control)
    if (-not $hidden) {
        throw 'Startup mode displayed the management window instead of hiding it.'
    }
    if (-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0) {
        throw 'The startup-mode process did not exit cleanly after its test duration.'
    }

    $process = Start-Process -FilePath $ExecutablePath `
        -ArgumentList '--test-seconds=30' -PassThru
    $startedProcesses.Add($process)
    $control = Wait-Window $process.Id 'LiveWallpaperEngine.Control'
    if ($control -eq [IntPtr]::Zero) {
        throw 'The main window was not created for the disable test.'
    }
    $dialog = Open-Settings $process $control
    Save-StartupSetting $dialog
    $disabled = $null -eq (Wait-StartupValue $null)
    if (-not $disabled) {
        throw 'Disabling the setting did not remove the startup value.'
    }
    Stop-ControlledApplication $process $control
}
finally {
    foreach ($started in $startedProcesses) {
        if (-not $started.HasExited) {
            $started.Kill()
            [void]$started.WaitForExit(5000)
        }
        if (-not $started.HasExited) {
            $residualCount++
        }
        $started.Dispose()
    }
    if ($null -eq $runKey) {
        $runKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runKeyPath)
    }
    if ($runValueExisted) {
        $runKey.SetValue($runValueName, $runValue, $runValueKind)
    } else {
        $runKey.DeleteValue($runValueName, $false)
    }
    $registryRestored = if ($runValueExisted) {
        ($runKey.GetValueNames() -contains $runValueName) -and
        ($runKey.GetValue($runValueName) -ceq $runValue)
    } else {
        -not ($runKey.GetValueNames() -contains $runValueName)
    }
    $runKey.Dispose()
    Restore-FileSnapshot $settingsPath $settingsSnapshot
    Restore-FileSnapshot $legacySettingsPath $legacySnapshot
    $settingsRestored =
        (Get-FileSnapshot $settingsPath) -ceq $settingsSnapshot -and
        (Get-FileSnapshot $legacySettingsPath) -ceq $legacySnapshot
}

"STARTUP_SETTING_ENABLED=$enabled"
"STARTUP_COMMAND_QUOTED=$quoted"
"STARTUP_LAUNCH_HIDDEN=$hidden"
"STARTUP_SETTING_DISABLED=$disabled"
"STARTUP_REGISTRY_RESTORED=$registryRestored"
"SETTINGS_RESTORED=$settingsRestored"
"RESIDUAL_COUNT=$residualCount"
