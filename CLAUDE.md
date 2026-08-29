# UNI 76 - project journal

This file is the living technical log for UNI 76. Keep it up to date as the
project moves forward; it is the fastest way for anyone (human or AI) to
get oriented without re-reading the whole codebase.

## What this is

- **Brand**: Nostalgia Audio
- **Product**: UNI 76
- **Purpose**: a universal vintage/analogue-style audio effect processor
  (not a synth).
- **Platforms**: Windows x64, macOS (Intel x86_64 + Apple Silicon arm64,
  built as a Universal Binary).
- **Format policy**: VST3 only. Do not add AU, AAX, VST2, LV2, or
  Standalone to the product build without a deliberate, separate decision -
  see `FORMATS` in `Source/Plugin/CMakeLists.txt`.

## Build architecture

- **CMake is the single source of truth.** Projucer is not used and must
  not be reintroduced.
- **JUCE 9**, fetched via `FetchContent` pinned to a specific commit (see
  `cmake/PluginIdentity.cmake`, `UNI76_JUCE_GIT_TAG`) for reproducible
  builds. Do not switch this to a floating branch.
- **AudioProcessorValueTreeState** is the parameter system.
  `juce::WebBrowserComponent` (+ `WebSliderRelay` /
  `WebSliderParameterAttachment`) is the UI bridge. Windows uses the
  WebView2 backend (statically-linked loader); other platforms fall back to
  their native WebView automatically.
- See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the folder-by-folder
  breakdown and [docs/BUILD.md](docs/BUILD.md) for exact build commands.

## HTML/CSS/JS UI rule

- **HTML/CSS-first.** The GUI is built entirely in HTML + CSS + vanilla
  JavaScript (`Resources/Web/`), compiled into the plugin binary as
  `BinaryData` - never loaded from disk at runtime.
- **No raster UI assets.** No PNG/JPG, no SVG illustrations, no Canvas
  drawing, no CDN, no Google Fonts/external fonts, no external JS
  libraries or frameworks, no 3D/photoreal rendering. Knobs, ticks, scales
  and meters are all real DOM elements positioned/rotated with CSS custom
  properties and `transform` - see `Resources/Web/knobs.css` +
  `Resources/Web/knob.js`. The only permitted decorative effect beyond flat
  colour is very low-opacity CSS-generated grain/gradient (see the
  `.app::after` rule in `shell.css`) - never anything that reads as 3D.
- **One primary knob per processor - with one deliberate exception.**
  Each of the 7 modules gets exactly one large interactive knob bound to
  its one public parameter. Any secondary read-out (the Preamp
  filter-position lines, the tri-point scales on EQ/Pitch/Pan/Verb/Image)
  is purely a derived visual computed from that same parameter's value in
  JS - never a second control, never a second parameter. PAN's tri-scale
  (`ORIGINAL`/`WIDE`/`MOTION`) is a stereo width + slow ear-to-ear motion
  scale, not L/R balance - see the PAN section below. **IMAGE is the one
  exception**: alongside its main `imager` knob (width/imaging amount,
  same "one knob, one parameter" pattern as every other module), it also
  carries a second, small, genuinely interactive control - a square
  spatial field pad (`Resources/Web/field_pad.js`) whose X axis drives
  `imageTilt` and whose Y axis drives `imager` itself (kept in sync with
  the main knob via the shared JUCE SliderState singleton - see the
  IMAGE section below and docs/DSP_IMAGE.md). This is a real second
  control, unlike every other module's purely-derived tri-scale/filter-
  lines, and it does not extend to any other module without a separate,
  deliberate decision.
- **8 immutable public parameters.** See below - the UI must never grow a
  9th public parameter without a deliberate, separate decision (the
  IMAGE/`imageTilt` addition above was exactly such a decision, already
  made and shipped).
- No network requests from the frontend, ever. The one vendored exception
  to "no external JS" is `Resources/Web/juce_webview.js`, an unmodified
  local copy of JUCE's own `@juce-framework/webview` frontend module -
  it's part of the framework's own native bridge mechanism, not a
  third-party dependency.
- CSS is split by concern (`tokens` / `reset` / `shell` / `header` /
  `modules` / `knobs` / `scales` / `meters` / `responsive`), all sized in
  relative units (`rem`, `vw`, `clamp()`) rather than fixed pixels, so the
  UI scales with the editor instead of being pinned to a canvas size. See
  `Source/UI/WebUIEditor.cpp` for the resizable 3:2-locked window
  (600x400 min / 960x640 default / 1350x900 max) this is designed to fill.

## Immutable identity (do not change after first public release)

| | |
|---|---|
| Company | Nostalgia Audio |
| Product | UNI 76 |
| Bundle ID | `com.nostalgiaaudio.uni76` |
| Plugin manufacturer code | `Nsta` |
| Plugin code | `Uni6` |
| Version (current) | `0.1.0` |

Single source of truth: [`cmake/PluginIdentity.cmake`](cmake/PluginIdentity.cmake).
Do not duplicate these values by hand elsewhere - `include()` that file, or
read from `Source/Core/PluginIdentity.h` for the C++-only schema-version
constant.

### The 8 public parameters (stable IDs, do not rename)

`preamp`, `eq`, `saturation`, `pitch`, `panorama`, `reverb`, `imager`,
`imageTilt` - see
[`Source/Parameters/ParameterIDs.h`](Source/Parameters/ParameterIDs.h).
Six are `0..100%` `AudioParameterFloat`s: default `50%` for `eq` (its
centred/flat "PHONE" position), `0%` for the other five - including
`panorama` (fully off/ORIGINAL, matching a console where drive/
saturation/width/space/image all start at zero). `pitch` is a discrete
`AudioParameterInt`, `-12..+12` semitones, step 1, default `0` (25 fixed
positions, no cents - see [docs/DSP_PITCH.md](docs/DSP_PITCH.md)'s
"Parameter and state migration" section). `imageTilt` is the other
exception - a continuous `AudioParameterFloat`, `-100..+100`, default `0`
(CENTER, sitting at the range's exact midpoint, same normalised-midpoint
shape as `eq`'s 50% PHONE default and `pitch`'s 0 ST default) - a
deliberate, explicit exception to the "one parameter per module" rule
(see [docs/DSP_IMAGE.md](docs/DSP_IMAGE.md)): IMAGE alone has two
independent axes, `imager` (frequency-dependent width/imaging amount)
and `imageTilt` (a static L/R stereo balance/tilt on top of that same
image - not a pan). `panorama`'s internal ID is a naming holdover - the
module it drives is a stereo width + slow motion control, not an L/R
balance pan, see below. Low Cut / High Cut are **not** separate
parameters - they are internal to `Source/DSP/PreampProcessor`, derived
entirely from `preamp` (see [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md)).

## PREAMP + EQ + SAT + PITCH + PAN + VERB + IMAGE DSP (all seven modules now have real audio processing)

