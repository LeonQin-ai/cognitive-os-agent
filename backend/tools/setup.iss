; setup.iss — Inno Setup script for the cognitive-os-agent desktop edition.
;
; Produces a standard Windows installer (wizard + uninstaller + Add/Remove
; Programs entry). Per-user install (no admin) to %LOCALAPPDATA%\cognitive-os-agent.
; The desktop and start-menu shortcuts launch the native WebView2 shell
; (cognitive-os-agent-desktop.exe), which starts the backend and opens the web console
; in a single window.
;
; Build (package.sh does this):
;   ISCC.exe tools/setup.iss

#define MyAppName "cognitive-os-agent"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Cognitive OS"
#define MyAppExeName "cognitive-os-agent-desktop.exe"

[Setup]
AppId={{2636E16C-C375-4BA7-867B-E6AACE2045F7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\cognitive-os-agent
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=cognitive-os-agent-setup
SetupIconFile=..\dist\cognitive-os-agent.ico
UninstallDisplayIcon={app}\cognitive-os-agent.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "..\build\cognitive-os-agent.exe";         DestDir: "{app}"; DestName: "cognitive-os-agent.exe";          Flags: ignoreversion
Source: "..\build\cognitive-os-agent-desktop.exe"; DestDir: "{app}";                                   Flags: ignoreversion
Source: "..\dist\cognitive-os-agent.ico";         DestDir: "{app}";                                   Flags: ignoreversion
Source: "..\README.md";                DestDir: "{app}"; DestName: "README.txt";           Flags: ignoreversion

[Icons]
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\cognitive-os-agent.ico"; WorkingDir: "{app}"
Name: "{group}\{#MyAppName}";       Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\cognitive-os-agent.ico"; WorkingDir: "{app}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
