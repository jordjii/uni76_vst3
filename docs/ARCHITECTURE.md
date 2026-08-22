# UNI 76 - Architecture

Status: technical foundation + production UI + PREAMP DSP (frozen after a
sound-calibration pass - see [docs/DSP_PREAMP.md](DSP_PREAMP.md)) + EQ DSP
(see [docs/DSP_EQ.md](DSP_EQ.md)). Saturation, Pitch, Panorama, Reverb and
Imager remain a strict passthrough. See root [CLAUDE.md](../CLAUDE.md) for
the living project log and the rules this architecture exists to enforce.

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
  DSP/          PreampProcessor + EqProcessor (real DSP, chained in that
                order - see docs/DSP_PREAMP.md / docs/DSP_EQ.md) plus
                architectural placeholders for the other 5 processing
                modules (Saturation, Pitch, Panorama, Reverb, Imager).
                See "DSP modules" below.
  UI/           The WebView editor and the C++ <-> JS bridge
                (WebSliderRelay / WebSliderParameterAttachment wiring,
                resource provider for the embedded HTML/CSS/JS).

Resources/Web/  The production HTML/CSS/JS UI. Compiled into the binary via
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
- **There is no `Source/Utils/` right now.** An earlier pass added one
  generic helper there; the UI rewrite stopped needing it, and it was
  removed rather than left as an unused file. Recreate the folder when a
  real cross-cutting helper shows up - don't pre-populate it speculatively.

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

`Source/DSP/PreampProcessor.h`/`.cpp` is the first module with real audio
processing - see [docs/DSP_PREAMP.md](DSP_PREAMP.md) for its full signal
chain, oversampling strategy, curves, and measured harmonic/frequency
data. It's built from three pieces:

- `PreampCurves.h` - pure, stateless functions mapping the `preamp`
  parameter (0..1) onto every drive-dependent shape (waveshaper drive,
  asymmetry, Low Cut/High Cut, output trim, ...). One place to look up "what
  does the curve do at t=0.5", directly unit-testable.
- `Biquad.h` - a small allocation-free biquad/one-pole/DC-blocker/
  integer-delay-line toolkit. Deliberately not `juce::dsp::IIR::Filter`:
  that class's `Coefficients` are heap-allocated and its `makeLowPass()`-
  style factories allocate on every call, which `PreampProcessor` cannot
  afford since it recomputes filter shapes every block.
- `PreampProcessor.h`/`.cpp` - the actual engine: `juce::dsp::Oversampling`
  around the nonlinear stage only, the filter chain, smoothing, and the
  enable/disable crossfade.

`Source/DSP/EqProcessor.h`/`.cpp` is the second - see
[docs/DSP_EQ.md](DSP_EQ.md) for its morph curves and measured frequency
response. Chained after PREAMP in `PluginProcessor::processBlock()`. Built
from `EqCurves.h` (the DARK/PHONE/AIR anchors and the smoothstepped morph
function) and `EqProcessor.h`/`.cpp` (a fixed five-stage minimum-phase
filter network - HP, low shelf, bell, high shelf, LP - reusing the same
`Biquad.h` toolkit PREAMP uses). No oversampling (adds zero latency), no
nonlinearity.

`Source/DSP/Saturation.h`/`Pitch.h`/`Panorama.h`/`Reverb.h`/`Imager.h`
remain deliberately empty placeholder classes - no `prepare`/`process`
methods, no fake processing - and are not referenced from
`PluginProcessor`. Wiring one in means giving it real behaviour like
PREAMP/EQ got, not just calling an empty stub.

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

The editor is resizable within a fixed 3:2 aspect ratio (600x400 min /
960x640 default / 1350x900 max - `setResizeLimits` +
`getConstrainer()->setFixedAspectRatio()` in `WebUIEditor.cpp`); the CSS
itself is unit-relative (`clamp()`/`vw`/`rem`) so it reflows to fill
whatever size within that range the host allows.

### Frontend structure

`Resources/Web/` is deliberately split by concern rather than one
monolithic file:

```
index.html          Markup: header, 7 module <section>s, footer.

tokens.css           Every colour/spacing/typography/knob-geometry value,
                      as CSS custom properties - nothing else hard-codes one.
reset.css             Minimal base reset.
shell.css              App grid (header/main/footer) + the CSS-only grain overlay.
header.css              Header bar (brand / product title / placeholder controls).
modules.css               Per-module panel chrome (accent bar, index, name, tag).
knobs.css                  The knob itself - see below.
scales.css                  Tri-point scales + the Preamp filter-line indicators.
meters.css                   Footer I/O meters + signal-path readout.
responsive.css                 Small guards for the extreme ends of the resize range.

juce_webview.js       Vendored, unmodified JUCE frontend bridge module.
knob.js                Reusable ParameterKnob class: interaction + ARIA only.
aux_visuals.js           Derived-indicator positioning (tri-scale marker,
                          Preamp filter lines) - reads a value, writes a
                          CSS custom property, never touches a parameter.
app.js                    Bootstrap: constructs one ParameterKnob per module
                          and wires its onChange callback to the matching
                          aux_visuals updater, if that module has one.
```

### CSS knobs

Each `.knob` is plain DOM (`.knob__ticks` with 11 generated `.knob__tick`
children, `.knob__body`, `.knob__indicator`) - no image, SVG, or canvas.
Both the static tick marks and the live indicator use the same "rotated
clock hand" technique: an invisible box exactly the size of the knob is
rotated around its own centre (`--tick-angle` for ticks, `--knob-angle` for
the live value), and a short mark pinned to that box's top edge sweeps
around the circle as a result. `knob.js` only ever writes `--knob-angle`
(and delegates ARIA `aria-valuenow`/`aria-valuetext`) - every other visual
detail is CSS.

`ParameterKnob` (`knob.js`) owns interaction, not rendering: pointer
drag (with `setPointerCapture`, Shift for fine adjustment), mouse wheel
(debounced into begin/end gesture pairs), double-click reset to 50%,
arrow-key nudging (held key = one continuous gesture), and the
`sliderDragStarted()`/`sliderDragEnded()` calls that tell the host where a
DAW-automation gesture begins and ends. Rendering only happens from the
relay's `valueChangedEvent` - the knob deliberately does *not* render its
own optimistic guess of the value on construction, so it can never flash
an incorrect position before the real parameter value arrives from the
backend (see the comment in `ParameterKnob`'s constructor).

### Derived (non-parameter) visual indicators

The tri-point scales on Saturation/Pitch/Panorama/Reverb/Imager
(`aux_visuals.js`) are illustrative read-outs computed from the *existing*
parameter's value - those 5 modules are still passthrough, so these don't
imply DSP behaviour that isn't implemented. They do not create, read, or
write any additional APVTS parameter.

EQ's tri-point scale is different in status (it's now real DSP) but not in
implementation: `DARK`/`PHONE`/`AIR` are the three genuinely correct named
positions at 0%/50%/100% already, so the existing generic marker binding
(`bindTriScale` - a plain 0..100% position, no per-module formula) needed
no change when EQ's DSP landed, unlike Preamp's filter-line indicators
below.

The Preamp module's "LOW CUT" / "HIGH CUT" lines are different: they now
track the *real* drive-dependent filters `Source/DSP/PreampProcessor`
applies (`preampLowCutHz`/`preampHighCutHz` in `PreampCurves.h`, see
[docs/DSP_PREAMP.md](DSP_PREAMP.md)). `aux_visuals.js`'s copy of that
formula is a hand-mirrored literal, not a live binding (there's no shared
runtime between the plugin core and the WebView UI) - if the DSP curve
changes, update both places together.
