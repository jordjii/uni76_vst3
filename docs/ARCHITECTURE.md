# UNI 76 - Architecture

Status: technical foundation + production UI + PREAMP DSP (frozen after a
sound-calibration pass - see [docs/DSP_PREAMP.md](DSP_PREAMP.md)) + EQ DSP
(see [docs/DSP_EQ.md](DSP_EQ.md)) + SAT DSP (see
[docs/DSP_SAT.md](DSP_SAT.md)) + PITCH DSP (see
[docs/DSP_PITCH.md](DSP_PITCH.md)) + PAN DSP (see
[docs/DSP_PAN.md](DSP_PAN.md)) + VERB DSP (see
[docs/DSP_VERB.md](DSP_VERB.md)) + IMAGE DSP (see
[docs/DSP_IMAGE.md](DSP_IMAGE.md)). All 7 modules now have real DSP.
See root [CLAUDE.md](../CLAUDE.md) for the living project log and the
rules this architecture exists to enforce.

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
  DSP/          PreampProcessor + EqProcessor + SatProcessor + PitchProcessor
                + PanoramaProcessor + VerbProcessor + ImagerProcessor (real
                DSP, chained in that order - see docs/DSP_PREAMP.md /
                docs/DSP_EQ.md / docs/DSP_SAT.md / docs/DSP_PITCH.md /
                docs/DSP_PAN.md / docs/DSP_VERB.md / docs/DSP_IMAGE.md).
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

ThirdParty/     Vendored third-party DSP dependencies, exact pinned-commit
                source copied in directly (not FetchContent) - currently
                Signalsmith Stretch + Signalsmith Linear (both MIT), used
                only by PitchProcessor. See docs/DSP_PITCH.md's "Dependency
                and license" section.
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

Exactly 7 public parameters, IDs centralised in
[`Source/Parameters/ParameterIDs.h`](../Source/Parameters/ParameterIDs.h).
Six are `AudioParameterFloat` in a `0..100` (%) range (`50%` default for
`eq`, `0%` for the rest, including `panorama`); `pitch` is the one
exception - a discrete `AudioParameterInt` (`-12..+12` semitones, step 1,
default 0) rather than a percentage, since PITCH's musically meaningful
values are integer semitones, not a continuous 0-100% range (see
[docs/DSP_PITCH.md](DSP_PITCH.md)'s "Parameter and state migration"
section for why, and how the old percent-based `pitch` state migrates).
`panorama` briefly defaulted to `50%` under an earlier, retired
MONO/NATURAL/WIDE contract; it is back to `0%` under the current
ORIGINAL/WIDE/MOTION contract - a fresh instance's stereo field is
untouched, matching every other non-EQ module (see
[docs/DSP_PAN.md](DSP_PAN.md)'s "Parameter and state migration" section
for the full schema history). Its ID is a naming holdover from before the
module's actual behaviour (a combined stereo width + slow L/R motion
control, not a simple L/R balance pan) was decided.
`Low Cut` / `High Cut` are deliberately *not* separate parameters - they
are internal implementation details of PreampProcessor, controlled
indirectly (if at all) rather than exposed as their own automation lanes.

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

`Source/DSP/SatProcessor.h`/`.cpp` is the third - see
[docs/DSP_SAT.md](DSP_SAT.md) for its nonlinear model and measured data.
Chained after EQ. Built from `SatCurves.h` (drive/asymmetry/compression/
tilt/output-trim curves) and `SatProcessor.h`/`.cpp` (its own independent
`juce::dsp::Oversampling` instance around a frequency-tilt pre-emphasis ->
envelope-driven dynamic gain -> bounded waveshaper -> tilt de-emphasis
chain). Reuses the bounded per-half-gain waveshaper structure PREAMP's
calibration pass proved safe, and `Biquad.h`'s `EnvelopeFollower`
(peak-hold envelope, added for this module) for its "memory" component -
otherwise a fully independent set of curves/constants from PREAMP.

