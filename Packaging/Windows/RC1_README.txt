UNI 76 (Nostalgia Audio) - Windows x64 - Internal Release Candidate 1
========================================================================

This is an INTERNAL Release Candidate build - not a public release.
Do not redistribute this installer outside the development team.

What was installed:
  - The UNI 76 VST3 plugin, at the standard Windows location:
      C:\Program Files\Common Files\VST3\UNI 76.vst3
  - This README, an uninstaller, and THIRD_PARTY_NOTICES.txt, at:
      C:\Program Files\Nostalgia Audio\UNI 76\

Requirements:
  - Windows 10 or 11, 64-bit.
  - A VST3-compatible DAW.
  - Microsoft Edge WebView2 Runtime (the plugin's user interface depends
    on it). Most current Windows 10/11 systems already have it as a
    shared system component; Setup checked for it and will have shown a
    message if it could not be found. See THIRD_PARTY_NOTICES.txt.

Known RC1 limitations (see docs/FULL_DSP_AUDIT.md for full detail):
  - Not code-signed. Windows SmartScreen may warn on first run of the
    installer - this is expected for an unsigned internal build.
  - pluginval (a third-party VST3 validator) does not currently complete
    against this plugin in the development environment; this is a
    documented, investigated validator-specific finding (destruction
    off the host's message thread), not a defect reachable through
    normal DAW usage - see docs/FULL_DSP_AUDIT.md section 36.
  - macOS is not built or tested yet.

To uninstall: use Windows Settings -> Apps, or the uninstaller in
C:\Program Files\Nostalgia Audio\UNI 76\. This removes only UNI 76's own
files (the VST3 bundle and this product folder) - no other plugins in
the shared VST3 folder are touched.

Support / feedback: report issues to the development team directly -
this build has no public support channel.
