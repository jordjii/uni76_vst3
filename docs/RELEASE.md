# UNI 76 - Release

This document is a checklist for future releases. Nothing on it has been
done yet - this stage of the project is a technical foundation only,
see the top-level report and [CLAUDE.md](../CLAUDE.md) for current status.

## Before the first public release

- [ ] **JUCE commercial license.** UNI 76 is being fetched against open
      JUCE under AGPLv3 at this stage. A closed-source commercial plugin
      requires a paid JUCE license before public release. This also gates
      disabling the JUCE splash screen
      (`JUCE_DISPLAY_SPLASH_SCREEN`, currently left at its default/shown
      state in `Source/Plugin/CMakeLists.txt` - do not flip this without a
      commercial license in place).
- [ ] **Freeze plugin identity.** `cmake/PluginIdentity.cmake` values
      (`BUNDLE_ID`, `PLUGIN_MANUFACTURER_CODE`, `PLUGIN_CODE`) become
      permanent the moment UNI 76 is used by a real host/DAW session and
      saved into a project file. Confirm them one more time before the
      first release build that leaves this machine.
- [ ] **Windows code signing.** Obtain an Authenticode certificate; wire
      signing into the build/installer pipeline. See
      [Packaging/Windows/README.md](../Packaging/Windows/README.md).
- [ ] **macOS signing & notarization.** Obtain a Developer ID
      Application + Developer ID Installer certificate; fill in
      `Packaging/macOS/build-pkg.sh` (currently a documented skeleton that
      refuses to run without real credentials). See
      [Packaging/macOS/README.md](../Packaging/macOS/README.md).
- [ ] **WebView2 Runtime bootstrapping.** Finish the Windows installer's
      runtime-presence check + Evergreen Bootstrapper fallback (TODO in
      `Packaging/Windows/UNI76.iss`).
- [ ] **Real installers, not skeletons.** Both `Packaging/Windows/UNI76.iss`
      and `Packaging/macOS/build-pkg.sh` need their TODOs resolved and need
      to be run end-to-end against a real Release build before being
      called a release pipeline.
- [ ] **pluginval / AudioPluginHost pass.** Not run at this stage (see
      docs/BUILD.md) - run one before shipping.

## State/preset schema migrations

`Source/Core/PluginIdentity.h` defines `uni76::stateSchemaVersion`. When a
future change alters what's stored in the saved `AudioProcessorValueTreeState`
in a way that isn't automatically forward-compatible (e.g. a parameter is
renamed, restructured, or its meaning changes), bump this constant and add
explicit migration logic in
`UNI76AudioProcessor::setStateInformation` (`Source/Plugin/PluginProcessor.cpp`)
keyed off the old value read from the loaded state. Do not silently drop or
reinterpret old presets.

## Explicitly out of scope until DSP work begins

Per the current project stage, none of the following exist yet and should
not be started without a separate go-ahead: saturation/pitch/EQ/reverb/
imager/preamp DSP, final visual design, a presets browser, copy protection,
or a licensing system.
