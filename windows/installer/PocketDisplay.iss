; PocketDisplay Windows installer (Inno Setup)
; ---------------------------------------------------------------------------
; Builds an UNSIGNED, self-contained installer. The exe links the static CRT
; (no VC++ Redistributable needed) and bundles adb + the VDD driver files.
; The VDD driver is NOT installed by Setup - the app self-installs it at
; runtime when the user picks Extended mode. We only bundle the files here.
;
; Output: windows/installer/dist/PocketDisplay-Setup.exe
;
; SourceDir defaults to the directory containing this .iss (windows/installer).
; The Release build output is ..\build\Release relative to that.

#define MyAppName "PocketDisplay"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "PocketDisplay"
#define MyAppExeName "PocketDisplay.exe"
#define ReleaseDir "..\build\Release"

[Setup]
AppId={{B3F0A7C2-7C4E-4D2A-9E2C-7A1F6D9C0E11}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Program Files install + runtime driver install both need admin.
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=dist
OutputBaseFilename=PocketDisplay-Setup
SetupIconFile=..\resources\icon.ico
UninstallDisplayIcon={app}\icon.ico
UninstallDisplayName={#MyAppName}
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
; Clean unsigned metadata so SmartScreen "More info" shows a clear name.
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
VersionInfoVersion={#MyAppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#ReleaseDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; Bundled adb (USB mode) - all 3 files must stay together.
Source: "{#ReleaseDir}\platform-tools\adb.exe";          DestDir: "{app}\platform-tools"; Flags: ignoreversion
Source: "{#ReleaseDir}\platform-tools\AdbWinApi.dll";    DestDir: "{app}\platform-tools"; Flags: ignoreversion
Source: "{#ReleaseDir}\platform-tools\AdbWinUsbApi.dll"; DestDir: "{app}\platform-tools"; Flags: ignoreversion
; Virtual Display Driver files (installed by the app at runtime, not by Setup).
Source: "{#ReleaseDir}\drivers\virtual-display\MttVDD.inf";      DestDir: "{app}\drivers\virtual-display"; Flags: ignoreversion
Source: "{#ReleaseDir}\drivers\virtual-display\MttVDD.cat";      DestDir: "{app}\drivers\virtual-display"; Flags: ignoreversion
Source: "{#ReleaseDir}\drivers\virtual-display\MttVDD.dll";      DestDir: "{app}\drivers\virtual-display"; Flags: ignoreversion
Source: "{#ReleaseDir}\drivers\virtual-display\vdd_settings.xml"; DestDir: "{app}\drivers\virtual-display"; Flags: ignoreversion
; Fonts + branding assets.
Source: "{#ReleaseDir}\Anton-Regular.ttf";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleaseDir}\SpaceGrotesk-Medium.ttf"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleaseDir}\SpaceGrotesk-Bold.ttf";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleaseDir}\logo-primary.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleaseDir}\icon.ico";         DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleaseDir}\LICENSES.txt";     DestDir: "{app}"; Flags: ignoreversion
; Uninstall-time VDD cleanup helper.
Source: "uninstall-vdd.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Best-effort removal of the VDD driver + virtual display device. Runs hidden
; (no console flash); errors are ignored if the driver was never installed.
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\uninstall-vdd.ps1"""; Flags: runhidden; RunOnceId: "RemoveMttVDD"