Chain order: `Input -> PREAMP -> EQ -> SAT -> PITCH -> PAN -> VERB ->
IMAGE -> Output` - `PluginProcessor::processBlock()` calls
`preampProcessor.process()`, then `eqProcessor.process()`, then
`satProcessor.process()`, then `pitchProcessor.process()`, then
`panoramaProcessor.process()`, then `verbProcessor.process()`, then
`imagerProcessor.process()` (reading both `imager` and `imageTilt`), in
that order. Total plugin latency is the **sum** of every stage's own
latency (`preampProcessor.getLatencySamples() + eqProcessor.getLatencySamples()
+ satProcessor.getLatencySamples() + pitchProcessor.getLatencySamples()
+ panoramaProcessor.getLatencySamples() + verbProcessor.getLatencySamples()
+ imagerProcessor.getLatencySamples()`, computed in `prepareToPlay`) -
EQ, PAN, VERB and IMAGE all always contribute 0; PITCH always contributes
a nonzero, sample-rate-proportional amount (unlike PREAMP/SAT, it does
not drop to 0 at high sample rates - see docs/DSP_PITCH.md's "Fixed
latency" section).

`Source/DSP/PreampProcessor.*` implements the `01 PREAMP / TRANSFORMER`
module's full signal chain - see
[docs/DSP_PREAMP.md](docs/DSP_PREAMP.md) for the topology, curves,
oversampling strategy, measured harmonic/frequency-response data, and
known tradeoffs in full. In short: DC/infrasonic protection, a
drive-dependent "transformer" coloration + asymmetric-gain tanh waveshaper
(oversampled 4x at 44.1/48kHz, 2x at 88.2/96kHz, none at 176.4kHz+),
drive-dependent soft Low Cut/High Cut, output compensation, and a
latency-aligned enable/disable crossfade driven by `preampEnabled`
(`Source/Core/ModuleEnableState.h`). **Frozen as of the sound-calibration
pass** - do not change PREAMP's DSP, constants, or gain staging without
a discovered objective regression.

`Source/DSP/EqProcessor.*` implements the `02 EQ / BAND SHAPER` module -
see [docs/DSP_EQ.md](docs/DSP_EQ.md) for the full topology, morph curves,
and measured frequency response. In short: one macro parameter (`eq`, the
TONE knob) morphs a fixed five-stage minimum-phase filter network (HP ->
low shelf -> bell/presence -> high shelf -> LP) continuously across two
linked regions - `DARK (0%) -> PHONE (50%)` and `PHONE (50%) -> AIR
(100%)` - sharing PHONE as a named centre, not a linear frequency sweep
and not three switched presets. No oversampling (adds zero latency), no
nonlinearity - PREAMP (and the future SAT module) own harmonic character;
EQ only shapes frequency response. Same `eqEnabled`-driven crossfade
bypass pattern as PREAMP, but without the delay-alignment PREAMP needs
(EQ has no latency to align against).

`Source/DSP/SatProcessor.*` implements the `03 SAT / ANALOG DRIVE`
module - see [docs/DSP_SAT.md](docs/DSP_SAT.md) for the full topology,
nonlinear model, and measured data. Deliberately **not** a second
PREAMP: SAT is an energy-dependent saturation/compression stage (a
peak-hold envelope follower drives a soft dynamic gain stage ahead of its
own, more aggressive waveshaper), surrounded by a frequency tilt
(low-shelf cut + high-shelf boost pre/de-emphasis pair) instead of
PREAMP's Low/High Cut - protects bass, gradually softens highs at high
HEAT. Reuses PREAMP's *proven-safe* bounded per-half-gain waveshaper
primitive with its own constants (not PREAMP's curve values), and owns
its own independent oversampling instance/latency (same 4x/2x/none
policy as PREAMP, same measured 6/6/4/4/0/0 samples across
44.1/48/88.2/96/176.4/192kHz) rather than a risky shared refactor of
PREAMP's. Same latency-aligned `saturationEnabled` crossfade bypass
pattern as PREAMP.

`Source/DSP/PitchProcessor.*` implements the `04 PITCH / VARISPEED`
module - see [docs/DSP_PITCH.md](docs/DSP_PITCH.md) for algorithm
selection, the configuration benchmark, dependency/license, and measured
bass-stability/sideband/latency/CPU data. Pure pitch-shift only, duration
always preserved (every `process()` call uses equal input/output sample
counts, so the engine only ever pitch-shifts, never time-stretches): two
fully independent mono instances of the vendored MIT-licensed Signalsmith
Stretch engine (`ThirdParty/signalsmith-stretch/`, exact pinned commit),
one per channel rather than one shared multi-channel instance, so
bit-identical stereo input is guaranteed by construction to produce
bit-identical stereo output. `pitch` is a discrete `AudioParameterInt`
(`-12..+12` semitones, step 1, default 0) rather than the other six
parameters' `0..100%` float - see the "7 public parameters" section
above and docs/DSP_PITCH.md's state-migration notes. STFT configured at
140ms block / 35ms interval (`Source/DSP/PitchCurves.h`), chosen from a
measured bass-stability benchmark over the library's own default preset
and several alternatives - latency is fixed (140ms at every sample rate,
independent of semitone value/enabled state/host block size) and does
**not** drop to zero at high sample rates the way PREAMP/SAT's
oversampling latency does. Same latency-aligned `pitchEnabled` crossfade
bypass pattern as PREAMP/SAT (`Biquad.h`'s `IntegerDelayLine`).

`Source/DSP/PanoramaProcessor.*` implements the `05 PAN / STEREO FIELD`
module - see [docs/DSP_PAN.md](docs/DSP_PAN.md) for the full topology and
measured data. Despite the `panorama` parameter ID (kept only for
compatibility, never renamed), **this is not an L/R balance pan** - it's
a combined stereo width + slow ear-to-ear motion control: `ORIGINAL (0%)
-> WIDE (50%) -> MOTION (100%)`. (An earlier revision briefly used a
`MONO (0%) <- NATURAL (50%) -> WIDE (100%)` width-only contract - see
docs/DSP_PAN.md's product-contract history note; that contract, its
default, and its migration are all superseded.) Mid (`0.5*(L+R)`) is
never modified - width/motion apply only to a separately-built "spatial"
signal (real Side content plus, for mono/near-mono sources, a phase-
decorrelated "induced" component derived from Mid via a single allpass,
its own bass content stripped by a cascaded 6th-order Butterworth
highpass before blending - not a delay, so no comb filtering/wow/
flutter/pitch drift), reshaped into L and R contributions by **two
independent per-channel RBJ low-shelf filters** (bass gets much smaller
width/motion ceilings than mid/high),
and applied to L/R via a constant-power rotation driven by a slow
(~0.3Hz), free-running, deterministic LFO whose phase is never reset by
a parameter change - a stable centre with the *surrounding* space
moving, not a global auto-pan. A follow-up pass replaced an earlier
(commit `b6f83ca`) band-split-then-differently-gained-sum crossover with
this shelf-per-channel design after finding and proving the split-based
approach had a genuine, mathematically-explained frequency-response bump
at its own crossover (a vector-sum of two phase-shifted complementary
bands weighted by different real gains, not a linear interpolation - see
docs/DSP_PAN.md's "Crossover artifact" section for the derivation); two
independent single-path shelves have no second, differently-gained path
to vector-sum against, so no bump is possible by construction, verified
by a dedicated frequency-response regression test (perfectly monotonic,
zero envelope violations, at every macro value). ORIGINAL (0%) is a
provable (not just measured) identity transform: every curve in
`PanoramaCurves.h` evaluates to its identity value there (width gain 1.0,
motion depth 0.0, induced blend 0.0), which makes both shelves' gain
collapse to an algebraically exact 0dB identity filter, so the whole
reconstruction collapses back to the unmodified input. No delay-based
widening (Haas), no chorus, no random modulation, no oversampling - zero
added latency. Same `panoramaEnabled`-driven crossfade bypass pattern as
EQ (no delay-alignment needed, PAN has no latency to align against).

`Source/DSP/VerbProcessor.*` implements the `06 VERB / VINTAGE SPACE`
module - see [docs/DSP_VERB.md](docs/DSP_VERB.md) for the full topology
and measured data. A single 1970s-style electromechanical **plate**
reverb + analog send/return electronics - not a generic digital hall,
not a ROOM/PLATE/CHAMBER morph, not a convolution IR: `DRY (0%) -> PLATE
(50%) -> DEEP (100%)`, always the *same* plate machine (only send
amount, decay time, and pre-delay change with the macro, never the
plate's own physical character). DRY is read into locals and written
back unmodified in the same per-sample loop iteration - it never passes
through any filter, delay line, or nonlinearity, making "dry is never
touched" an algebraic guarantee (measured RMS diff **0** on a broadband
source). The wet path: a 350Hz cascaded 4-pole Butterworth highpass on
the send, a tiny asymmetric-tanh analog send stage, a 4-stage short-
delay diffuser (early density - deliberately *not* used alone as the
whole reverb, which is the "cheap Schroeder" architecture the product
brief explicitly rejects), a smoothly-variable pre-delay, a 12-line FDN
plate tank (Householder feedback matrix - orthogonal/energy-preserving,
O(N) per sample - with per-line one-pole damping so highs decay faster
than mid, fed from a single mono sum and read out via two independent
fixed sign patterns for genuinely decorrelated stereo width, the way a
real plate's two pickups at different positions would), an analog return
stage (tiny tanh + ~9.5kHz soft bandwidth ceiling), and a second, lighter
350Hz safety highpass on the wet output (a recirculating feedback
network's own resonances aren't guaranteed to respect an input-side
filter alone). Measured: 40-120Hz wet content sits 60-92dB down (almost
no tail at all); 1kHz RT60 ~3.5s at 100% (5kHz and 8kHz measurably
shorter - highs decay faster than mid, by design); wet-path THD under 2%
at every macro setting (an earlier, more aggressive send/return
asymmetry setting measured 5-6%, since the dominant even-harmonic term
is driven by the tanh's *asymmetry*, not its drive gain - reducing
asymmetry specifically, not just drive, is what actually fixed it); a
hard safety clamp on any single delay line's feedback gain
(`verbLineFeedbackGainMax`) was added after a more aggressive damping-
filter tuning attempt pushed the shortest line's loop gain close enough
to instability to measurably distort the result. PAN=100+VERB=50
integration measured 80Hz bass L/R at 0.16dB (bass stays centred and
stable with the reverb layered on top); full-chain VERB0->VERB100 bass
change measured under 0.1dB at 60/80/100Hz. Zero added latency (pre-
delay/tank recirculation are wet-path effects, not a lookahead on the
direct signal), same `reverbEnabled`-driven bypass pattern as PAN
(mutes only the wet contribution, no delay-alignment needed).

`Source/DSP/ImagerProcessor.*` implements the `07 IMAGE / STEREO IMAGE`
module - see [docs/DSP_IMAGE.md](docs/DSP_IMAGE.md) for the full
topology and measured data. The one module with **two** independent
public parameters (a deliberate, explicit exception to the "one knob per
module" rule - see "HTML/CSS/JS UI rule" above): `imager` (0-100%,
frequency-dependent stereo width/imaging amount - `ORIGINAL(0%) ->
FOCUS -> WIDE(100%)`) and `imageTilt` (-100..+100, a static L/R stereo
image balance/tilt, default 0/CENTER - explicitly not a hard pan). Both
axes are Mid/Side-domain and orthogonal by construction: `imager` only
ever reshapes Side (a single low-shelf filter, low-frequency asymptote
shrinking toward centre as the macro rises for low-end centering/mono
compatibility, high-frequency asymptote growing up to 2x for a real
mastering-imager-style widen - Mid is never touched, so a mono source is
never stereoized by this axis alone); `imageTilt` only ever reshapes Mid
(a bounded, constant-power gain pair - `gL^2+gR^2==2` exactly, for any
tilt value, the same algebraic proof PAN's own motion rotation uses -
applied via two independent per-channel low-shelf filters whose
low-frequency asymptote is always exactly 1.0/no-tilt and whose
high-frequency asymptote is the full tilt gain, reusing PAN's own
proven-safe single-shelf-not-band-split-then-sum construction so no
frequency-response bump is possible by construction - Side is never
touched, so existing stereo width is always fully preserved regardless
of tilt). Both axes evaluate to their own identity value at their own
zero point, which is what makes all three of IMAGE=0/TILT=CENTER,
IMAGE>0/TILT=CENTER, and IMAGE=0/TILT!=CENTER provable (not just
measured) as the correct axis being the only one active. No oversampling,
no delay-based widening, no time-varying modulation (TILT is explicitly
static, unlike PAN) - zero added latency at every setting. UI: a second,
small square spatial field pad (`Resources/Web/field_pad.js`) lives
inside IMAGE's own panel alongside its main knob - horizontal drag is
`imageTilt`, vertical drag is `imager` itself, kept in sync with the
main knob via the shared JUCE SliderState singleton - fitting within a
dedicated height allowance in that one panel's aux zone (`modules.css`'s
`.module__aux--imager`) without any redesign of the other six modules'
panels. See docs/DSP_IMAGE.md's "UI: spatial field pad" section.

### Per-module enabled/disabled state (not a parameter)

Each module has a persistent on/off flag (`Source/Core/ModuleEnableState.h`)
that survives editor close/reopen and host state save/reload - but it is
**deliberately not an 8th-through-14th APVTS parameter**: it's not a DAW
automation target. It's bridged to the frontend via two small native
functions (`uni76SetModuleEnabled` / `uni76GetModuleEnabledStates`, see
`Source/UI/WebUIEditor.cpp`) rather than a `WebToggleRelay`, and persisted
as plain properties (`preampEnabled`, `eqEnabled`, ...) on the same saved
ValueTree as the APVTS parameters, not inside the parameter tree itself.
Defaults to enabled; a saved state from before this flag existed loads as
enabled=true for every module (see `stateSchemaVersion` in
`Source/Core/PluginIdentity.h`). Once a module's DSP exists, its
`processBlock()` work should check this flag - today, with DSP still
passthrough everywhere, disabling a module is a UI-only visual mute and
has no audio effect, and the UI must not imply otherwise.

## Realtime audio-thread rules

`AudioProcessor::processBlock()` must stay realtime-safe: **no allocations,
no locks, no file I/O, no calls into the WebView/GUI layer**. This applies
to every module's DSP once it exists, not just at the foundation stage -
`Source/DSP/PreampProcessor` allocates its oversampler, scratch buffers,
and delay lines exclusively in `prepare()`; `process()` only reads/writes
already-sized buffers and plain-float filter state (see
`Source/DSP/Biquad.h`'s comment on why it isn't `juce::dsp::IIR::Filter`).
Allocate/lock/log on the message thread or in `prepareToPlay`, never in
`processBlock`.

**The one sanctioned exception is INPUT/OUTPUT meter telemetry**
(`Source/Core/LevelMeter.h`). `processBlock()` pushes the block's peak
magnitude into a lock-free `std::atomic<float>` (a compare-and-swap "keep
the max" loop - no allocation, no lock, no WebView/JS call of any kind from
the audio thread itself). A `juce::Timer` on the *editor*, running on the
message thread, is the only thing that ever reads/resets that atomic,
applies attack/release envelope smoothing, and forwards the result to the
WebView as a `meterLevels` JS event. Meters are telemetry only - not an
APVTS parameter, never part of `getStateInformation()`/`setStateInformation()`.
Follow this same push-an-atomic / read-on-a-timer shape for any future
audio-thread-to-UI data path; don't reach for anything heavier.

## Don't change working architecture without a reason

If something here works and is documented, don't refactor it "for
cleanliness" without a concrete reason tied to a real requirement. In
particular:

- Don't reintroduce Projucer.
- Don't move off the WebView-based UI bridge to a hand-rolled polling
  protocol - JUCE's relay/attachment mechanism already solves
  bidirectional sync correctly.
- Don't add DSP formats/effects beyond the 7 parameters without an explicit
  decision to start that stage of work.
- Don't add AU/AAX/VST2/LV2/Standalone targets without a reason.

## Current status (as of this entry)

**Stage: technical foundation + production UI + PREAMP DSP (frozen after
sound calibration) + EQ DSP + SAT DSP + PITCH DSP + PAN DSP + VERB DSP +
IMAGE DSP. All 7 modules now have real audio processing.**

Verified on this machine (Windows, Visual Studio Community 2026 /
MSVC 19.51):

- Clean configure + build for both `windows-debug` and `windows-release`
  presets, **zero compiler warnings**, including `juce_dsp` (linked for
  PREAMP's and SAT's independent oversampling instances; EQ and PAN
  themselves need no oversampling) and the vendored Signalsmith Stretch/
  Linear headers (PITCH) - their own upstream warnings are suppressed at
  the include site (`#pragma warning (push, 0)` / GCC diagnostic push
  around the vendored include in `PitchProcessor.cpp`), not patched into
  the vendored source itself.
- Real `.vst3` produced at
  `build/windows-release/Source/Plugin/UNI76_artefacts/Release/VST3/UNI 76.vst3`.
- All `UNI76Tests` (JUCE `UnitTest`-based) pass in both Debug and Release,
  including the full PREAMP DSP suite (see
  [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md)), the full EQ DSP suite (see
  [docs/DSP_EQ.md](docs/DSP_EQ.md)), the full SAT DSP suite (see
  [docs/DSP_SAT.md](docs/DSP_SAT.md)), the full PITCH DSP suite (see
  [docs/DSP_PITCH.md](docs/DSP_PITCH.md)), and the full PAN DSP suite (see
  [docs/DSP_PAN.md](docs/DSP_PAN.md)): PREAMP transparency near
  DRIVE=0%, calibrated harmonic progression, level-dependent behaviour,
  Low/High Cut, gain-staging within a few dB, aliasing suppression, null
  test; EQ DARK/PHONE/AIR response shape, PHONE's voice-band suppression
  either side of a readable mid, click-free automation through the 50%
  crossing, zero-latency bypass; SAT's energy-dependent dynamic gain
  (peak-hold envelope, crest-factor reduction verified after fixing an
  increasing-crest regression the first attack-time design measured),
  bounded per-half-gain waveshaper (own constants, not PREAMP's),
  frequency tilt (bass retained within 0.3dB, highs soften 3.6-4.6dB at
  HEAT=100%), own independent oversampling/latency summed correctly into
  total plugin latency, aliasing suppression; PITCH's fixed latency
  (independent of semitone/enabled/host block size, and - unlike
  PREAMP/SAT - nonzero at every sample rate including 192kHz), pitch
  accuracy (<0.25% error at every tested interval), a 36-case bass-
  stability matrix via phase-vocoder frequency reassignment (worst case
  0.13% frequency deviation / 0.043dB amplitude-modulation depth, both
  roughly an order of magnitude inside their pass thresholds - see
  docs/DSP_PITCH.md), sideband suppression (32-43dB below the target
  fundamental), 0 ST transparency (<0.2dB gain deviation), transient
  quality (no pre-echo/double-hit), bit-identical stereo output for
  bit-identical input (two independent mono engines, not a shared
  multi-channel instance), discrete-parameter state migration from the
  old 0-100% pitch format (verified against the real, experimentally-
  confirmed old serialized format, not guessed), and click-free discrete
  automation transitions; PREAMP+EQ+SAT+PITCH integration (representative
  combinations incl. EQ PHONE + SAT HOT + PITCH shifted, no NaN/Inf/gain-
  explosion); all four modules' mono/stereo (incl. no stereo drift), all
  supported sample rates, multiple block sizes (SAT/PITCH: 32-2048),
  NaN/Inf safety, and state save/restore.
- **PITCH polyphonic material** (a follow-up pass, closing a gap the
  initial PITCH work flagged as untested): bass+harmonics, two low tones,
  major/minor triads, a dense 5-note chord, and the especially critical
  bass+chord case, each swept across 6 semitone intervals - the bass-
  under-chord case measures as the *most* stable of the five (freqStd
  0.09-0.29%, ampDbStd under 0.14dB - see
  [docs/DSP_PITCH.md](docs/DSP_PITCH.md)'s "Polyphonic material" section,
  which also documents a test-design confound (real acoustic beating
  between closely-spaced test tones, and Goertzel-bin leakage between
  chord tones a minor third apart) that was found and fixed in the test
  methodology rather than papered over with looser thresholds - the DSP
  itself was not changed for this). Also closed: an exact PITCH-alone vs
  PREAMP+EQ+SAT vs total-plugin latency breakdown at 44.1/48/96/192kHz, a
  broadband 0 ST A/B against a latency-aligned dry copy (RMS diff 7.2e-8),
  and non-identical (not just dual-mono) stereo content confirming the two
  independent per-channel engines track a shared bass component to within
  0.02% and 0.01dB of each other.
- **PAN DSP** (width + slow motion, see [docs/DSP_PAN.md](docs/DSP_PAN.md)) -
  a follow-up pass that replaced an earlier MONO/NATURAL/WIDE width-only
  revision (commit `0ad8510`) with the current, final ORIGINAL(0%)/
  WIDE(50%)/MOTION(100%) contract: width mapping matches the product
  brief's target curve (measured 1.00/1.14/1.45/1.76/1.90 vs targets
  1.0/~1.2/~1.45/~1.68/~1.9 across 0/25/50/75/100%); ORIGINAL (0%)
  measured as a near-identity transform (RMS diff ~1e-4-scale) - provable
  from the topology, not just measured, since every curve evaluates to
  its identity value at t=0 regardless of what the crossover/allpass
  filters are doing internally; motion verified via an explicit stereo-
  centroid trajectory metric growing monotonically with width (RMS
  excursion 0/0.0016/0.050/0.234/0.348 at 0/25/50/75/100%) that visits
  clearly left- and right-biased states at MOTION, smoothly and
  continuously, not just varying shades of one side - closing a real bug
  found during development (an early induced-signal design was
  *provably* always negatively correlated with Mid, biasing the whole
  trajectory toward one side permanently; fixed by using the allpass's
  raw output instead of its difference against Mid, and by verifying
  with realistic multi-partial mono material rather than a single
  worst-case sustained tone); the motion LFO measured sample-rate- and
  block-size-independent (~3.35s period, target ~3.33s) and confirmed
  not reset by width automation; combined stereo power stays within
  ~0.1dB across a full motion cycle (constant-power rotation, proven
  algebraically for the spatial term and confirmed in the full signal
  including its Mid cross-term); no pitch drift at MOTION (<0.2% error,
  100Hz-5kHz); bass motion measured 5-25x smaller than 3kHz's; a second
  real bug found and fixed - the mono-source "induced" signal initially
  leaked its own bass-frequency content into the low band (measured
  22.5dB L/R imbalance on a centred 80Hz tone), fixed by highpassing the
  induced signal before blending it in (residual dropped to 2.5dB,
  honestly documented rather than hidden); mono fold-down and
  correlation are disclosed as no longer bit-exact-invariant under
  motion specifically (up to ~2.5dB fold-down change, correlation can go
  slightly negative on correlated material at full MOTION) per the
  product brief's own relaxed mono-compatibility requirement; no runaway
  gain; click-free automation/bypass; zero added latency at every sample
  rate/block size/width/enabled state, confirmed the total plugin
  latency is unchanged from the pre-PAN PITCH baseline; PITCH+PAN
  integration (PAN's motion doesn't add wobble to PITCH's already-
  verified bass stability); full 5-module chain integration; backward-
  compatible state migration - schema v5 forces *both* genuinely old
  pre-DSP states *and* v4 states saved under the retired MONO/NATURAL/
  WIDE contract to the new 0% (ORIGINAL) default, since neither an old
  value nor a v4-era "50% NATURAL" choice means anything under the
  current contract.
- **PAN crossover-artifact + correlation fix** (commit `b6f83ca`'s
  topology superseded, see [docs/DSP_PAN.md](docs/DSP_PAN.md)'s
  "Crossover artifact" and "Correlation" sections) - a follow-up pass
  that fixed two known issues in the ORIGINAL/WIDE/MOTION topology
  without touching its product contract (still 0%/50%/100% =
  ORIGINAL/WIDE/MOTION, default 0%, ~0.3Hz LFO, 0 added latency):
  (1) a real ~2.5dB frequency-response bump at the 150-200Hz crossover,
  root-caused via a QM-AM-inequality derivation (`|a*LP+b*(1-LP)|` at
  the crossover equals `sqrt((a^2+b^2)/2) >= (a+b)/2`, i.e. a *vector*
  sum of two phase-shifted complementary bands weighted by different
  real gains, not a linear interpolation, whenever the two gains
  differ) - fixed by replacing the shared band-split-then-sum with two
  independent per-channel RBJ low-shelf filters (no second, differently-
  gained path to vector-sum against, by construction), verified via a
  dedicated 14-frequency (40Hz-10kHz) regression test showing a
  perfectly smooth, monotonic response with zero envelope violations at
  every width - this test's own first version had a `float`-phase-drift
  and inconsistent-LFO-phase measurement bug of its own, caught and
  fixed before trusting any number from it; (2) correlation on
  correlated stereo material going slightly negative at 100% width
  (-0.07 to -0.11 measured), root-caused algebraically (`E[L*R] =
  E[Mid^2] - width^2*E[Side^2]` for symmetric-gain width alone, which
  provably goes negative once amplified Side power exceeds Mid power,
  independent of motion rotation) and fixed by reducing
  `panWidthMaxHigh` (1.9->1.6) - the actual dominant lever, found only
  after `panMotionThetaRange`'s reduction (pi/4->0.55, tried first on
  the assumption motion rotation was the main driver) measured almost
  no improvement on its own, a useful negative result kept anyway since
  it still softens rotation's peak L/R gain ratio at no cost - now
  measured `>=0` at every tested width (0.965/0.955/0.883/0.567/+0.216
  at 0/25/50/75/100%) while motion stays clearly audible (centroid
  RMS excursion 0.237 at 100%, still visiting clearly left- and right-
  biased states). A 2nd-order Butterworth was also tried for the
  induced-signal lowpass specifically (hoping to reduce a separate,
  ~2.6-2.9dB centre-stability residual, unrelated to the crossover fix)
  and measured *worse* (~3.4dB, the same vector-sum mechanism
  reintroduced via a steeper filter's larger phase excursion) - reverted
  rather than shipped. All previously-passing PAN/PREAMP/EQ/SAT/PITCH
  tests remain green; new regression tests cover crossover-region
  frequency response, correlation on representative material, and the
  updated width/motion mapping targets.
- **PAN centre-bass isolation fix** (commit `fc98bbc`'s remaining
  blocker closed, see [docs/DSP_PAN.md](docs/DSP_PAN.md)'s "Centre-bass
  isolation" section) - a follow-up pass that closed the one issue the
  crossover/correlation fix explicitly left open: a centred 80Hz bass
  tone under stereo highs still measured ~2.6-2.9dB of L/R movement at
  MOTION, unacceptable for a control whose core rule is "the low end
  stays centred." Root-caused as a *different* mechanism from the fixed
  crossover bump: `inducedHigh = induced - LP(induced)` (stripping the
  synthesised induced signal's own bass before blending) is itself a
  complementary subtraction, and `1-LP(f)` has its own phase-shifted-
  vector-subtraction hump right at its corner (proven algebraically: at
  a 1st-order corner, `1-LP(fc) = 0.5+0.5j`, magnitude 0.707, a *rise*
  where the design intent was suppression) - explaining why an earlier
  attempt at a steeper 2nd-order version of this same subtraction
  measured *worse* (~3.4dB), not better. The actual fix: stop building
  `inducedHigh` as anyone's complement at all - unlike the width/motion
  shelves, it has no reconstruction identity to protect (it multiplies
  by `panInducedBlend(t)`, exactly 0 at t=0 regardless of filter shape),
  so it can use a *real*, independently-designed, cascaded 6th-order
  Butterworth highpass (-36dB/oct) with no vector-sum risk at all - 2nd
  order alone left ~1.5dB, 4th order still left ~1.5dB specifically at
  120Hz (closest to the corner), a third cascaded stage finally closed
  the gap. Measured: centre-bass-under-stereo-highs L/R dropped from
  ~2.6-2.9dB to **0.19dB**; a new dedicated `CenteredBassMotionIsolation`
  test independently confirms bass magnitude barely shifts from ORIGINAL
  to MOTION (0.10dB), a bass-only (not broadband) centroid trajectory
  stays essentially flat (rmsExcursion 0.0006), and high-frequency
  motion stays fully intact and unweakened (broadband centroid
  excursion 0.149, ~260x the bass-only figure); a new 40-120Hz
  magnitude/centroid table (`Tests/PluginTests.cpp`) confirms every
  frequency at 100% width lands well inside this round's tightened
  per-frequency targets (40-80Hz <0.25dB actual ~0.0-0.10dB, 100Hz
  <0.5dB actual 0.28dB, 120Hz <0.75dB actual 0.24dB). Correlation and
  mono fold-down both improved further as side effects (correlation at
  100% width on the correlated-chord test material rose from +0.216 to
  +0.393, still positive at every macro value; worst-case mono fold-down
  level change dropped from ~1.5dB to ~1.1dB) - the previous filter's
  near-corner overshoot had been adding a small amount of extra,
  differently-phased induced content around 150-300Hz that the clean
  highpass no longer contributes. Motion depth, width mapping, LFO rate,
  and 0 added latency are all unchanged from the previous round; all
  previously-passing PREAMP/EQ/SAT/PITCH/PAN tests remain green.
- **VERB DSP** (electromechanical plate reverb, see
  [docs/DSP_VERB.md](docs/DSP_VERB.md)) - the sixth module to get real
  DSP: a 1970s-style plate + analog send/return electronics, one machine
  at every setting (`DRY(0%)/PLATE(50%)/DEEP(100%)`, macro only changes
  send/decay/pre-delay). Architecture: 350Hz cascaded 4-pole Butterworth
  highpass on the wet send, a tiny asymmetric-tanh analog send stage, a
  4-stage short-delay Schroeder-allpass diffuser (early density - not
  used alone as the whole reverb, which the product brief explicitly
  rejected as a "cheap Schroeder" architecture), a smoothly-variable
  pre-delay, a 12-line FDN plate tank (Householder feedback matrix -
  orthogonal/energy-preserving, O(N) per sample - with per-line
  `OnePoleLowPass` damping so highs decay faster than mid, fed from a
  single mono sum and read out via two independent fixed sign patterns
  for genuinely decorrelated stereo, the way a real plate's two pickups
  at different positions would), an analog return stage (tiny tanh +
  ~9.5kHz soft bandwidth ceiling), and a second, lighter 350Hz safety
  highpass on the wet output (a recirculating feedback network's own
  resonances aren't guaranteed to respect an input-side filter alone).
  DRY is read into locals and written back unmodified in the same
  per-sample loop iteration - never passing through any filter/delay/
  nonlinearity - making "dry never touched" an algebraic guarantee
  (measured RMS diff **0** on a broadband source) rather than a
  measured approximation. Measured: 40-120Hz wet content 60-92dB down
  (almost no tail); 160-500Hz a smooth, monotonic transition into a
  fully-present plate by 500Hz; 1kHz RT60 ~3.5s at 100% with 5kHz and
  8kHz measurably (and increasingly) shorter, confirming highs decay
  faster than mid; wet-path THD under 2% at -18dBFS across the whole
  macro range. Two real tuning findings along the way: (1) the per-line
  damping filter's small per-pass insertion loss compounds hugely over
  the hundreds of feedback passes a multi-second RT60 needs even for
  content nominally *below* its own cutoff, silently capping mid-
  frequency decay time far under its nominal target unless the decay-
  time formula's own anchors are tuned to compensate; pushing the
  damping cutoff too high to avoid this instead pushed the shortest
  delay line's loop gain close enough to instability to measurably
  distort the result (THD roughly doubled), which is what motivated
  adding `verbLineFeedbackGainMax`, a hard safety clamp on any single
  line's feedback gain independent of the RT60 formula; (2) the
  send/return analog stages' dominant even-harmonic (H2) content is
  driven specifically by the tanh waveshaper's *asymmetry* term, not
  its drive gain - an initial tuning pass that only reduced drive
  measured barely any THD improvement (5.9% -> 4.9%), while reducing
  asymmetry specifically brought it down to ~1-2%. PAN=100+VERB=50
  integration measured 80Hz bass L/R at 0.16dB (bass stays centred and
  stable with the plate layered on top - PAN's and VERB's independent
  bass-protection mechanisms compound rather than compete); full-chain
  (PREAMP+EQ+SAT+PITCH+PAN+VERB) VERB0->VERB100 bass change measured
  under 0.1dB at 60/80/100Hz. Zero added latency at every sample rate/
  block size/wet value/enabled state. UI tri-scale updated to
  `DRY`/`PLATE`/`DEEP` (was the passthrough placeholder's illustrative
  `SPRING`/`PLATE`/`CHAMBER`); knob default unchanged at 0%.
- **Real VST3 host validation.** A purpose-built harness using the real
  `AudioPluginFormatManager`/`VST3PluginFormat` hosting code (not
  `UNI76AudioProcessor` directly) loaded the built `.vst3`, confirmed all 7
  parameters (plus one host-added generic bypass parameter, standard VST3
  hosting behaviour, not a UNI 76 parameter), confirmed PITCH defaults to
  0 ST and PAN defaults to 0% ORIGINAL, drove PREAMP/EQ/SAT/PITCH/PAN
  through real `processBlock()` calls across the full `-12..+12` ST range,
  the full 0-100% PAN width range, and through a combined full-chain
  setting with real bass, confirmed measurable/bounded/finite output,
  correct correlation behaviour at MOTION, and correct total (summed)
  latency (unchanged by PAN), ran automation sweeps (incl. rapid discrete
  PITCH jumps and the full PAN width range) during continuous audio,
  round-tripped state through a real `getStateInformation()`/
  `setStateInformation()` save+restore, and confirmed identical behaviour
  from a `.vst3` copy in a completely isolated directory (no
  `Resources/Web` nearby), proving the UI resources and all five DSP
  modules' code are genuinely self-contained in the binary.
  `preampEnabled`/`eqEnabled`/`saturationEnabled`/`pitchEnabled`/
  `panoramaEnabled` bypass are each verified at the C++ level
  (`Tests/PluginTests.cpp`) rather than through this external harness -
  like the editor's `IPlugView`, `ModuleEnableState` is deliberately not
  VST3-visible (no parameter, no exposed state format), so an external
  black-box host harness has no legitimate way to toggle it without going
  through the WebView native bridge, which - as established in the UI
  audit - such a harness cannot reach either.
- **VERB acceptance/calibration pass** (validation-only round on top of
  the module's original implementation, see
  [docs/DSP_VERB.md](docs/DSP_VERB.md)'s "Acceptance / calibration pass"
  section) - a mandatory narrowband resonance sweep (16 points, 1/3-
  octave, followed up at 10-20Hz resolution around two flagged regions,
  plus a full decay-curve comparison at the single most theoretically-
  likely candidate for a reinforced mode - two delay lines' harmonic
  series nearly coincide near 1235Hz) found **no genuine dominant/stuck
  mode**, so no FDN/diffuser/damping retuning was made; impulse-density,
  the 40-1000Hz low-end table, THD, wet-only/full-output/mono-fold-down
  correlation (confirming the previously-reported ~-0.45 wet-only figure
  doesn't destabilise the full, dry-dominated mix), PAN=100%+VERB=100%
  time-varying correlation, centred-bass-under-PAN+VERB, and the RT60
  table across all four macro values all independently confirmed the
  existing design already meets its targets - no architecture change
  resulted. This pass did, however, find and fix a **second, independent
  instance** of the pre-delay NaN-indexing bug class documented above: a
  non-finite `wetNormalised01` (the macro parameter itself, not an audio
  sample) reached `verbPiecewise()`'s array-segment-index computation
  (`VerbCurves.h`) via the same `std::clamp`-does-not-clamp-NaN gap,
  caught this round by a new, dedicated regression test written
  specifically to hunt for this bug class - fixed at both the entry
  point (`VerbProcessor::process()`) and the shared curve utility
  itself. New final listening WAVs at `docs/audio/verb-final-*.wav`.

- **macOS**: not built or tested - no macOS machine available in this
  session. `CMakePresets.json` defines `macos-debug`/`macos-release`
  presets targeting a Universal Binary (`x86_64;arm64`), but they are
  unverified. The vendored Signalsmith Linear header contains its own
  guard against a known Apple Clang 16.0.0 + `-ffast-math` SIMD bug
  (`#error`s at compile time if that exact combination is detected) -
  relevant only once macOS builds start, not evaluated this session.
- **pluginval**: not installed on this machine; installing a third-party
  executable validator from the internet was treated as out of scope. The
  VST3-hosting validation above already covers real VST3 hosting,
  parameter automation, and state persistence, so this is a lower-priority
  follow-up than it was in the previous entry.
- **Windows/macOS production installers**: both `Packaging/*` folders are
  documented skeletons, not working installers (see
  [docs/RELEASE.md](docs/RELEASE.md)).
- **Manual mouse-driven interaction with the real editor**: a follow-up
  session closed this gap for PITCH specifically - a scratch JUCE host
  app loaded the real built `.vst3`, created its real WebView2 editor, and
  real synthetic OS-level mouse/keyboard input (not host-API parameter
  calls) drove the PITCH knob: a 110px drag landed exactly on -12 ST, one
  wheel notch moved exactly 1 ST, one ArrowUp press moved exactly 1 ST,
  double-click reset exactly to 0 ST, dragging 300px past either edge
  still clamped exactly at -12/+12 ST with no overshoot, and Shift showed
  no fine-control effect - see [docs/DSP_PITCH.md](docs/DSP_PITCH.md)'s
  "Live UI verification" section and the three real screenshots in
  `docs/screenshots/pitch-{minus12,zero,plus12}.png`. This was PITCH's own
  knob only, still through synthetic (not a human's physical) input, and
  the other 5 modules' knobs were not re-exercised this way (unchanged
  since the PREAMP/EQ/SAT pass, not expected to need it). A follow-up
  pass closed this gap for PAN's ORIGINAL/WIDE/MOTION rework specifically
  (superseding the MONO/NATURAL/WIDE-era PAN entry, which had not been
  screenshotted): a scratch JUCE host app loaded the real built `.vst3`,
  created its real WebView2 editor, and confirmed by direct screenshot -
  not by reading `index.html`/`knob.js` - that a fresh instance renders
  the PAN knob at its 0% resting position (pointer left, matching every
  other non-EQ module) with the tri-scale reading `ORIGINAL WIDE MOTION`
  and its marker at the far left, then used a real host-API automation
  call (`setValueNotifyingHost`, the same call path a DAW's automation
  lane uses - not a WebView-internal shortcut) to drive PAN to 100% and
  confirmed the knob rotated fully, the label read `100% WIDTH`, and the
  tri-scale marker moved to the far right under `MOTION`, with the rest
  of the UI (all 6 other modules, header, footer) unchanged from the
  idle baseline. This was real-value automation rather than a
  physical/synthetic mouse drag (unlike PITCH's pass above), since the
  PAN acceptance checklist only required confirming the labels/default/
  marker are correct, not re-verifying drag-distance-to-value mapping
  (PAN's knob input handling is shared, unmodified `knob.js` code, already
  covered by PITCH's synthetic-mouse pass). Screenshots saved at
  `docs/screenshots/pan-final.png` (PAN ~100%, MOTION).
- **IMAGE DSP** (the seventh and final module to get real DSP, see
  [docs/DSP_IMAGE.md](docs/DSP_IMAGE.md)) - a deliberate, explicit
  exception to the "one knob per module" rule: two independent public
  parameters, `imager` (0-100%, frequency-dependent width/imaging amount
  - `ORIGINAL(0%)/FOCUS/WIDE(100%)`) and the new `imageTilt` (-100..
  +100, default 0/CENTER, a static L/R stereo-image balance/tilt -
  explicitly not a pan). Both axes are Mid/Side-domain and orthogonal by
  construction (`imager` only ever reshapes Side via a single low-shelf,
  never Mid; `imageTilt` only ever reshapes Mid via a bounded constant-
  power gain pair applied through two per-channel low-shelf filters,
  never Side), which makes all three identity cases (IMAGE=0/TILT=CENTER,
  IMAGE>0/TILT=CENTER, IMAGE=0/TILT!=CENTER) provable, not just measured -
  CENTER/0% null test measured RMS diff < 1e-5. `imageTilt`'s gain law
  (`gL=sqrt(2)*cos(pi/4+theta)`, `gR=sqrt(2)*sin(pi/4+theta)`) is
  provably constant-power (`gL^2+gR^2==2` for any tilt, measured exactly
  2.000 at every tested value) and provably symmetric
  (`gL(theta)==gR(-theta)`, measured relative difference at
  floating-point precision on a genuinely asymmetric source); combined
  loudness stays within ~0.22dB across the full -100..+100 sweep; the
  quiet channel never fully disappears even at full tilt (bounded to
  `thetaMax=42deg`, short of the true 45deg "vanish point" - measured
  ~-22.6dB down, never silent) and correlation never goes negative
  (measured 0.34 at full tilt, down from 1.0 at CENTER). A frequency-
  dependent bass-safety shelf (reusing PAN's own proven-safe single-
  shelf-not-band-split-then-sum construction, so no frequency-response
  bump is possible by construction) keeps tilt under 1.4dB below 80Hz
  even at full ±100%, converging to the full theoretical gain-ratio by
  ~1-2kHz. PAN+TILT integration confirmed TILT shifts the *average* bias
  of PAN's own motion trajectory while motion continues and PAN's ~0.3Hz
  LFO period stays unaffected (measured within 0.75%). Real VST3 host
  validation confirmed 9 host-visible parameters (8 UNI 76 + 1 host
  bypass), correct defaults, and a full automation/state-round-trip
  pass; real WebView2 GUI screenshots at IMAGE0/TILTCENTER,
  IMAGE100/TILTCENTER, IMAGE100/TILTLEFT100 and IMAGE100/TILTRIGHT100
  (`docs/screenshots/image-*.png`) confirm the compact linear TILT
  control this round shipped rendered correctly inside IMAGE's own panel
  without disturbing the other six modules - superseded by a later
  UI-only round's spatial field pad (`Resources/Web/field_pad.js`, see
  docs/DSP_IMAGE.md's "UI: spatial field pad" section); the parameter
  contract and DSP are unchanged. No schema bump was needed
  for the new `imageTilt` parameter (see `Source/Core/PluginIdentity.h`'s
  documented reasoning - a brand-new parameter has no old value to
  reinterpret, unlike PITCH's v3 or PAN's v5 migrations) - verified via a
  hand-built legacy state missing the `imageTilt` node, which correctly
  falls back to its own default (0/CENTER).
- **IMAGE spatial field pad** (UI-only follow-up round, no DSP/parameter
  change - see docs/DSP_IMAGE.md's "UI: spatial field pad" section)
  replaced the linear TILT slider above with a small square 2D pad
  (`Resources/Web/field_pad.js`) that drives `imageTilt` (X axis) and
  `imager` itself (Y axis, inverted so bottom=0%/top=100%) from a single
  control, spatially: bottom-centre=original/centred,
  top-centre=wide-centred, top-left/right=wide+left/right bias,
  bottom-left/right=left/right bias with minimal image amount. The main
  IMAGE knob is unchanged and stays the primary `imager` control - both
  it and the pad write the same parameter and stay in sync for free via
  `getSliderState("imager")`'s single JS-side singleton (both widgets
  register listeners on the exact same object, so a change from either
  one, or from host automation, notifies both). A small flat "listener/
  microphone" mark (two CSS-only `<span>`s, no images/SVG) sits fixed at
  the pad's bottom-centre as a non-interactive spatial reference point.
  Real screenshot testing at all three supported window sizes (600x400/
  960x640/1350x900) caught a genuine layout bug before it shipped: an
  initial `width: clamp(...); max-width: 100%` construction let the
  pad's own intrinsic size push the whole IMAGE column wider than its
  fair share of the 7-column row at 600x400, overflowing the editor -
  fixed by flipping the priority (`width: 100%; max-width: clamp(...)`,
  parent-driven first, capped second). A comparison screenshot of the
  *prior* linear-TILT UI at the same 600x400 size confirmed a separate,
  pre-existing header/tri-scale text truncation at that size predates
  this round and was not introduced or fixed by it (out of scope - this
  round only touched IMAGE's own control, not the app's overall
  minimum-width text layout). Real VST3 host validation confirmed
  parameter/UI sync in both directions (host automation of `imager` moves
  both the knob and the pad's Y position; host automation of
  `imageTilt` moves the pad's X position) and that state restore on a
  *fresh* plugin instance (no live automation call after load) renders
  the puck at the correct position purely from the loaded APVTS state -
  the same mechanism every other control already relies on, no new
  persistence path needed. Screenshots at
  `docs/screenshots/image-pad-{center,wide-center,wide-left,wide-right}.png`.

- **Full pre-release technical audit** (see
  [docs/FULL_DSP_AUDIT.md](docs/FULL_DSP_AUDIT.md) for the complete
  37-section report) - a verification-only pass across the whole plugin
  as one commercial VST3, run from commit `39ab854` with every module's
  DSP and the current UI frozen: signal path/parameter contract/defaults
  reconfirmed via a real VST3 host; every real historical state-schema
  version walked through migration with an *audible*, not just numeric,
  check (an old project genuinely can't gain pitch-shift/mono-ness/PAN
  motion/VERB/IMAGE/FIELD bias on load); a genuine technical-neutral
  state defined and measured (PREAMP/SAT/PITCH/PAN/VERB/IMAGE/TILT at 0,
  EQ disabled rather than given a fake "flat" value) - found to be
  *near-* rather than *bit-*transparent by design (PREAMP/SAT's own
  nonzero minimum drive-gain floors, `preampDriveGainMin=0.05`/
  `satDriveGainMin=0.08`, already present before this audit); full-chain
  gain staging across 8 source types x 5 levels; a 65-combination
  extreme parameter matrix (no NaN/Inf/poisoned state at any
  combination); hot-nonlinear input bounded at every tested case;
  integrated low-end/high-end stress confirming PITCH accuracy holds and
  no aliasing/fold-back explosion, while honestly documenting that
  PAN=100%+VERB=100%+IMAGE=100%+TILT=+-100% *simultaneously* (a combined
  extreme no single module's own tuning specifically targeted) shows
  more Side/decorrelation than any one module alone - VERB's own
  reverb tail is itself genuinely decorrelated stereo by design above
  ~120-160Hz; PITCH full-chain-vs-alone regression within a documented
  tolerance; PAN+IMAGE+FIELD integration confirming PAN's LFO period/
  motion survive IMAGE/TILT unperturbed and TILT alone never creates new
  motion; true mono-bus processing confirmed to leave TILT bit-identical-
  neutral (no second channel to bias between); a full latency table
  across all 6 supported sample rates cross-checked between the real
  host and the in-process processor (identical); module-bypass timing,
  automation torture (all 8 params + all 7 enable flags simultaneously),
  NaN/Inf-boundary robustness, prepare/process/release lifecycle across
  rates and all 7 standard block sizes, a zero-dynamic-allocation proof
  for `processBlock()` under the heaviest exercised configuration, a
  Release CPU benchmark (worst case ~5.5x faster than real time on this
  machine), bit-identical determinism across two independent runs, and
  16/8-instance independence (in-process and real-VST3-hosted) all
  passed. **One real, commercial-release-blocking bug found and fixed**:
  `UNI76AudioProcessor::getTailLengthSeconds()` unconditionally returned
  `0.0` regardless of VERB's actual wet amount, meaning a host would cut
  VERB's audible tail off immediately (e.g. on bounce, or when a clip
  ends) even at DEEP/100% (~6s target RT60) - fixed to read the same
  `reverb`/`reverbEnabled` state `processBlock()` already uses and return
  `VerbCurves.h`'s own `verbDecaySeconds()`, verified via both the
  in-process test suite and a real VST3 host (`0s -> 2.6s -> 6.0s` at
  VERB `0/50/100%`, matching `verbDecayAnchors` exactly) - a host-
  metadata fix that changes no audio sample, so it didn't require the
  "proven regression" DSP-change protocol below. GUI re-verified via
  real WebView2 screenshots at all 3 sizes plus host-automation-sync,
  two-simultaneous-editors independence, and state-reopen-from-fresh-
  instance. Windows DLL dependencies and bundle contents both confirmed
  clean (no debug/dev-only DLL, no `WebView2Loader.dll` per the existing
  static-link design, bundle contains only the binary + moduleinfo.json).
  WebView native-bridge reviewed and confirmed to expose exactly the two
  documented functions with no filesystem/shell/network access. Official
  Tracktion `pluginval` was built from source this session (not a
  production dependency) and confirmed functional against a real
  third-party plugin, but does not currently complete against UNI 76 for
  reasons not fully root-caused this session (see
  docs/FULL_DSP_AUDIT.md's section 36) - flagged as an open follow-up,
  not a pass or a known defect. No third-party DAW smoke test was
  performed (no desktop-GUI-automation tool available this session,
  despite FL Studio 21 being genuinely installed) - the mandatory real-
  VST3-host harness already covers the same underlying hosting API. No
  new effects, no UI redesign, no new listening WAVs, and no presets/
  installer/signing/licensing/release work were started, per the audit's
  own scope.

- **RC1 blocker closure** (see
  [docs/FULL_DSP_AUDIT.md](docs/FULL_DSP_AUDIT.md)'s updated sections 5,
  6, 27, and 36 for full detail) - a focused follow-up to the full audit
  above, closing its three named release blockers without touching any
  frozen DSP/UI:
  - **Technical-neutral -4.94dB, decomposed**: proven to be a
    misinterpreted null-residual metric, not a gain/coloration problem -
    a new module-by-module decomposition test measured `gainDeltaDb`
    (pure level comparison) separately from the original null-residual-
    relative-to-input number and correlation, at 100Hz/1kHz/10kHz/
    broadband/impulse across all 7 cumulative PREAMP->SAT->PITCH->PAN->
    VERB->IMAGE/TILT stages (EQ disabled throughout). Real output gain
    stays within a fraction of a dB of unity everywhere (worst case
    -0.21dB at 10kHz); the deviation traces entirely to PREAMP+SAT, and
    `docs/DSP_PREAMP.md`'s own pre-existing "Null test (DRIVE=0%
    transparency)" section - written before this session - had *already*
    identified and explained this exact measurement artifact (documented
    near-identical numbers from an earlier round: "-9.6dB at 100Hz, -4.9dB
    at 10kHz"), root-caused to PREAMP's always-on 20Hz/20kHz soft Low/
    High Cut filters' own group delay near their cutoffs, with the
    *correct* (Goertzel-measured gain) figures already on record at
    under 0.3dB. Two independent measurement techniques, a session apart,
    converge on the same answer. **No DSP was changed** - the test's own
    acceptance metric and this document's phrasing were corrected
    instead.
  - **pluginval hang, isolated to an exact stage and mechanism**: exact
    revision recorded (`4c5adc2c1a9910251667152166139a0c37b953e6`, v1.0.4,
    JUCE 8.0.13), exact hang stage identified by reading pluginval's own
    source (`Open plugin (cold)` -> `testOpenPlugin()` +
    `deletePluginAsync()`'s `instance.reset()`, called from the JUCE
    message thread after the instance was created on pluginval's
    background `Validator` thread). A minimal scratch repro (built
    against UNI 76's own production JUCE 9.0.1, not pluginval's JUCE 8)
    reproduced the identical failure - and proved it isn't about
    crossing threads specifically: destroying the instance on *any*
    non-message thread fails, even one that also created it. The same
    harness against `Kontakt.vst3` (a real, non-JUCE third-party plugin)
    succeeds cleanly every time under the identical pattern - a
    genuine, isolated behavioural difference, not a guess. No raw stack
    trace was obtainable (no native debugger available in this
    environment, and neither JUCE's own crash handler nor Windows WER
    ever fires - the process simply terminates). No source change was
    made: every test in this project that creates/destroys UNI 76 on the
    actual host message thread - the standard, documented VST3-hosting
    convention, and the only pattern real DAWs are expected to use -
    already succeeds without issue, including pluginval's own successful
    handling of Kontakt. Recorded as a specific, evidenced, low-practical-
    risk open item, not a pass and not swept under "false positive"
    without proof.
  - **Long-run soak, closed**: an accelerated offline soak
    (`Tests/SoakTest.cpp`, a new standalone executable - deliberately
    *not* part of the routine `UNI76Tests` suite, since it processes
    hours of equivalent audio) ran 60 minutes-equivalent of continuously-
    automated stereo audio at 48kHz **twice** (all 8 parameters cycling
    on independent periods, module-enable flags toggling on a
    deterministic schedule, identical schedule/seed both times) plus a
    20-minutes-equivalent pass at 96kHz. Zero NaN/Inf across all three
    passes; peak stayed bounded; **zero dynamic allocations** after the
    first block in every pass (the same global-`operator new` audit
    technique the main audit's allocation test uses); working-set memory
    stayed flat within a rounding error across each full 60-minute pass
    (no growth trend); and the two 48kHz runs produced a **bit-identical
    output checksum** (FNV-1a over every processed sample), proving the
    plugin's determinism holds under sustained automation/enable churn,
    not just a single short buffer.
  - No independent DSP/UI bugs were found or fixed this round - all
    three blockers closed via test/documentation correction and
    additional evidence, per the frozen-DSP/UI scope.

- **RC1 candidate (Windows x64)** (see
  [docs/FULL_DSP_AUDIT.md](docs/FULL_DSP_AUDIT.md)'s "RC1 candidate"
  section for full detail) - the first internal, unpublished Windows x64
  Release Candidate, from commit `28ec37c` with DSP/UI still frozen (no
  retuning, no new effects). `UNI76_VERSION` stays `0.1.0` (not bumped);
  the installer carries its own separate `0.1.0-rc1` display version and
  the artifact is named `UNI76-Windows-x64-RC1-Setup.exe`, independent of
  the plugin's own version. **Factory preset system**: the header's
  PRESET/A-B/Settings buttons were real `disabled` placeholders with zero
  JS wiring before this round (not a partial implementation). Presets are
  a new, minimal, production-safe architecture
  (`Source/Core/FactoryPresets.h`) that adds **no new saved-state format
  and no new parameter** - a preset just calls the same
  `setValueNotifyingHost()`/`ModuleEnableState::setEnabled()` paths a
  user's own gesture already exercises, so it's captured by the existing
  save/restore mechanism automatically. 10 factory presets shipped
  (Default, Warm Analog, Dark Vintage, Telephone Plate, Wide Vintage,
  Motion Space, Focused Stereo, Deep Plate, Hot Console, Clean Wide) -
  musical starting points, no parameter at 100%, PITCH/TILT left neutral
  in every preset. **A/B**: a minimal, session-local-only (not persisted)
  two-slot snapshot toggle, implemented since it fit the existing
  architecture safely with no state-contract change
  (`Source/UI/WebUIEditor.h`'s `ABSnapshot`). **Settings**: left
  `disabled`, deliberately not implemented - documented, not silently
  skipped. Both the preset-application logic and the factory preset data
  itself are unit-tested in-process (`UNI76RC1FactoryPresetTests`, 0
  failures) - live UI click-through automation was attempted (both a
  cross-DLL `evaluateJavascript` approach, which crashed and was
  correctly abandoned as inherently unsafe, and real OS-level
  `SendInput` clicks, which landed on the correct WebView2 render
  surface at the correct coordinates but still didn't visibly open the
  dropdown) and not conclusively resolved this session - honestly
  documented as the one real gap, with a manual verification step added
  to the new `docs/RC1_FL_STUDIO_SMOKE_TEST.md` instead of a fabricated
  automated pass. `THIRD_PARTY_NOTICES.txt` (new, repo root) documents
  JUCE (AGPLv3/commercial dual-licence - explicitly flagged as
  **unresolved**, no commercial licence obtained), the VST3 SDK (MIT,
  bundled in JUCE), Signalsmith Stretch/Linear (MIT), and WebView2
  (loader statically linked, runtime is a separate Microsoft system
  component). `Packaging/Windows/UNI76.iss` went from a commented-out
  skeleton to a real, compiling Inno Setup installer (real generated
  `AppId` GUID, installs the real built `.vst3` to the standard
  `C:\Program Files\Common Files\VST3\UNI 76.vst3` location via an
  explicit `DestDir`, keeps its own uninstaller/notices/README in a
  separate per-product folder so uninstall never has to guess which
  loose files in the shared VST3 folder belong to UNI 76, checks for the
  WebView2 Runtime and informs rather than silently auto-installing it).
  A full install -> load/GUI/audio/state -> uninstall -> confirm-cleanup
  -> reinstall dry run was actually performed on this machine (not just
  described) - confirmed clean removal with every one of the many other
  third-party plugins already in the shared VST3 folder left untouched.
  Real VST3 host validation against the *installed* copy (not the dev
  build tree) confirmed the same parameter contract/defaults/latency/
  audio processing/state round-trip already established, plus correctly
  rendered GUI screenshots. Debug and Release builds both clean (0
  warnings); all tests green in both configurations.

- **UX polish pass** (real bugs found via manual testing of the RC1
  candidate, not a new feature round - see the user's own 21-item brief;
  DSP untouched except a fully-investigated-and-reverted VERB attempt,
  see below; no installer/release/macOS work) - closes several concrete
  UX problems and adds user presets:
  - **Preset popup reliability**: the PRESET dropdown now closes on
    selecting an item, re-clicking PRESET, clicking outside (via a
    capture-phase document listener - a bubble-phase listener could be
    starved by another widget's own `stopPropagation()`, the likely root
    cause of the reported "doesn't close reliably" bug), Escape, and
    after a user-preset Save/Delete.
  - **Preset -> full UI sync** (the real "modules stayed visually off
    after loading a preset that turns them on" bug): `module_power.js`
    now exposes one authoritative `refreshModuleEnabledUI()` that re-
    reads the real backend module-enabled state and redraws *both* the
    per-strip power buttons and the new footer SIGNAL PATH chips from it
    - called after every preset load, A/B switch, and toggle click, never
    an optimistic DOM-only update. A new regression test ("All modules
    OFF -> load a preset with modules ON -> processed audio audibly
    changes") in `Tests/PluginTests.cpp` covers the backend/audio half;
    the UI half is the same `refreshModuleEnabledUI()` mechanism, by
    construction always reading real state.
  - **Active-preset identity + dirty marker**: `WebUIEditor` now tracks
    which preset (factory or user) was last applied and a snapshot of its
    values; a cheap 8-float/7-bool compare against live state - piggy-
    backed onto the existing 30Hz meter-telemetry timer, not a new poll
    or a per-parameter-callback check - reports the current preset name
    and whether it's still an exact match ("Preset Name" vs
    "Preset Name *") via the same `meterLevels` JS event the meters
    already consume.
  - **User presets** (`Source/Core/UserPresets.h`, new): save/load/
    delete/list, stored as one `.uni76preset` XML file per preset (own
    schema version, sanitised filenames) under
    `Nostalgia Audio/UNI 76/Presets/User` in the standard per-user app-
    data directory (`juce::File::userApplicationDataDirectory`) - never a
    hardcoded path. All file I/O runs on the message thread, triggered
    only by a user gesture (menu open/Save/Load/Delete click) - the audio
    thread never touches it. The PRESET menu now lists FACTORY (grouped
    by category, see below) and USER sections plus a `SAVE CURRENT...`
    row with overwrite confirmation; user presets are deletable, factory
    presets are not (no code path exists that could delete one).
  - **Factory presets**: grew from RC1's flat 10-preset GENERAL-only bank
    to 32 presets across 5 categories (`Source/Core/FactoryPresets.h`) -
    GENERAL (the original 10, unchanged), VOCAL (6: Warm Lead, Airy Lead,
    Vintage Vocal, Plate Vocal, Wide Backing, Lo-Fi Vocal), PIANO (5:
    Warm Upright, Focused Grand, Wide Grand, Vintage Piano, Deep Plate
    Piano), ACOUSTIC GUITAR (5: Warm Fingerstyle, Bright Strum, Vintage
    Wood, Wide Acoustic, Plate Acoustic), ELECTRIC GUITAR (6: Clean
    Console, Warm Rhythm, Vintage Lead, Wide Clean, Plate Lead, Dark
    Rhythm) - values chosen per-instrument (gentle drive for vocals,
    low PREAMP/SAT for piano's wide range, more headroom for HEAT on
    electric guitar), PITCH always 0 ST, TILT always CENTER, no preset at
    a 100%/extreme value. The PRESET menu groups these under category
    headings.
  - **Settings removed**: the non-functional gear button and its dead
    `.header__btn--icon`/`:disabled` CSS are gone from the header
    entirely - no disabled placeholder left behind.
  - **A/B redesigned**: from a single `A / B (A)` button to two compact
    letters (`A` `B`) in the header, the active one highlighted with the
    same calm `--meter-green` token the meters' own "safe" zone uses (not
    a neon accent). Backend toggle logic (capture-then-apply,
    session-local, B starts identical to A so the first switch is a
    silent no-op) was already correct from RC1 and needed no change -
    only the frontend affordance changed.
  - **SIGNAL PATH footer made interactive**: renamed from "ANALOG SIGNAL
    PATH"; each chip (PREAMP/EQ/SAT/PITCH/PAN/VERB/IMAGE) is now a real
    button with a small status dot (green=on, dim grey=off) that toggles
    that module's enable state, fully synchronised with its strip's own
    power button through the shared `refreshModuleEnabledUI()`.
  - **Layout**: all 7 knobs shifted down together via one new shared
    token (`--knob-area-shift`, ~22px at the 960x640 reference, scaling
    with editor height) applied to `.module__knob-area`'s `margin-top` -
    titles stay pinned at top, the aux zone stays pinned at bottom via
    its existing `margin-top: auto`, so the shared knob/value/scale grid
    lines are unaffected. IMAGE's FIELD pad was resized down to the
    requested ~58-68px range (`--field-pad-size` retuned) and remains
    centred in its aux zone via the pre-existing auto-margin mechanism.
    A real, pre-existing bug was also found and fixed while verifying
    this at the 600px minimum width: `responsive.css` was hiding the
    *entire* header controls block (A/B + PRESET) below 700px width -
    harmless when those were RC1-era disabled placeholders, but broken
    now that they're live controls the editor's own 600px resize floor
    must support; fixed to hide only the decorative brand wordmark and
    shrink the preset-name label instead.
  - **VERB "too metallic" investigation - no DSP change shipped.** Per
    the brief's own "measure first" instruction, a new dedicated
    resonance-sweep regression test was written and used to measure the
    *original* topology's spectral flatness, then three different,
    principled FDN tuning directions (more lines with a smooth
    progression; more lines with prime-number lengths; diffusion/damping
    changes alone) were each tried and *measured* - all three came back
    neutral-to-worse on the same fixed metric, not an improvement. Rather
    than ship an unproven or measured-negative change, `VerbCurves.h`/
    `VerbProcessor.cpp` were reverted to their exact original values; the
    new test is kept as a permanent diagnostic baseline for a future
    attempt with a more robust (multi-checkpoint) methodology. See
    docs/DSP_VERB.md's new "Metallic-ring reduction investigation"
    section for the full numbers and reasoning. VERB's sound is therefore
    **unchanged** from the RC1 candidate.
  - **Startup profiling**: real instrumentation added (a JS-side
    `performance.now()` timeline from first script execution through
    `DOMContentLoaded` to "app ready", correlated against a native-side
    high-res-clock timestamp taken at the very start of the editor's
    constructor, reported once via a new `uni76ReportStartupTiming`
    native function and logged to stdout) - but real cold/warm open
    numbers were **not captured this round**; the instrumentation is
    real and in place, ready to be read the next time the editor is
    opened through any host or scratch harness, but no separate
    profiling session was run before this round's time budget was
    consumed by the VERB investigation above. Flagged as the one
    incomplete item from the original brief - see the round's own final
    report for the full list of what's still open.
  - Debug and Release builds both clean (0 warnings) after every change
    in this round; all pre-existing tests plus the new resonance-sweep,
    preset-category, and UX-polish regression tests pass.

- **Live-testing follow-up round** (real bugs and DSP requests found
  after the user installed and used the RC1-plus-UX-polish build in
  Ableton Live - a direct continuation of the round above, not a
  separate feature pass):
  - **Real bug: the preset menu's actual close mechanism.** The prior
    round's capture-phase-listener fix was correct but never took visual
    effect - `header.css`'s `.preset-menu { display: flex; ... }` was
    unconditional, and author CSS always wins over the browser's own
    `[hidden] { display: none }` UA rule at equal specificity, so JS
    toggling `menu.hidden` never actually hid the element. Fixed with a
    `.preset-menu[hidden] { display: none; }` override (higher
    specificity than the plain class).
  - **A/B relocated** from beside PRESET (where a long preset name
    visibly shifted the buttons sideways - a real bug) to its own row
    directly under "NOSTALGIA AUDIO", larger letters, fully decoupled
    from the preset label's width.
  - **Module power indicator** changed from a background-matching
    (nearly invisible) dot to a real LED: red when enabled, dark grey/
    black when disabled.
  - **EQ redesigned** from the DARK/PHONE/AIR five-stage morph to an
    always-on, steep two-cut "telephone band" filter (HP+LP, each
    48dB/octave via 4 cascaded 2nd-order stages), modelled directly on a
    real reference plugin's (FabFilter Pro-Q3) cut-band settings - see
    docs/DSP_EQ.md's new "Redesign" section for the exact anchor
    frequencies/Q and full reasoning. Knob relabelled TONE -> PHONE TONE;
    displayed value remapped to -50%..+50% (frontend-only - the
    underlying `eq` parameter is unchanged, still 0..100%/default 50%,
    so no schema migration was needed). **A real, significant, confirmed-
    intentional consequence**: because the centre/default anchor's own HP
    corner sits at 461Hz, EQ at its resting default now removes
    essentially all content below ~461Hz - a major departure from every
    other module's "transparent by default" contract, explicitly
    confirmed as wanted (not a bug) before shipping. This broke several
    pre-existing full-chain low-frequency regression tests (PAN+VERB
    bass-centring, PITCH bass-stability-in-context, the full-audit
    low-end integration tests) purely because EQ now legitimately removes
    the bass those tests fed it before it ever reached PAN/VERB/PITCH -
    fixed by explicitly disabling EQ in those specific tests (they test
    PAN/VERB/PITCH's own bass behaviour, not "does EQ preserve bass",
    which is no longer the right question to ask of this module). A new
    resonance/anchor-accuracy test suite verifies the three anchors match
    the specified reference numbers exactly and the slope is genuinely
    steep (measured 48.1dB/octave).
  - **PITCH tonality-limit added** (8000Hz, Signalsmith Stretch's own
    documented example value) to reduce audible "smearing"/wobble at
    large +/-ST amounts - see docs/DSP_PITCH.md's "Bass stability /
    wobble analysis" follow-up note. Existing bass-stability benchmark
    (40-120Hz) re-ran green, unaffected as expected (tonality limit only
    changes treatment far above that range); the wobble reduction itself
    was not separately re-measured this round.
  - **VERB decay left unchanged** - DEEP (100%) already measures ~3.5s
    RT60, matching what was asked for; pushing it further would risk the
    same `verbLineFeedbackGainMax` stability ceiling the original VERB
    round already found and documented, and would need its own
    measurement cycle to do safely.
  - **IMAGE's FIELD pad + tri-scale hidden** (not deleted - see
    index.html's `hidden` attribute and comment) pending a fuller
    redesign (bipolar -100%..+100% mono<->stereo, requested but not
    started this round) - `imageTilt` keeps its DSP and parameter intact,
    just has no visible UI control right now.
  - **`EDITOR_WANTS_KEYBOARD_FOCUS` changed `TRUE` -> `FALSE`** - a real
    bug: standard DAW transport shortcuts (e.g. Space for play/stop) were
    being swallowed whenever the plugin editor had focus. An insert
    effect with no MIDI input has no legitimate ongoing need for the host
    to route keyboard input to it.
  - Footer spacing (SIGNAL PATH caption, bolt clearance around the
    OUTPUT meter's "+3" label) and header title spacing tuned per direct
    visual feedback.
  - **Not started this round** (flagged, waiting on the user): PREAMP and
    SAT "redo" requests - too vague to act on without a concrete
    reference to compare against (no access to third-party plugins in
    this environment to audition them live), user said they'll describe
    the specific problem in more detail. PAN's requested redesign into a
    literal tempo-synced auto-panner (nested nested-knob UI, width +
    speed with musical-division snapping) - the largest of the requested
    changes, a wholly new feature (host BPM access, new UI widget), not
    started. IMAGE's bipolar mono<->stereo redesign - not started beyond
    hiding the now-stale tri-scale/FIELD UI.
  - All Debug/Release builds clean (0 warnings); full test suite green
    after each change, including the newly-adapted EQ test suite and the
    four full-chain low-frequency tests updated to isolate EQ.

## Next steps (not started - waiting for a separate go-ahead)

Presets browser, copy protection, licensing system - see
[docs/RELEASE.md](docs/RELEASE.md) for the pre-public-release checklist.
All 7 modules' DSP is now complete (see
[docs/DSP_PREAMP.md](docs/DSP_PREAMP.md), [docs/DSP_EQ.md](docs/DSP_EQ.md),
[docs/DSP_SAT.md](docs/DSP_SAT.md), [docs/DSP_PITCH.md](docs/DSP_PITCH.md),
[docs/DSP_PAN.md](docs/DSP_PAN.md), [docs/DSP_VERB.md](docs/DSP_VERB.md)
and [docs/DSP_IMAGE.md](docs/DSP_IMAGE.md)).
