# UNI 76 - Architecture

Status: foundation stage. No DSP, no final UI design. See root
[CLAUDE.md](../CLAUDE.md) for the living project log and the rules this
architecture exists to enforce.

## Layers

```
Source/
  Plugin/       AudioProcessor entry point + CMake target definition.
                Wires everything else together; contains no DSP or UI logic
                itself.
  Core/         Non-DSP, non-UI identity/constants shared across the
                codebase (e.g. state schema version).
  Parameters/   Centralised parameter IDs and the APVTS parameter layout.
                The only place parameter defaults/ranges are defined.
  DSP/          Architectural placeholders for the 7 processing modules
                (Preamp, Eq, Saturation, Pitch, Panorama, Reverb, Imager).
                Empty at this stage - see "DSP modules" below.
  UI/           The WebView editor and the C++ <-> JS bridge
                (WebSliderRelay / WebSliderParameterAttachment wiring,
                resource provider for the embedded HTML/CSS/JS).
  Utils/        Small generic helpers with no audio or UI dependencies of
                their own (e.g. reading JucePlugin_* build info).

Resources/Web/  The HTML/CSS/JS diagnostic UI. Compiled into the binary via
                juce_add_binary_data() - never read from disk at runtime.

Tests/          juce::UnitTest-based automated checks (see docs/BUILD.md).

Packaging/      Windows (Inno Setup) and macOS (pkgbuild/notarytool)
                installer foundations. Not production-ready yet.

cmake/          PluginIdentity.cmake - the single source of truth for
                company/bundle/plugin-code identity.
```

## Why this split

- **Parameters is separate from DSP** because the 7 public parameters exist
  and are host-automatable *before* any DSP that reads them exists. Keeping
  them independent means the DSP modules can be built later without
  touching parameter identity, and vice versa.
- **UI only depends on Plugin + Parameters**, never on DSP. The web UI reads
  parameter values through the relay/attachment bridge, not by reaching
  into DSP internals.
- **Core is intentionally tiny.** It exists so cross-cutting non-DSP,
  non-UI facts (right now: just the state schema version) have exactly one
  home, rather than being duplicated or guessed at in Plugin/UI/Tests.

## Plugin identity

Company name, bundle ID, and the two 4-character plugin codes
(`PLUGIN_MANUFACTURER_CODE` / `PLUGIN_CODE`) live in
[`cmake/PluginIdentity.cmake`](../cmake/PluginIdentity.cmake) and nowhere
else. They are **immutable after the first public release** - a host
identifies a previously-seen plugin (and its saved state/automation) by
these values. See CLAUDE.md for the current fixed values.

## Parameters

Exactly 7 public parameters, all `AudioParameterFloat` in a `0..100` (%)
range with a `50%` default, IDs centralised in
[`Source/Parameters/ParameterIDs.h`](../Source/Parameters/ParameterIDs.h).
`Low Cut` / `High Cut` are deliberately *not* separate parameters - they
will become internal implementation details of the future Preamp DSP
module, controlled indirectly (if at all) rather than exposed as their own
automation lanes.

State is saved/restored through `AudioProcessorValueTreeState`
(`getStateInformation` / `setStateInformation` in
[`PluginProcessor.cpp`](../Source/Plugin/PluginProcessor.cpp)), tagged with
a schema version (`Source/Core/PluginIdentity.h`) so future preset formats
can migrate old saves instead of silently misreading them.

## DSP modules

`Source/DSP/*.h` contain one placeholder class per future module
(`Preamp`, `Eq`, `Saturation`, `Pitch`, `Panorama`, `Reverb`, `Imager`).
They are deliberately empty - no `prepare`/`process` methods, no fake
processing - and are not referenced from `PluginProcessor` yet.
`processBlock()` is a strict passthrough (see the realtime rules in
CLAUDE.md). Wiring a module in means giving it real behaviour, not just
calling an empty stub.

## Web UI / native bridge

The editor (`Source/UI/WebUIEditor.*`) hosts a `juce::WebBrowserComponent`
configured for the WebView2 backend on Windows (falling back automatically
to the platform default elsewhere) and serves `Resources/Web/*` from
compiled-in `BinaryData` via a `WebBrowserComponent::Resource` provider
(`Source/UI/WebResourceProvider.*`).

Each of the 7 parameters gets one `juce::WebSliderRelay` (named after its
parameter ID) plus one `juce::WebSliderParameterAttachment`, JUCE's own
mechanism for keeping a JS-side control and a `RangedAudioParameter` in
sync in both directions - not a hand-rolled polling protocol. The frontend
side of that bridge is `Resources/Web/juce_webview.js`, an unmodified local
copy of JUCE's own `@juce-framework/webview` frontend module (see its
header for the JUCE license terms it's distributed under).

The `SinglePageBrowser` subclass overrides `pageAboutToLoad` to allow only
the plugin's own embedded resource root - the frontend cannot navigate to
an external site.

This is a technical prototype UI ("TECHNICAL UI PROTOTYPE" is literally
printed in it) - not the final UNI 76 design.
