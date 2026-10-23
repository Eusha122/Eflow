; The installer people download from the website. Built by tools\build_installer.ps1.
;
; It installs per user (no administrator prompt), registers Eflow's sparse package so the Notifications tab can read
; what Windows shows, and leaves nothing behind when uninstalled. It is not code-signed, so Windows SmartScreen warns
; about it the first time; the Store build is the route for people who would rather not see that.

#define AppName "Eflow"
#define AppPublisher "Eusha Ibna Akbor"
#define AppUrl "https://safeshare.co/eflow"
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef Payload
  #define Payload "..\..\build\installer\payload"
#endif

[Setup]
AppId={{8E4F2C7A-5D31-4B6E-9A42-E7C1F0B83D55}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
VersionInfoVersion={#AppVersion}
; Per user: no administrator prompt, and nothing is written outside this user's own folders.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\Eflow
DisableDirPage=yes
DisableProgramGroupPage=yes
DefaultGroupName={#AppName}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\eflow.exe
OutputDir=..\..\build\installer
OutputBaseFilename=EflowSetup
SetupIconFile=..\..\src\eflow.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 2004 or newer: what the island's composition and the sparse package both need.
MinVersion=10.0.19041

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Start Eflow when I sign in"; GroupDescription: "Getting started:"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Getting started:"; Flags: unchecked

[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\eflow.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\eflow.exe"; Tasks: desktopicon

[Registry]
; "Start with Windows", only when asked for. Eflow's own tray menu and the tour's last step write the same value.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Eflow"; \
    ValueData: """{app}\eflow.exe"""; Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\eflow.exe"; Description: "Open {#AppName} now"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Its own settings and the Python runtime / speech models Solu downloaded (several GB): all under one folder.
Type: filesandordirs; Name: "{localappdata}\Eflow"

[Code]
// The sparse package gives eflow.exe a package identity, which is what Windows requires before an app may read the
// notifications it shows. Registering it is best-effort: everything else in Eflow works without it, so a failure
// here must not fail the install.
procedure RegisterSparsePackage();
var
  resultCode: Integer;
  script: String;
begin
  script := ExpandConstant('{app}\package\register.ps1');
  if FileExists(script) then
    Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + script + '" -AppDir "' +
         ExpandConstant('{app}') + '"', '', SW_HIDE, ewWaitUntilTerminated, resultCode);
end;

procedure UnregisterSparsePackage();
var
  resultCode: Integer;
begin
  Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command ' +
       '"Get-AppxPackage -Name Eflow.Island | Remove-AppxPackage"', '', SW_HIDE, ewWaitUntilTerminated, resultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then RegisterSparsePackage();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  resultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    // Close a running copy first, or its files cannot be removed.
    Exec('taskkill.exe', '/F /IM eflow.exe', '', SW_HIDE, ewWaitUntilTerminated, resultCode);
    UnregisterSparsePackage();
  end;
end;
