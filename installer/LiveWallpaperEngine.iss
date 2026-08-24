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

[Code]
const
  UninstallRegistryKey =
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A5FD237F-05F8-4DA8-A3C3-61DA448AD7BB}_is1';

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
