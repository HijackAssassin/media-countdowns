; Inno Setup script for Media Countdowns. Run by package-app.bat (here), which Website\"archive this build.bat" calls:
;   ISCC /DAppVersion=x.y.z /DSourceDir=<project>\Website\MediaCountdowns /DOutDir=<project>\Website "Media Countdowns.iss"
; Packs everything in SourceDir (the app + Notifier\, a copy of Build\) into
; OutDir\Media-Countdowns-V<version>-Setup.exe - the version in that name is what "push to website.bat"
; (and the website) read it from, so the name must not change.
; AppName (and so the uninstall entry), the install folder and the shortcuts are unchanged from
; V5.4.28, so a new version installs over an old one.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\Website\MediaCountdowns"
#endif
#ifndef OutDir
  #define OutDir "..\Website"
#endif

[Setup]
AppName=Media Countdowns
AppVersion={#AppVersion}
AppPublisher=Patrick Price
DefaultDirName={autopf}\Media Countdowns
DefaultGroupName=Media Countdowns
OutputDir={#OutDir}
OutputBaseFilename=Media-Countdowns-V{#AppVersion}-Setup
Compression=lzma
SolidCompression=yes

; Installer EXE icon
SetupIconFile=appicon.ico

; Icon shown in Windows "Add/Remove Programs"
UninstallDisplayIcon={app}\MediaCountdowns.exe

[Files]
; Copy all contents (including subfolders) into Program Files\Media Countdowns\
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; Start Menu shortcut
Name: "{group}\Media Countdowns"; Filename: "{app}\MediaCountdowns.exe"; WorkingDir: "{app}"

; Desktop shortcut (optional)
Name: "{commondesktop}\Media Countdowns"; Filename: "{app}\MediaCountdowns.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\MediaCountdowns.exe"; Description: "Launch Media Countdowns now"; Flags: nowait postinstall skipifsilent
