; ==============================================================================
;  UNI 76 (Nostalgia Audio) - Inno Setup installer.
;
;  RC1 status: this installer is real (compiles a working installer that
;  copies the actual built .vst3 into the standard Windows VST3 location,
;  checks for the WebView2 Runtime, and supports clean install/uninstall),
;  used for internal RC candidate builds only - NOT signed, NOT published.
;  See docs/RELEASE.md for what's still required before a public release
;  (code signing being the main one).
;
;  Keep AppName / AppPublisher / AppVersion in sync with
;  cmake/PluginIdentity.cmake by hand; Inno Setup cannot include a CMake
;  file directly. AppVersion here is an *installer* display version, kept
;  deliberately separate from UNI76_VERSION (the plugin binary's own
;  internal/host-visible version, cmake/PluginIdentity.cmake) - see
;  docs/FULL_DSP_AUDIT.md's RC1 report for the versioning rationale. Do
;  not bump UNI76_VERSION itself here.
; ==============================================================================

#define MyAppName "UNI 76"
#define MyAppPublisher "Nostalgia Audio"
#define MyAppVersion "0.1.0-rc1"
#define MyAppURL "https://nostalgiaaudio.com"
#define MyVst3BundleName "UNI 76.vst3"

; Path to the built Release VST3 bundle, relative to this .iss file.
; Override at compile time with "iscc UNI76.iss /DMyBuiltVst3Dir=..." if
; building from a different location.
#ifndef MyBuiltVst3Dir
  #define MyBuiltVst3Dir "..\..\build\windows-release\Source\Plugin\UNI76_artefacts\Release\VST3\UNI 76.vst3"
#endif

[Setup]
; Generated once for this project (Inno Setup -> Tools -> Generate GUID) -
; this is how Windows identifies upgrades to this installer across
; versions. Never change it after any build leaves this machine.
AppId={{AD0D1F20-AF8E-4EFE-A506-8422D280C1D3}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
; The plugin bundle itself always installs to the standard shared VST3
; location (see [Files] below, which targets {commoncf64}\VST3 directly,
; independent of {app}) - {app} here is only this product's OWN small
; folder for the uninstaller, licence notices, and a short README, kept
; deliberately separate from the shared, multi-vendor VST3 folder so
; uninstall never has to guess which loose files in that shared folder
; belong to UNI 76.
DefaultDirName={autopf}\Nostalgia Audio\UNI 76
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=UNI76-Windows-x64-RC1-Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern

[Files]
; The VST3 bundle - a directory, not a single file - is copied whole into
; the standard per-machine VST3 location, preserving its own internal
; "UNI 76.vst3" folder name (Contents/Resources/moduleinfo.json,
; Contents/x86_64-win/UNI 76.vst3). recursesubdirs+createallsubdirs
; preserves the bundle's own internal structure exactly.
Source: "{#MyBuiltVst3Dir}\*"; DestDir: "{commoncf64}\VST3\{#MyVst3BundleName}"; Flags: recursesubdirs createallsubdirs ignoreversion

; Licence/notice files and a short RC1 README live in this product's own
; folder, not the shared VST3 folder.
Source: "..\..\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "RC1_README.txt"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
; The bundle directory itself isn't a single [Files] entry Inno can track
; for removal as a unit (it's expanded file-by-file above) - explicitly
; remove the whole bundle folder on uninstall so no empty/partial
; "UNI 76.vst3" directory is left behind. This only ever targets our own
; named bundle folder, never anything else under the shared VST3 path.
Type: filesandordirs; Name: "{commoncf64}\VST3\{#MyVst3BundleName}"

[Code]
// WebView2 Runtime presence check (RC1: detect + inform only - this
// installer does NOT silently download or execute Microsoft's Evergreen
// Bootstrapper on the user's behalf; see docs/RC1 report and
// Packaging/Windows/README.md for the reasoning). Detection follows
// Microsoft's own documented method: the Evergreen Runtime writes its
// version string to a "pv" registry value under a fixed client GUID,
// under both the native and WOW6432Node hives depending on OS/runtime
// bitness.
function IsWebView2RuntimeInstalled(): Boolean;
var
  Pv: String;
  Found: Boolean;
begin
  Found := False;

  if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Pv) then
    if (Pv <> '') and (Pv <> '0.0.0.0') then
      Found := True;

  if not Found then
    if RegQueryStringValue(HKLM32, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Pv) then
      if (Pv <> '') and (Pv <> '0.0.0.0') then
        Found := True;

  // Per-user install of the Evergreen Runtime (HKCU) is also valid.
  if not Found then
    if RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Pv) then
      if (Pv <> '') and (Pv <> '0.0.0.0') then
        Found := True;

  Result := Found;
end;

procedure InitializeWizard();
begin
  if not IsWebView2RuntimeInstalled() then
  begin
    MsgBox(
      'UNI 76''s user interface requires the Microsoft Edge WebView2 Runtime, ' + #13#10 +
      'which was not detected on this system.' + #13#10#13#10 +
      'Most current Windows 10/11 systems already have it as a shared ' + #13#10 +
      'system component. If UNI 76''s interface fails to display after ' + #13#10 +
      'installing, download the "Evergreen Bootstrapper" from Microsoft:' + #13#10#13#10 +
      'https://developer.microsoft.com/microsoft-edge/webview2/' + #13#10#13#10 +
      'Setup will continue - this is informational only.',
      mbInformation, MB_OK);
  end;
end;

[Messages]
; Nostalgia Audio branding note shown at the top of the wizard's welcome
; page - Inno's default text already covers install location/licence,
; this just clarifies what's being installed at a glance.
WelcomeLabel2=This will install [name/ver] on your computer, as a VST3 plug-in for use in a compatible DAW.%n%nThis is an internal Release Candidate build - not for public distribution.
