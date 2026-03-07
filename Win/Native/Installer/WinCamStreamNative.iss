#define MyAppName "WinCamStream"
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#ifndef SourceRuntime
  #define SourceRuntime "..\Runtime"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\Release"
#endif
#define SetupIcon "..\WcsNativeWinUI\Assets\WcsLogo.ico"

[Setup]
AppId={{AF2ED2EE-A488-49D2-A310-7E8F5CB7A1E6}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=WinCamStream
DefaultDirName={autopf}\WinCamStream
DefaultGroupName=WinCamStream
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\WcsNativeWinUI\Assets\WcsLogo.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=WinCamStream-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
SetupLogging=yes
SetupIconFile={#SetupIcon}

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceRuntime}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.pdb,*.lib,*.exp,wcs_native_winui-launcher.log"

[Icons]
Name: "{group}\WinCamStream"; Filename: "{app}\wcs_native_winui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\WcsNativeWinUI\Assets\WcsLogo.ico"
Name: "{group}\WinCamStream (Legacy UI)"; Filename: "{app}\wcs_native_ui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\WcsNativeWinUI\Assets\WcsLogo.ico"
Name: "{group}\Install UnityCapture Driver"; Filename: "{app}\UnityCapture\Install.bat"; WorkingDir: "{app}\UnityCapture"
Name: "{group}\Uninstall UnityCapture Driver"; Filename: "{app}\UnityCapture\Uninstall.bat"; WorkingDir: "{app}\UnityCapture"
Name: "{group}\Uninstall WinCamStream"; Filename: "{uninstallexe}"
Name: "{autodesktop}\WinCamStream"; Filename: "{app}\wcs_native_winui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\WcsNativeWinUI\Assets\WcsLogo.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\wcs_native_winui.exe"; Description: "{cm:LaunchProgram,WinCamStream}"; Flags: nowait postinstall skipifsilent
