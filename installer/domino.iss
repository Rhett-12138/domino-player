; Domino Player — Windows per-user installer (Inno Setup 6)
; Build with installer/build.ps1; the script passes AppVersion/SourceDir/OutputDir.

; build.ps1 generates this include with absolute SourceDir/OutputDir/version.
#ifndef AppVersion
  #include "build.generated.iss"
#endif
#ifndef AppVersion
  #define AppVersion "0.3.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\domino-player-v0.3.0-win64"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
AppId={{D2D4D5D1-8F98-4A0E-9D66-6C0C5A0B8E11}
AppName=Domino Player
AppVersion={#AppVersion}
AppVerName=Domino Player {#AppVersion}
AppPublisher=Domino Player
AppPublisherURL=https://github.com/Rhett-12138/domino-player
AppSupportURL=https://github.com/Rhett-12138/domino-player/issues
DefaultDirName={localappdata}\Programs\DominoPlayer
DefaultGroupName=Domino Player
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=domino-player-v{#AppVersion}-win64-setup
SetupIconFile=..\flutter\windows\runner\resources\app_icon.ico
UninstallDisplayIcon={app}\domino_flutter.exe
LicenseFile=..\LICENSE
InfoBeforeFile=UNSIGNED_NOTICE.txt
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
UsePreviousAppDir=yes

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Domino Player"; Filename: "{app}\domino_flutter.exe"
Name: "{group}\卸载 Domino Player"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Domino Player"; Filename: "{app}\domino_flutter.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\domino_flutter.exe"; Description: "启动 Domino Player"; Flags: nowait postinstall skipifsilent

; 用户数据在 %APPDATA%\DominoPlayer，不随卸载删除；安装文件由 Inno 自动清理。

[Code]
function VCRuntimeAvailable: Boolean;
begin
  Result :=
    FileExists(ExpandConstant('{sys}\VCRUNTIME140.dll')) and
    FileExists(ExpandConstant('{sys}\MSVCP140.dll')) and
    FileExists(ExpandConstant('{sys}\VCRUNTIME140_1.dll'));
end;

function InitializeSetup: Boolean;
var
  ErrorCode: Integer;
begin
  Result := True;
  if not VCRuntimeAvailable then
  begin
    if MsgBox(
      'Microsoft Visual C++ 2015-2022 x64 runtime is missing.' + #13#10 +
      'Domino Player needs VCRUNTIME140.dll / MSVCP140.dll.' + #13#10#13#10 +
      'Open the official download page now? After installing it, run this setup again.',
      mbError, MB_YESNO) = IDYES then
    begin
      ShellExec('open', 'https://aka.ms/vs/17/release/vc_redist.x64.exe',
        '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
    end;
    Result := False;
  end;
end;
