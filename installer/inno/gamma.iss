; Gamma Code installer - Inno Setup 6, per-user.
; Installs gamma.exe to %LOCALAPPDATA%\Programs\GammaCode\,
; adds that directory to the user PATH, and verifies the binary's
; SHA-256 against CHECKSUM.txt after the files are in place.

#define public AppName "Gamma Code"
#define public AppVersion "0.1.1"
#define public AppId "6FA0DE35-3C8E-4B27-9C58-52E793F7A0B4"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ZStudio-lab
AppPublisherURL=https://github.com/ZStudio-lab/Gamma-Code
AppSupportURL=https://github.com/ZStudio-lab/Gamma-Code/issues
AppUpdatesURL=https://github.com/ZStudio-lab/Gamma-Code/releases
DefaultDirName={autopf}\GammaCode
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
SetupIconFile=..\staging\gamma.ico
OutputDir=..\out
OutputBaseFilename=Gamma-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\gamma.exe

[Files]
Source: "..\staging\gamma.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\staging\CHECKSUM.txt"; DestDir: "{app}"; Flags: ignoreversion

[Code]

// -- user PATH management ------------------------------------------------

const
  WM_SETTINGCHANGE = $001A;
  SMTO_ABORTIFHUNG = $0002;

// Declared without a suffix: callable from setup AND uninstall processes.
// (LPARAM arrives as AnsiString lP when a String param is declared over
// SendMessageTimeoutA; we use the W entry with String = WideString in Inno.)
function SendMessageTimeout(hWnd: HWND; Msg: UINT; wParam: Longint;
  lParam: String; fuFlags, uTimeout: UINT; var lpdwResult: DWord): Longint;
  external 'SendMessageTimeoutW@user32.dll stdcall';

// Broadcast "environment changed" so shells pick up the PATH change.
procedure RefreshEnvironment;
var
  Res: DWord;
  lParam: String;
begin
  lParam := 'Environment';
  SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
    lParam, SMTO_ABORTIFHUNG, 5000, Res);
end;

function UserPathGet(var Path: String): Boolean;
begin
  Result := RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Path);
end;

function UserPathContains(const Path, Entry: String): Boolean;
var
  P, E: String;
begin
  P := ';' + AnsiLowercase(Path) + ';';
  E := ';' + AnsiLowercase(Entry) + ';';
  Result := Pos(E, P) > 0;
end;

procedure UserPathAdd(const EntryNoTrailing: String);
var
  Path, Entry, NewPath: String;
begin
  Entry := RemoveBackslashUnlessRoot(EntryNoTrailing);
  if not UserPathGet(Path) then
    Path := '';
  if UserPathContains(Path, Entry) then
    Exit; // already present - keeps repair runs idempotent
  if Path = '' then
    NewPath := Entry
  else
    NewPath := Path + ';' + Entry;
  // REG_EXPAND_SZ superset: preserves any existing %VAR% references
  // other installers may have left in the user path.
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', NewPath);
end;

procedure UserPathRemove(const EntryNoTrailing: String);
var
  Source, Path, Entry, Token, NewPath: String;
  Delim: Integer;
begin
  Entry := AnsiLowercase(RemoveBackslashUnlessRoot(EntryNoTrailing));
  if not UserPathGet(Path) then
    Exit;
  // Tokenize on ';' (append one so the last token terminates cleanly) and
  // rebuild the value without the GammaCode entry, skipping empties.
  Source := Path + ';';
  NewPath := '';
  while Length(Source) > 0 do
  begin
    Delim := Pos(';', Source);
    if Delim = 0 then
      Break;
    Token := Copy(Source, 1, Delim - 1);
    Source := Copy(Source, Delim + 1, MaxInt);
    if Token = '' then
      Continue;
    if AnsiLowercase(RemoveBackslashUnlessRoot(Token)) = Entry then
      Continue; // drop the entry we are removing
    if NewPath = '' then
      NewPath := Token
    else
      NewPath := NewPath + ';' + Token;
  end;
  if NewPath = Path then
    Exit; // nothing changed
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', NewPath);
end;

// -- SHA-256 verification ------------------------------------------------
// The setup embeds CHECKSUM.txt next to gamma.exe. PowerShell computes
// the real hash of the installed binary against it. Exit code 1 = mismatch.

function VerifyInstalledChecksum: Boolean;
var
  Params, WorkDir: String;
  ResultCode: Integer;
  Launched: Boolean;
begin
  Result := False;
  Params :=
    '-NoProfile -ExecutionPolicy Bypass -Command ' + #34 +
    '$h = Get-FileHash -Algorithm SHA256 ''.\gamma.exe'' | ' +
    'Select-Object -ExpandProperty Hash; ' +
    '$e = (Get-Content -LiteralPath ''.\CHECKSUM.txt'' -Raw).Trim(); ' +
    'if ($h -ne $e) { exit 1 } else { exit 0 }' + #34;
  WorkDir := ExpandConstant('{app}');
  Launched := Exec('powershell.exe', Params, WorkDir, SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  Result := Launched and (ResultCode = 0);
end;

procedure VerifyFailureWarning;
begin
  MsgBox(
    'Could not verify gamma.exe after installation.' + #13#10 +
    'The binary on disk does not match the checksum embedded in this ' +
    'installer. Remove the installed files and download a fresh copy from:' + #13#10 +
    'https://github.com/ZStudio-lab/Gamma-Code/releases' + #13#10 +
    '...or rebuild gamma.exe from source and reinstall.',
    mbCriticalError, MB_OK);
end;

// -- lifecycle hooks -----------------------------------------------------

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    UserPathAdd(ExpandConstant('{app}'));
    RefreshEnvironment;
    if not VerifyInstalledChecksum then
      VerifyFailureWarning;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    UserPathRemove(ExpandConstant('{app}'));
    RefreshEnvironment;
  end;
end;
