[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Executable,
    [Parameter(Mandatory = $true)][string] $LibraryRoot,
    [Parameter(Mandatory = $true)][string] $Wallpaper,
    [ValidateRange(20, 1800)][int] $Seconds = 60,
    [switch] $AssertBounded
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LwePerformanceTest {
    public delegate bool Visitor(IntPtr w, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Visitor v, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder s, int c);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr w, uint m, IntPtr a, IntPtr b, uint f, uint t, out IntPtr r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern bool InvalidateRect(IntPtr w, IntPtr r, bool erase);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w, int command);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flag);
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((w,p) => {
            uint owner; GetWindowThreadProcessId(w, out owner);
            var name = new StringBuilder(128); GetClassName(w,name,128);
            if (owner == pid && name.ToString() == "LiveWallpaperEngine.Control") { found=w; return false; }
            return true;
        },IntPtr.Zero);
        return found;
    }
}
'@
$exe = (Resolve-Path -LiteralPath $Executable).Path
$library = (Resolve-Path -LiteralPath $LibraryRoot).Path
$media = (Resolve-Path -LiteralPath $Wallpaper).Path
if (@(Get-Process LiveWallpaperEngine,baseline -ErrorAction SilentlyContinue).Count) {
    throw 'Close existing wallpaper processes before this isolated controlled test.'
}
$settings = Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine/settings.json'
$beforeHash = if (Test-Path -LiteralPath $settings) { (Get-FileHash -LiteralPath $settings).Hash } else { '' }
$process = $null
$samples = [Collections.Generic.List[object]]::new()
try {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $exe -WindowStyle Hidden -PassThru -ArgumentList @(
        "--test-seconds=$($Seconds + 40)", ('--test-library-root="{0}"' -f $library),
        ('--test-wallpaper="{0}"' -f $media))
    $window = [IntPtr]::Zero
    do {
        Start-Sleep -Milliseconds 25
        if ($process.HasExited) { throw "Exited during startup: $($process.ExitCode)" }
        $window = [LwePerformanceTest]::Find([uint32]$process.Id)
        $ready = $false
        if ($window -ne [IntPtr]::Zero) {
            $list = [LwePerformanceTest]::GetDlgItem($window, 1102)
            $reply = [IntPtr]::Zero
            $ready = $list -ne [IntPtr]::Zero -and
                [LwePerformanceTest]::SendMessageTimeout($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero, 2, 100, [ref]$reply) -ne [IntPtr]::Zero -and
                $reply.ToInt64() -gt 0
        }
        if ($clock.Elapsed.TotalSeconds -gt 35) { throw 'Startup timed out.' }
    } until ($ready)
    # Start-Process Hidden suppresses the first ShowWindow call. Explicitly show
    # the controlled test window only after its list is populated and responsive.
    [void][LwePerformanceTest]::ShowWindow($window, 4)
    $readyMs = $clock.ElapsedMilliseconds
    $list = [LwePerformanceTest]::GetDlgItem($window, 1102)
    $count = [int][LwePerformanceTest]::SendMessage($list, 0x18B, [IntPtr]::Zero, [IntPtr]::Zero)
    $latencies = [Collections.Generic.List[long]]::new()
    for ($second = 0; $second -lt $Seconds; ++$second) {
        if ($second -lt ($Seconds * 3 / 4)) {
            $top = ($second * 7) % [Math]::Max(1, $count - 5)
            [void][LwePerformanceTest]::SendMessage($list, 0x197, [IntPtr]$top, [IntPtr]::Zero)
            $actualTop = [int][LwePerformanceTest]::SendMessage($list, 0x18E, [IntPtr]::Zero, [IntPtr]::Zero)
            if ($count -gt 12 -and [Math]::Abs($actualTop - $top) -gt 2) {
                throw "List did not scroll to the requested row: $top -> $actualTop"
            }
            [void][LwePerformanceTest]::InvalidateRect($list, [IntPtr]::Zero, $false)
        } elseif ($second -eq [int]($Seconds * 3 / 4)) {
            [void][LwePerformanceTest]::ShowWindow($window, 0)
        }
        $ping = [Diagnostics.Stopwatch]::StartNew()
        $reply = [IntPtr]::Zero
        if ([LwePerformanceTest]::SendMessageTimeout($window, 0, [IntPtr]::Zero, [IntPtr]::Zero, 2, 2000, [ref]$reply) -eq [IntPtr]::Zero) {
            throw 'Main window stopped responding for 2 seconds.'
        }
        $latencies.Add($ping.ElapsedMilliseconds)
        $process.Refresh()
        if ($process.HasExited) { throw 'Process exited unexpectedly.' }
        $samples.Add([pscustomobject]@{
            Second=$second; WorkingSetMB=[Math]::Round($process.WorkingSet64 / 1MB, 2)
            PrivateMB=[Math]::Round($process.PrivateMemorySize64 / 1MB, 2)
            Handles=$process.HandleCount
            Gdi=[LwePerformanceTest]::GetGuiResources($process.Handle,0)
            User=[LwePerformanceTest]::GetGuiResources($process.Handle,1)
            CpuSeconds=$process.TotalProcessorTime.TotalSeconds
        })
        Start-Sleep -Seconds 1
    }
    [void][LwePerformanceTest]::ShowWindow($window, 4)
    [void][LwePerformanceTest]::PostMessage($window, 0x111, [IntPtr]2199, [IntPtr]::Zero)
    if (-not $process.WaitForExit(15000) -or $process.ExitCode -ne 0) { throw 'Clean shutdown failed.' }
    $tailStart = $samples[[int]($Seconds * 3 / 4) + 2]
    $last = $samples[$samples.Count - 1]
    $privateGrowth = $last.PrivateMB - $tailStart.PrivateMB
    $gdiGrowth = $last.Gdi - $tailStart.Gdi
    $handleGrowth = $last.Handles - $tailStart.Handles
    $peakGdi = ($samples | Measure-Object Gdi -Maximum).Maximum
    $afterHash = if (Test-Path -LiteralPath $settings) { (Get-FileHash -LiteralPath $settings).Hash } else { '' }
    if ($beforeHash -ne $afterHash) { throw 'Controlled test changed user settings.' }
    [pscustomobject]@{
        Executable=$exe; ReadyMilliseconds=$readyMs; LibraryCount=$count
        MaximumPingMs=($latencies | Measure-Object -Maximum).Maximum
        Samples=$samples; CleanExit=$true; SettingsUnchanged=$true
        TailPrivateGrowthMB=$privateGrowth; TailGdiGrowth=$gdiGrowth; TailHandleGrowth=$handleGrowth
    } | ConvertTo-Json -Depth 4
    # Keep the measurements even when an assertion fails; diagnosis needs the
    # time series, not just the last threshold that was exceeded.
    if ($AssertBounded) {
        if ($privateGrowth -gt 16 -or $gdiGrowth -gt 5 -or $handleGrowth -gt 15) {
            throw "Resources grew after scrolling: private=$privateGrowth MB, GDI=$gdiGrowth, handles=$handleGrowth"
        }
        if ($peakGdi -gt ($samples[0].Gdi + 80)) {
            throw "Thumbnail GDI retention exceeded cache allowance: $($samples[0].Gdi) -> $peakGdi"
        }
    }
} finally {
    if ($process -and -not $process.HasExited) {
        $window = [LwePerformanceTest]::Find([uint32]$process.Id)
        if ($window -ne [IntPtr]::Zero) { [void][LwePerformanceTest]::PostMessage($window, 0x111, [IntPtr]2199, [IntPtr]::Zero) }
        if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id }
    }
}
