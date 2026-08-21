; setup.iss — Inno Setup script for the c-agent desktop edition.
;
; Produces a standard Windows installer (wizard + uninstaller + Add/Remove
; Programs entry). Per-user install (no admin) to %LOCALAPPDATA%\c-agent.
; The desktop and start-menu shortcuts launch the native WebView2 shell
; (c-agent-desktop.exe), which starts the backend and opens the web console
; in a single window.
;
; Build (package.sh does this):
;   ISCC.exe tools/setup.iss

#define MyAppName "c-agent"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Cognitive OS"
#define MyAppExeName "c-agent-desktop.exe"

[Setup]
AppId={{2636E16C-C375-4BA7-867B-E6AACE2045F7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\c-agent
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=c-agent-setup
SetupIconFile=..\dist\c-agent.ico
UninstallDisplayIcon={app}\c-agent.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "..\build\cagent";             DestDir: "{app}"; DestName: "cagent.exe";          Flags: ignoreversion
Source: "..\build\c-agent-desktop.exe"; DestDir: "{app}";                                   Flags: ignoreversion
Source: "..\dist\c-agent.ico";         DestDir: "{app}";                                   Flags: ignoreversion
Source: "..\README.md";                DestDir: "{app}"; DestName: "README.txt";           Flags: ignoreversion

[Icons]
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\c-agent.ico"; WorkingDir: "{app}"
Name: "{group}\{#MyAppName}";       Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\c-agent.ico"; WorkingDir: "{app}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
