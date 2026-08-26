#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif
#ifndef MyAppDisplayVersion
  #define MyAppDisplayVersion MyAppVersion
#endif
#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "LiveWallpaperEngine-" + MyAppVersion + "-x64-setup"
#endif
#ifndef ChineseMessagesFile
  #error Build with tools\build-release.ps1 so the pinned Chinese translation is supplied.
#endif

#define MyAppName "Live Wallpaper Engine"
#define MyAppExeName "LiveWallpaperEngine.exe"
#define MyAppPublisher "Live Wallpaper Engine contributors"
#define MyAppUrl "https://github.com/wenchenyang874-a11y/Live_Wallpaper_Engine"

[Setup]
AppId={{A5FD237F-05F8-4DA8-A3C3-61DA448AD7BB}
AppName={#MyAppName}
AppVersion={#MyAppDisplayVersion}
AppVerName={#MyAppName} {#MyAppDisplayVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppSupportURL={#MyAppUrl}/issues
AppUpdatesURL={#MyAppUrl}/releases
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductVersion={#MyAppVersion}.0
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename={#MyOutputBaseFilename}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19045
; The application intentionally maps WM_CLOSE to "hide to tray". During an
; upgrade that behavior leaves the executable locked, so Restart Manager must
; be allowed to terminate it after a graceful close attempt fails.
CloseApplications=force
RestartApplications=no
SetupLogging=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\assets\app-icon.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "{#ChineseMessagesFile}"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\out\x64\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"
Source: "..\README.md"; DestDir: "{app}"
Source: "..\CHANGELOG.md"; DestDir: "{app}"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[Code]
const
  UninstallRegistryKey =
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A5FD237F-05F8-4DA8-A3C3-61DA448AD7BB}_is1';
  ApplicationWindowClass = 'LiveWallpaperEngine.Control';
  ApplicationWindowTitle = 'Live Wallpaper Engine';
  InstallerShutdownMessage = $8008;
  ProcessTerminate = $0001;
  Synchronize = $00100000;
  WaitObject0 = $00000000;

function FindWindow(lpClassName, lpWindowName: String): HWND;
  external 'FindWindowW@user32.dll stdcall';
function GetWindowThreadProcessId(hWnd: HWND; var ProcessId: LongWord): LongWord;
  external 'GetWindowThreadProcessId@user32.dll stdcall';
function PostMessage(hWnd: HWND; Msg: LongWord; wParam, lParam: Longint): Boolean;
  external 'PostMessageW@user32.dll stdcall';
function OpenProcess(DesiredAccess: LongWord; InheritHandle: Boolean;
  ProcessId: LongWord): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: LongWord): LongWord;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function TerminateProcess(ProcessHandle: THandle; ExitCode: LongWord): Boolean;
  external 'TerminateProcess@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function ExistingInstallationFound(): Boolean;
begin
  { The stable AppId is the authoritative identity. Check both per-user and
    elevated installation scopes because the installer normally uses HKCU but
    permits the user to explicitly choose an administrative installation. }
  Result := RegKeyExists(HKCU, UninstallRegistryKey) or
            RegKeyExists(HKLM, UninstallRegistryKey);
end;

function InitializeSetup(): Boolean;
var
  OverwriteMode: String;
begin
  Result := True;
  if not ExistingInstallationFound() then
    Exit;

  { This parameter is reserved for controlled installer regression tests and
    unattended deployment. Interactive launches always show the confirmation.
    Silent launches without an explicit answer safely use the default IDNO. }
  OverwriteMode := Lowercase(ExpandConstant('{param:LWEOVERWRITE|}'));
  if OverwriteMode = 'yes' then
    Exit;
  if OverwriteMode = 'no' then
  begin
    Result := False;
    Exit;
  end;

  Result := SuppressibleMsgBox(
    '检测到已安装 Live Wallpaper Engine。是否覆盖安装？' + #13#10 +
    '选择“否”将退出安装。' + #13#10#13#10 +
    'Live Wallpaper Engine is already installed. Overwrite it?' + #13#10 +
    'Select No to exit Setup.',
    mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
end;

function ShutdownApplicationForUpgrade(): String;
var
  ApplicationWindow: HWND;
  ProcessId: LongWord;
  ProcessHandle: THandle;
begin
  Result := '';
  ApplicationWindow := FindWindow(
    ApplicationWindowClass, ApplicationWindowTitle);
  if ApplicationWindow = 0 then
    Exit;

  ProcessId := 0;
  if GetWindowThreadProcessId(ApplicationWindow, ProcessId) = 0 then
  begin
    Result := '无法识别正在运行的 Live Wallpaper Engine 进程，请手动退出后重试。';
    Exit;
  end;

  ProcessHandle := OpenProcess(
    ProcessTerminate or Synchronize, False, ProcessId);
  if ProcessHandle = 0 then
  begin
    Result := '无法关闭正在运行的 Live Wallpaper Engine，请手动退出后重试。';
    Exit;
  end;

  try
    Log('Requesting Live Wallpaper Engine PID ' + IntToStr(ProcessId) +
      ' to exit for upgrade.');
    PostMessage(ApplicationWindow, InstallerShutdownMessage, 0, 0);
    if WaitForSingleObject(ProcessHandle, 750) = WaitObject0 then
    begin
      Log('Live Wallpaper Engine exited gracefully for upgrade.');
      Exit;
    end;

    { Versions before 1.0.0 do not understand InstallerShutdownMessage. End
      only the process owning our stable control-window class, instead of
      leaving Restart Manager to wait on WM_CLOSE (which hides to tray). }
    Log('Live Wallpaper Engine PID ' + IntToStr(ProcessId) +
      ' did not exit; terminating legacy version.');
    if not TerminateProcess(ProcessHandle, 0) then
    begin
      Result := '无法结束旧版 Live Wallpaper Engine，请手动退出后重试。';
      Exit;
    end;
    if WaitForSingleObject(ProcessHandle, 1250) <> WaitObject0 then
      Result := '等待旧版 Live Wallpaper Engine 退出超时，请手动退出后重试。';
  finally
    CloseHandle(ProcessHandle);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := ShutdownApplicationForUpgrade();
end;