`Source/DSP/PitchProcessor.h`/`.cpp` is the fourth - see
[docs/DSP_PITCH.md](DSP_PITCH.md) for algorithm selection, the
configuration benchmark, and measured bass-stability/sideband/latency
data. Chained after SAT. Unlike the other three modules, it doesn't build
its own filter network from `Biquad.h` - it wraps two fully independent
mono instances of the vendored Signalsmith Stretch engine
(`ThirdParty/signalsmith-stretch/`, MIT-licensed, exact pinned commit),
one per channel rather than one shared multi-channel instance, so that
bit-identical stereo input is guaranteed by construction to produce
bit-identical stereo output. The vendored engine is kept behind a PIMPL
(`PitchProcessor::Engine`, defined only in the `.cpp`) so `PitchProcessor.h`
itself stays free of `ThirdParty/` include paths. `PitchCurves.h` holds
just the tuned STFT block/interval constants (seconds, not samples - see
`prepare()`) and the bypass-crossfade smoothing time. Reuses `Biquad.h`'s
`IntegerDelayLine` for the same latency-aligned bypass crossfade pattern
PREAMP/SAT use.

`Source/DSP/PanoramaProcessor.h`/`.cpp` is the fifth - see
[docs/DSP_PAN.md](DSP_PAN.md) for the full topology and measured data.
Chained after PITCH. Despite the `panorama` parameter ID (a naming
holdover, never renamed), it's a combined Mid/Side stereo-*width* AND
slow deterministic L/R *motion* control (ORIGINAL/WIDE/MOTION), not a
simple L/R balance pan and not a naive whole-signal auto-pan. Built from
`PanoramaCurves.h` (width, motion-depth and induced-signal-blend curves,
each provably identity/zero at 0%) and `PanoramaProcessor.h`/`.cpp` (the
Mid/Side matrix, a `Biquad.h` allpass deriving a phase-decorrelated
"induced" reference from Mid for mono-source spatialisation, a pair of
`OnePoleLowPass` filters keeping synthesised spatial energy and motion
out of the low band, a free-running LFO driving a constant-power L/R
rotation of the Side/spatial signal only - Mid itself is never rotated).
Unlike PREAMP/EQ/SAT/PITCH, needs no oversampling and adds no latency - a
pure per-sample gain/filter/rotation morph. Two properties are provable
from the topology rather than just measured: mono fold-down at 0% is
bit-identical to the input's own mono sum (Mid is read once and never
modified), and ORIGINAL (0%) is a true identity transform - every curve
in `PanoramaCurves.h` evaluates to its neutral value (width gain 1.0,
motion depth 0.0, induced blend 0.0) at t=0 by construction, so the
motion rotation collapses to gainL==gainR==1.0 and the LFO's own phase
never reaches the output.

`Source/DSP/VerbProcessor.h`/`.cpp` is the sixth - see
[docs/DSP_VERB.md](DSP_VERB.md) for the full topology and measured data.
Chained after PAN. A single 1970s-style electromechanical plate reverb +
analog send/return electronics - not a generic digital hall, not a
ROOM/PLATE/CHAMBER morph. Dry is read into locals and written back
unmodified in the same per-sample loop iteration, never passing through
any filter/delay/nonlinearity in the file - the wet contribution is
purely additive (an aux-send level, not a crossfade), scaled by
`VerbCurves.h`'s `verbWetGain(t)` (exactly 0.0 at t=0) and the bypass
smoother, which is what makes "dry never touched" an algebraic
guarantee and DRY (0%) a provable identity. The wet path is a 350Hz
cascaded 4-pole Butterworth highpass, a tiny asymmetric-tanh analog
send stage, a 4-stage short-delay Schroeder-allpass diffuser (early
density - not used alone as the whole reverb, which would be the
rejected "cheap Schroeder" architecture), a smoothly-variable pre-delay,
a 12-line FDN plate tank (Householder feedback matrix - an orthogonal,
energy-preserving mix computable in O(N) per sample - with per-line
`OnePoleLowPass` damping so highs decay faster than mid, fed from a
single mono sum and read out via two independent fixed sign patterns
for decorrelated stereo width), an analog return stage (tiny tanh +
soft bandwidth ceiling), and a second, lighter 350Hz safety highpass on
the wet output. Adds no latency (pre-delay/tank recirculation are
wet-path effects, not a lookahead on the direct signal).

