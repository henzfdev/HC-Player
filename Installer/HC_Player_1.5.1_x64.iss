#define MyAppName "HC Player"
#define MyAppVersion "1.5.1"
#define MyAppVersionQuad "1.5.1.0"
#define MyAppPublisher "HC Player"
#define MyAppURL "https://github.com/henzfdev/HC-Player"
#define MyAppExeName "HC Player.exe"

; Permanent HC Player installer identity. Keep this AppId on future upgrades.
#define MyAppId "{{805B14D6-30AC-45A2-BE42-CE0EB9F68408}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
AppCopyright=Copyright (c) 2026 Heinz

DefaultDirName={autopf}\HC Player
DefaultGroupName=HC Player
DisableProgramGroupPage=yes
AllowNoIcons=yes

LicenseFile=Payload\LICENSE
SetupIconFile=InstallerAssets\HCPlayer_Setup.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName=HC Player 1.5.1

OutputDir=Output
OutputBaseFilename=HC_Player_1.5.1_x64_Setup

Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern dynamic
SetupLogging=yes

PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763

CloseApplications=yes
CloseApplicationsFilter=HC Player.exe
RestartApplications=no

; File associations belong to HC Player itself (per-user).
ChangesAssociations=no

UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousLanguage=yes

VersionInfoVersion={#MyAppVersionQuad}
VersionInfoProductVersion={#MyAppVersionQuad}
VersionInfoProductName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=HC Player Setup
VersionInfoOriginalFileName=HC_Player_1.5.1_x64_Setup.exe
VersionInfoCopyright=Copyright (c) 2026 Heinz

[Languages]
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
ptbr.HCDesktopIcon=Criar um ícone do HC Player na Área de Trabalho
english.HCDesktopIcon=Create an HC Player desktop icon
ptbr.HCAdditionalTasks=Tarefas adicionais:
english.HCAdditionalTasks=Additional tasks:
ptbr.HCInitialLanguage=Aplicando o idioma inicial do HC Player...
english.HCInitialLanguage=Applying the initial HC Player language...
ptbr.HCRemoveData=Deseja também remover configurações, histórico e outros dados do HC Player deste computador?
english.HCRemoveData=Also remove HC Player settings, history, and other HC Player user data from this computer?

[Tasks]
Name: "desktopicon"; Description: "{cm:HCDesktopIcon}"; GroupDescription: "{cm:HCAdditionalTasks}"; Flags: unchecked

[Files]
Source: "Prerequisites\vc_redist.x64.exe"; Flags: dontcopy noencryption
Source: "Prerequisites\WindowsAppRuntimeInstall-x64.exe"; Flags: dontcopy noencryption

Source: "Payload\HC Player.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "Payload\*"; Excludes: "\HC Player.exe,\portable.flag,\Data\*,*.pdb,*.lib,*.exp,*.ilk,*.obj,*.iobj,*.ipdb"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: files; Name: "{app}\portable.flag"
Type: files; Name: "{app}\HC Player.pdb"
Type: files; Name: "{app}\HC Player.lib"
Type: files; Name: "{app}\HC Player.exp"

[Icons]
Name: "{group}\HC Player"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\HC Player"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--hc-initial-language {code:InitialLanguageTag}"; WorkingDir: "{app}"; StatusMsg: "{cm:HCInitialLanguage}"; Flags: runasoriginaluser waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,HC Player}"; WorkingDir: "{app}"; Flags: runasoriginaluser nowait postinstall skipifsilent

[Code]
const
  SHCNE_ASSOCCHANGED = $08000000;
  SHCNF_IDLIST = $0000;

var
  PrerequisiteRestartNeeded: Boolean;
  RemoveUserDataChosen: Boolean;

procedure SHChangeNotify(
  wEventId, uFlags: LongWord; dwItem1, dwItem2: NativeUInt);
  external 'SHChangeNotify@shell32.dll stdcall';

function InitialLanguageTag(Param: String): String;
begin
  if ActiveLanguage = 'ptbr' then
    Result := 'pt-BR'
  else
    Result := 'en-US';
end;

function RunPrerequisite(
  const FileName, Parameters, DisplayName: String): String;
var
  ResultCode: Integer;
  FullPath: String;
begin
  Result := '';
  ExtractTemporaryFile(FileName);
  FullPath := ExpandConstant('{tmp}\') + FileName;

  Log('HC Player prerequisite: ' + DisplayName);
  if not Exec(
    FullPath, Parameters, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Result := 'Could not start prerequisite: ' + DisplayName + '.';
    Exit;
  end;

  Log(DisplayName + ' exit code: ' + IntToStr(ResultCode));

  if ResultCode = 3010 then
  begin
    PrerequisiteRestartNeeded := True;
    Exit;
  end;

  { 1638 can mean another/newer compatible package is already installed. }
  if (ResultCode <> 0) and (ResultCode <> 1638) then
    Result := DisplayName + ' failed with exit code ' +
      IntToStr(ResultCode) + '.';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  PrerequisiteRestartNeeded := False;

  Result := RunPrerequisite(
    'vc_redist.x64.exe',
    '/install /quiet /norestart',
    'Microsoft Visual C++ v14 Runtime (x64)');
  if Result <> '' then Exit;

  Result := RunPrerequisite(
    'WindowsAppRuntimeInstall-x64.exe',
    '--quiet',
    'Windows App Runtime 2.4.0 (x64)');
  if Result <> '' then Exit;

  if PrerequisiteRestartNeeded then
    NeedsRestart := True;
end;

function IsRealUserSid(const Name: String): Boolean;
begin
  Result :=
    ((Pos('S-1-5-21-', Name) = 1) or
     (Pos('S-1-12-1-', Name) = 1)) and
    (Pos('_Classes', Name) = 0);
end;

function UserHiveHasHCPlayerState(const Sid: String): Boolean;
var
  Base: String;
begin
  Base := Sid + '\';
  Result :=
    RegKeyExists(HKEY_USERS, Base + 'Software\HCPlayer') or
    RegKeyExists(HKEY_USERS,
      Base + 'Software\Classes\Applications\HC Player.exe') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MKV.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.AAC.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.AVI.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.FLAC.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.M3U8.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.M4A.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MKA.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MOV.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MP3.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MP4.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MPEG.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.OGG.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.OPUS.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.WAV.1') or
    RegKeyExists(HKEY_USERS, Base + 'Software\Classes\HCPlayer.WEBM.1') or
    RegValueExists(HKEY_USERS,
      Base + 'Software\RegisteredApplications', 'HC Player');
end;

procedure RemoveOpenWithValue(
  const Sid, ExtensionName, ProgId: String);
var
  KeyName: String;
begin
  KeyName := Sid + '\Software\Classes\' + ExtensionName +
    '\OpenWithProgids';
  if RegValueExists(HKEY_USERS, KeyName, ProgId) then
    RegDeleteValue(HKEY_USERS, KeyName, ProgId);
  RegDeleteKeyIfEmpty(HKEY_USERS, KeyName);
end;

procedure RemoveHCPlayerRegistryForSid(const Sid: String);
var
  Base: String;
begin
  Base := Sid + '\';

  RegDeleteKeyIncludingSubkeys(
    HKEY_USERS, Base + 'Software\HCPlayer');
  RegDeleteKeyIncludingSubkeys(
    HKEY_USERS, Base + 'Software\Classes\Applications\HC Player.exe');

  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MKV.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.AAC.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.AVI.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.FLAC.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.M3U8.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.M4A.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MKA.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MOV.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MP3.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MP4.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.MPEG.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.OGG.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.OPUS.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.WAV.1');
  RegDeleteKeyIncludingSubkeys(HKEY_USERS, Base + 'Software\Classes\HCPlayer.WEBM.1');

  if RegValueExists(
    HKEY_USERS, Base + 'Software\RegisteredApplications', 'HC Player') then
    RegDeleteValue(
      HKEY_USERS, Base + 'Software\RegisteredApplications', 'HC Player');

  RemoveOpenWithValue(Sid, '.mkv',  'HCPlayer.MKV.1');
  RemoveOpenWithValue(Sid, '.aac',  'HCPlayer.AAC.1');
  RemoveOpenWithValue(Sid, '.avi',  'HCPlayer.AVI.1');
  RemoveOpenWithValue(Sid, '.flac', 'HCPlayer.FLAC.1');
  RemoveOpenWithValue(Sid, '.m3u8', 'HCPlayer.M3U8.1');
  RemoveOpenWithValue(Sid, '.m4a',  'HCPlayer.M4A.1');
  RemoveOpenWithValue(Sid, '.mka',  'HCPlayer.MKA.1');
  RemoveOpenWithValue(Sid, '.mov',  'HCPlayer.MOV.1');
  RemoveOpenWithValue(Sid, '.mp3',  'HCPlayer.MP3.1');
  RemoveOpenWithValue(Sid, '.mp4',  'HCPlayer.MP4.1');
  RemoveOpenWithValue(Sid, '.mpeg', 'HCPlayer.MPEG.1');
  RemoveOpenWithValue(Sid, '.ogg',  'HCPlayer.OGG.1');
  RemoveOpenWithValue(Sid, '.opus', 'HCPlayer.OPUS.1');
  RemoveOpenWithValue(Sid, '.wav',  'HCPlayer.WAV.1');
  RemoveOpenWithValue(Sid, '.webm', 'HCPlayer.WEBM.1');
end;

function ResolveLoadedUserProfilePath(
  const Sid: String; var ProfilePath: String): Boolean;
var
  RawPath, Token, SystemDrive: String;
begin
  Result := False;
  ProfilePath := '';

  if not RegQueryStringValue(
    HKEY_LOCAL_MACHINE,
    'SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\' + Sid,
    'ProfileImagePath', RawPath) then
    Exit;

  Token := '%SystemDrive%';
  if LowerCase(Copy(RawPath, 1, Length(Token))) = LowerCase(Token) then
  begin
    SystemDrive := GetEnv('SystemDrive');
    if SystemDrive = '' then
      SystemDrive := ExpandConstant('{sd}');
    RawPath := SystemDrive +
      Copy(RawPath, Length(Token) + 1, Length(RawPath));
  end;

  if Pos('%', RawPath) <> 0 then Exit;

  while (Length(RawPath) > 3) and
    (RawPath[Length(RawPath)] = '\') do
    Delete(RawPath, Length(RawPath), 1);

  if (Length(RawPath) < 4) or
     (RawPath[2] <> ':') or
     (RawPath[3] <> '\') or
     (Pos('..', RawPath) <> 0) or
     (not DirExists(RawPath)) then
    Exit;

  ProfilePath := RawPath;
  Result := True;
end;

function HCPlayerUserDataMarkerExists(
  const ProfilePath: String): Boolean;
begin
  Result := FileExists(
    ProfilePath +
    '\AppData\Local\HC Player\hc-player-owned.marker');
end;

procedure RemoveHCPlayerLocalDataForProfile(
  const ProfilePath: String);
var
  LocalParent, TargetPath: String;
begin
  LocalParent := ProfilePath + '\AppData\Local';
  TargetPath := LocalParent + '\HC Player';

  if (LowerCase(ExtractFileDir(TargetPath)) <> LowerCase(LocalParent)) or
     (LowerCase(ExtractFileName(TargetPath)) <> 'hc player') then
  begin
    Log('HC Player cleanup: containment check failed; skipped.');
    Exit;
  end;

  if DirExists(TargetPath) then
    if not DelTree(TargetPath, True, True, True) then
      Log('HC Player cleanup: could not fully remove ' + TargetPath);
end;

procedure CleanupHCPlayerLoadedUserProfiles(
  RemoveLocalData: Boolean);
var
  UserHives: TArrayOfString;
  I: Integer;
  Sid, ProfilePath: String;
  RegistryOwned, MarkerOwned, ProfileResolved: Boolean;
begin
  if not RegGetSubkeyNames(HKEY_USERS, '', UserHives) then
  begin
    Log('HC Player cleanup: unable to enumerate HKEY_USERS; skipped.');
    Exit;
  end;

  for I := 0 to GetArrayLength(UserHives) - 1 do
  begin
    Sid := UserHives[I];
    if IsRealUserSid(Sid) then
    begin
      RegistryOwned := UserHiveHasHCPlayerState(Sid);
      ProfileResolved :=
        ResolveLoadedUserProfilePath(Sid, ProfilePath);
      MarkerOwned := False;
      if ProfileResolved then
        MarkerOwned := HCPlayerUserDataMarkerExists(ProfilePath);

      if RegistryOwned then
        RemoveHCPlayerRegistryForSid(Sid);

      if RemoveLocalData and ProfileResolved and
         (MarkerOwned or RegistryOwned) then
        RemoveHCPlayerLocalDataForProfile(ProfilePath);
    end;
  end;

  SHChangeNotify(
    SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
end;

procedure CurUninstallStepChanged(
  CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RemoveUserDataChosen :=
      MsgBox(
        CustomMessage('HCRemoveData'),
        mbConfirmation,
        MB_YESNO) = IDYES;
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    CleanupHCPlayerLoadedUserProfiles(RemoveUserDataChosen);
  end;
end;
