; Inno Setup Script for AirCast Server
[Setup]
AppName=AirCast Server
AppVersion=1.0.0
AppPublisher=AirCast Project
DefaultDirName={autopf}\AirCast Server
DefaultGroupName=AirCast Server
OutputBaseFilename=AirCast-Server-Setup
OutputDir=installer_output
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\AirCastServer.exe

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "package\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\AirCast Server"; Filename: "{app}\AirCastServer.exe"
Name: "{autodesktop}\AirCast Server"; Filename: "{app}\AirCastServer.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\AirCastServer.exe"; Description: "{cm:LaunchProgram,AirCast Server}"; Flags: nowait postinstall skipifsilent
