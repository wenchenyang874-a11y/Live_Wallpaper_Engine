[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [Parameter(Mandatory=$true)][string]$VideoPath, [string]$CompressionSource, [switch]$MessageOnly)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WorkflowProbe {
 public delegate bool Visitor(IntPtr w,IntPtr p);
 [StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}
 [DllImport("user32.dll")]public static extern bool EnumWindows(Visitor v,IntPtr p);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr w,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern IntPtr GetDlgItem(IntPtr w,int id);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr w,out RECT r);
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern void mouse_event(uint flags,int x,int y,uint data,UIntPtr extra);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr w);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 [DllImport("user32.dll")]public static extern IntPtr SendMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 [DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr w,IntPtr dc,uint flags);
 [DllImport("user32.dll")]public static extern bool IsWindowEnabled(IntPtr w);
 public static IntPtr Find(int pid,string name) {IntPtr result=IntPtr.Zero; EnumWindows((w,p)=>{uint id;GetWindowThreadProcessId(w,out id);var s=new StringBuilder(128);GetClassName(w,s,128);if(id==pid&&s.ToString()==name){result=w;return false;}return true;},IntPtr.Zero);return result;}
}
'@
function Wait-Window($Id,$Class) {
 for($i=0;$i -lt 80;$i++){ $w=[WorkflowProbe]::Find($Id,$Class);if($w -ne [IntPtr]::Zero){return $w};Start-Sleep -Milliseconds 100 }
 throw "Window missing: $Class"
}
function Click($Window,[int]$X=-1,[int]$Y=-1,[switch]$Right) {
 if($Window -eq [IntPtr]::Zero){throw 'Missing control'}
 $r=New-Object WorkflowProbe+RECT;[void][WorkflowProbe]::GetWindowRect($Window,[ref]$r)
 if($X -lt 0){$X=($r.R-$r.L)/2};if($Y -lt 0){$Y=($r.B-$r.T)/2}
 if($MessageOnly){
   $point=[IntPtr](($Y -shl 16) -bor $X)
   [void][WorkflowProbe]::PostMessage($Window,0x201,[IntPtr]1,$point)
   [void][WorkflowProbe]::PostMessage($Window,0x202,[IntPtr]::Zero,$point)
   Start-Sleep -Milliseconds 160;return
 }
 [void][WorkflowProbe]::SetCursorPos($r.L+$X,$r.T+$Y)
 $down=2;$up=4;if($Right){$down=8;$up=16}
 [WorkflowProbe]::mouse_event($down,0,0,0,[UIntPtr]::Zero);Start-Sleep -Milliseconds 35
 [WorkflowProbe]::mouse_event($up,0,0,0,[UIntPtr]::Zero);Start-Sleep -Milliseconds 160
}
function Capture($Window,$Path) {
 $r=New-Object WorkflowProbe+RECT;[void][WorkflowProbe]::GetWindowRect($Window,[ref]$r)
 $bmp=New-Object Drawing.Bitmap ($r.R-$r.L),($r.B-$r.T);$g=[Drawing.Graphics]::FromImage($bmp);$dc=$g.GetHdc()
 try{[void][WorkflowProbe]::PrintWindow($Window,$dc,2)}finally{$g.ReleaseHdc($dc)}
 $bmp.Save($Path);$g.Dispose();$bmp.Dispose()
}
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo ('out\functional-'+[Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $output)
$library=Join-Path $output 'library';[void](New-Item -ItemType Directory -Path $library)
$video=Join-Path $library 'test-video.mp4';Copy-Item -LiteralPath $VideoPath -Destination $video
$settings=Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine\settings.json'
$snapshot=if(Test-Path -LiteralPath $settings){[IO.File]::ReadAllBytes($settings)}else{$null}
$log=Join-Path $env:LOCALAPPDATA 'LiveWallpaperEngine\logs\LiveWallpaperEngine.log'
$baseline=(Get-Item -LiteralPath $log).Length
$process=$null
try {
 $process=Start-Process -FilePath (Join-Path $repo "out\x64\$Configuration\LiveWallpaperEngine.exe") -ArgumentList @('--test-seconds=120',('--test-library-root="{0}"' -f $library),('--test-wallpaper="{0}"' -f $video)) -PassThru
 $main=Wait-Window $process.Id 'LiveWallpaperEngine.Control';[void][WorkflowProbe]::SetForegroundWindow($main);Start-Sleep -Seconds 2
 $list=[WorkflowProbe]::GetDlgItem($main,1102)
 if($MessageOnly){
   [void][WorkflowProbe]::SendMessage($list,0x186,[IntPtr]::Zero,[IntPtr]::Zero)
   [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2192,[IntPtr]::Zero)
 }else{Click $list 220 34 -Right;[Windows.Forms.SendKeys]::SendWait('{DOWN}{DOWN}{DOWN}{ENTER}')}
 $options=Wait-Window $process.Id 'LiveWallpaperEngine.WallpaperOptions'
 Capture $options (Join-Path $output 'options.png')
 [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2196,[IntPtr]::Zero)
 Start-Sleep -Seconds 2
 [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2196,[IntPtr]::Zero)
 Click ([WorkflowProbe]::GetDlgItem($options,5201))
 [void][WorkflowProbe]::SendMessage([WorkflowProbe]::GetDlgItem($options,5220),0x405,[IntPtr]1,[IntPtr]74)
 [void][WorkflowProbe]::SendMessage([WorkflowProbe]::GetDlgItem($options,5221),0x405,[IntPtr]1,[IntPtr]23)
 [void][WorkflowProbe]::SendMessage([WorkflowProbe]::GetDlgItem($options,5222),0x405,[IntPtr]1,[IntPtr]37)
 [void][WorkflowProbe]::PostMessage($options,0x114,[IntPtr]::Zero,[IntPtr]::Zero)
 Click ([WorkflowProbe]::GetDlgItem($options,5207))
 [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2198,[IntPtr]::Zero);Start-Sleep -Milliseconds 350
 $saved=Get-Content -LiteralPath $settings -Raw -Encoding UTF8 | ConvertFrom-Json
 if($saved.version -ne 6 -or $saved.wallpaperPreferences[0].fit -ne 1 -or $saved.wallpaperPreferences[0].focusX -ne 74 -or $saved.wallpaperPreferences[0].volume -ne 37){throw 'Wallpaper preferences did not persist'}
 "OptionsSelectionAndPersistence=True; MessageOnly=$MessageOnly"
 # Settings uses real title-bar mouse input; diagnostic controls must remain reachable.
 $settingsButton=Wait-Window $process.Id 'LiveWallpaperEngine.SettingsButton';Click $settingsButton
 $dialog=Wait-Window $process.Id 'LiveWallpaperEngine.Settings';Click ([WorkflowProbe]::GetDlgItem($dialog,3307))
 Capture $dialog (Join-Path $output 'diagnostics.png')
 Click ([WorkflowProbe]::GetDlgItem($dialog,3309));$details=Wait-Window $process.Id 'LiveWallpaperEngine.Task';Start-Sleep -Milliseconds 250
 Capture $details (Join-Path $output 'last-session.png');Click ([WorkflowProbe]::GetDlgItem($details,5102))
 Click ([WorkflowProbe]::GetDlgItem($dialog,3303));"DiagnosticsAccess=True; MessageOnly=$MessageOnly"
 Click ([WorkflowProbe]::GetDlgItem($main,1103));$import=Wait-Window $process.Id 'LiveWallpaperEngine.ImportChoice'
 Capture $import (Join-Path $output 'import.png')
 Click ([WorkflowProbe]::GetDlgItem($import,3203));Click ([WorkflowProbe]::GetDlgItem($import,3203));Click ([WorkflowProbe]::GetDlgItem($import,3204));Click ([WorkflowProbe]::GetDlgItem($import,3204))
 [void][WorkflowProbe]::PostMessage($import,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 "ImportOptionsToggles=True; MessageOnly=$MessageOnly"
 [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2199,[IntPtr]::Zero)
 if(-not $process.WaitForExit(10000)){throw 'Clean exit timed out'}
 if($process.ExitCode -ne 0){throw "Exit code $($process.ExitCode)"}
 $bytes=[IO.File]::ReadAllBytes($log);$segment=[Text.Encoding]::UTF8.GetString($bytes,$baseline,$bytes.Length-$baseline)
 $counts=[regex]::Matches($segment,'CONTROLLED_VIDEO_TRANSFER_COUNT=(\d+)')
 if($counts.Count -lt 2 -or [long]$counts[1].Groups[1].Value -le [long]$counts[0].Groups[1].Value){throw 'Playback stalled with options open'}
 'OptionsPlaybackIndependent=True'
 # Restart from v6 JSON; no explicit wallpaper argument bypassing restoration.
 $process=Start-Process -FilePath (Join-Path $repo "out\x64\$Configuration\LiveWallpaperEngine.exe") -ArgumentList @('--test-seconds=30',('--test-library-root="{0}"' -f $library)) -PassThru
 $main=Wait-Window $process.Id 'LiveWallpaperEngine.Control';Start-Sleep -Milliseconds 600
 $list=[WorkflowProbe]::GetDlgItem($main,1102);[void][WorkflowProbe]::SendMessage($list,0x186,[IntPtr]::Zero,[IntPtr]::Zero)
 [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2192,[IntPtr]::Zero)
 $options=Wait-Window $process.Id 'LiveWallpaperEngine.WallpaperOptions'
 $focus=[WorkflowProbe]::SendMessage([WorkflowProbe]::GetDlgItem($options,5220),0x400,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32()
 $volume=[WorkflowProbe]::SendMessage([WorkflowProbe]::GetDlgItem($options,5222),0x400,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32()
 if($focus -ne 74 -or $volume -ne 37){throw 'Saved preferences were not restored'}
 Click ([WorkflowProbe]::GetDlgItem($options,5206));'OptionsRestartRestoration=True'
 foreach($mode in @(2,3,0)) {
   [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2192,[IntPtr]::Zero);$options=Wait-Window $process.Id 'LiveWallpaperEngine.WallpaperOptions'
   Click ([WorkflowProbe]::GetDlgItem($options,(5200+$mode)));Click ([WorkflowProbe]::GetDlgItem($options,5207))
   Start-Sleep -Milliseconds 300
   if($process.HasExited){throw 'Video mode change crashed'}
 }
 'AllVideoFitModesApplied=True'
 [void][WorkflowProbe]::PostMessage($main,0x111,[IntPtr]2199,[IntPtr]::Zero)
 if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'Restart run did not exit normally'}
 if($CompressionSource) {
   $cancelLibrary=Join-Path $output 'cancelled-library'
   $process=Start-Process -FilePath (Join-Path $repo "out\x64\$Configuration\LiveWallpaperEngine.exe") -ArgumentList @('--test-seconds=2',('--test-library-root="{0}"' -f $cancelLibrary),('--test-compressed-import="{0}"' -f $CompressionSource)) -PassThru
   $task=Wait-Window $process.Id 'LiveWallpaperEngine.Task';Click ([WorkflowProbe]::GetDlgItem($task,5102))
   if(-not $process.WaitForExit(15000) -or $process.ExitCode -ne 0){throw 'Cancellation did not shut down cleanly'}
   $files=@(Get-ChildItem -LiteralPath $cancelLibrary -File | Where-Object Name -NotMatch '^\.')
   if($files.Count -ne 0){throw 'Cancelled compressed import left media or partial files'}
   'TaskCancelButtonNoPartialFiles=True'
 }
 "Artifacts=$output"
} finally {
 if($process -and -not $process.HasExited){$w=[WorkflowProbe]::Find($process.Id,'LiveWallpaperEngine.Control');[void][WorkflowProbe]::PostMessage($w,0x111,[IntPtr]2199,[IntPtr]::Zero);if(-not $process.WaitForExit(10000)){Stop-Process -Id $process.Id -Force}}
 if($null -ne $snapshot){[IO.File]::WriteAllBytes($settings,$snapshot)}elseif(Test-Path -LiteralPath $settings){Remove-Item -LiteralPath $settings}
}