`Source/DSP/ImagerProcessor.*` implements `07 IMAGE / STEREO IMAGE` - see
docs/DSP_IMAGE.md for the full topology and measured data. The one
module with two independent public parameters (a deliberate exception to
the "one knob per module" rule): `imager` (frequency-dependent stereo
width - a single low-shelf reshaping Side only, low-frequency asymptote
shrinking toward centre as the macro rises, high-frequency asymptote
growing up to 2x) and `imageTilt` (a static L/R stereo balance/tilt - a
bounded constant-power gain pair applied to Mid only via two independent
per-channel low-shelf filters, reusing PAN's own proven-safe single-
shelf construction for frequency-dependent bass safety). Both axes are
Mid/Side-domain and orthogonal by construction, and both evaluate to
their own identity value at their own zero point - `imager=0%` and
`imageTilt=0/CENTER` together give a provable bit-exact identity. Adds
no latency (pure gain/filter morph, no oversampling, no delay-based
widening, no time-varying modulation).

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
(debounced into begin/end gesture pairs), double-click reset to default,
arrow-key nudging (held key = one continuous gesture), and the
`sliderDragStarted()`/`sliderDragEnded()` calls that tell the host where a
DAW-automation gesture begins and ends. Rendering only happens from the
relay's `valueChangedEvent` - the knob deliberately does *not* render its
own optimistic guess of the value on construction, so it can never flash
an incorrect position before the real parameter value arrives from the
backend (see the comment in `ParameterKnob`'s constructor).

PITCH's knob is the one exception to the otherwise-continuous 0-100%
interaction model: `app.js` passes a `discrete: { steps, ariaMin, ariaMax,
ariaStep }` object (25 steps, -12..+12, step 1) that `ParameterKnob`
threads through `_snapToStep()`/`_stepFromCurrent()` - drag/wheel/keyboard
each move exactly one semitone, Shift fine-control is skipped (no
fractional semitones exist), double-click resets to 0 ST, and ARIA reports
real semitone min/max/step/value instead of a 0-100 percent range. Every
other module passes no `discrete` option and falls through to the original
continuous behaviour unchanged.

### Derived (non-parameter) visual indicators

IMAGE's tri-point scale (`ORIGINAL`/`FOCUS`/`WIDE`, `aux_visuals.js`)
is a read-out derived purely from the `imager` parameter's value - it
uses the same generic `bindTriScale` binding as EQ/SAT/PITCH/PAN/VERB's
own tri-scales, does not create, read, or write any additional APVTS
parameter, and does not represent `imageTilt` at all (see below). IMAGE
is the one module with a *second*, genuinely interactive control living
alongside its tri-scale in the same aux zone: a square spatial field pad
(`Resources/Web/field_pad.js`) whose X axis drives `imageTilt` and whose
Y axis drives `imager` itself, bound directly to both parameters via the
same `getSliderState`/relay bridge every main knob uses - not a derived
visual, a real second control (in fact a second way to set `imager`
too, kept in sync with the main knob through the shared SliderState
singleton), wired up in `app.js`'s dedicated `initImageField()` (kept
separate from the generic `initModule()`/`MODULES` loop, which is built
around exactly one control per module). See docs/DSP_IMAGE.md's "UI:
spatial field pad" section.

EQ's, SAT's, PITCH's and PAN's tri-point scales are different in status
(all four are now real DSP) but not in implementation: `DARK`/`PHONE`/
`AIR`, `CLEAN`/`WARM`/`HOT`, `- OCT`/`0`/`+ OCT` and `ORIGINAL`/`WIDE`/
`MOTION` are each the three genuinely correct named positions at 0%/50%/
100% already, so the existing generic marker binding (`bindTriScale` - a
plain 0..100% position, no per-module formula) needed no change when any
of the four modules' DSP landed, unlike Preamp's filter-line indicators
below. PITCH's `onChange` callback does pass a converted value though:
`bindTriScale` expects a 0..100 percent, so `app.js` always forwards
`normalised * 100` (the knob's position within its own range) rather
than the module's real-units `scaled` value, which for PITCH is
semitones, not a percentage. PAN needed no such conversion - it stayed a
plain `AudioParameterFloat` (its default returned to 0% under the
current ORIGINAL/WIDE/MOTION contract - see "Parameters" above and
[docs/DSP_PAN.md](DSP_PAN.md)), so `scaled` and `normalised*100` are
already numerically identical for it, same as every other non-PITCH
module.

The Preamp module's "LOW CUT" / "HIGH CUT" lines are different: they now
track the *real* drive-dependent filters `Source/DSP/PreampProcessor`
applies (`preampLowCutHz`/`preampHighCutHz` in `PreampCurves.h`, see
[docs/DSP_PREAMP.md](DSP_PREAMP.md)). `aux_visuals.js`'s copy of that
formula is a hand-mirrored literal, not a live binding (there's no shared
runtime between the plugin core and the WebView UI) - if the DSP curve
changes, update both places together.
