; ==============================================================================
;  UNI 76 (Nostalgia Audio) - Inno Setup installer SKELETON.
;
;  This is a foundation, not a working production installer: it establishes
;  the shape of the script (identity, install location, WebView2 runtime
;  check) so the real installer can be filled in later without redesigning
;  the packaging approach. Do not treat a successful compile of this script
;  as a validated release installer - see docs/RELEASE.md.
;
;  Keep AppName / AppPublisher / AppVersion in sync with
;  cmake/PluginIdentity.cmake by hand; Inno Setup cannot include a CMake
;  file directly.
; ==============================================================================

#define MyAppName "UNI 76"
#define MyAppPublisher "Nostalgia Audio"
#define MyAppVersion "0.1.0"
#define MyAppURL "https://nostalgiaaudio.com"

[Setup]
; TODO before first public release: generate a real GUID (Inno Setup ->
; Tools -> Generate GUID) and never change it again afterwards - it is how
; Windows identifies upgrades to this installer.
AppId={{00000000-0000-0000-0000-000000000000}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=UNI76-{#MyAppVersion}-Setup
Compression=lzma
SolidCompression=yes

[Files]
; Not wired up yet - this stage of the project ships no release artefact.
; Once a Release build pipeline exists, point this at the built .vst3:
; Source: "..\..\build\windows-release\Source\Plugin\UNI76_artefacts\Release\VST3\UNI 76.vst3\*"; \
;   DestDir: "{app}\UNI 76.vst3"; Flags: recursesubdirs ignoreversion

[Code]
// TODO: check for the WebView2 Runtime before/after install and offer the
// Evergreen bootstrapper if it's missing. Detection is normally done by
// checking for the registry value described at:
//   HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\
//     {F3017226-FE2A-4295-8BDF-00C3A9A7E4C5} -> pv
// (also check the non-WOW6432Node key on native x64). If absent, the
// installer should offer to download and run the Evergreen Bootstrapper
// from Microsoft (MicrosoftEdgeWebview2Setup.exe) - not implemented here.
// See docs/BUILD.md, "WebView2 Runtime" section, for the full strategy.
