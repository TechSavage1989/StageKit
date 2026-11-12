; Inno Setup script for the StageKit Windows installer.
;
; Built by .github/scripts/Package-Windows.ps1, which passes:
;   /DAppVersion=   version shown in "Apps & features"
;   /DSourceDir=    folder that contains the built "stagekit" plugin folder
;   /DOutputDir=    where to write the installer
;   /DOutputBase=   installer file name without ".exe"
;
; The plugin is installed where OBS Studio looks for third-party plugins:
;   C:\ProgramData\obs-studio\plugins\stagekit
; so the installer has no folder page. Settings live in the user's OBS scene
; collection and are never touched by installing or uninstalling.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\..\release\RelWithDebInfo"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\..\release"
#endif
#ifndef OutputBase
  #define OutputBase "stagekit-windows-x64-Installer"
#endif

[Setup]
; Keep this AppId for the life of the product: it is how Windows recognises
; an upgrade and finds the uninstaller.
AppId={{8C2F4B1E-6A53-4C0B-9E7A-5D1F2B3C4A60}
AppName=StageKit for OBS Studio
AppVersion={#AppVersion}
AppPublisher=TechSavage1989
AppPublisherURL=https://github.com/TechSavage1989/StageKit
AppSupportURL=https://github.com/TechSavage1989/StageKit/issues
AppUpdatesURL=https://github.com/TechSavage1989/StageKit/releases
DefaultDirName={commonappdata}\obs-studio\plugins\stagekit
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=StageKit for OBS Studio
; If OBS is running it holds the plugin file open; offer to close it.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install [name/ver] as a plugin for OBS Studio.%n%nClose OBS Studio before continuing. Your scenes and settings are not changed.
FinishedLabel=StageKit is installed. Start OBS Studio: the StageKit filters appear in any source's filter list, and the dock under Docks.

[Files]
Source: "{#SourceDir}\stagekit\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: dirifempty; Name: "{app}"
