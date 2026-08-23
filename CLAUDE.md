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
- **One primary knob per processor.** Each of the 7 modules gets exactly
  one large interactive knob bound to its one public parameter. Any
  secondary read-out (the Preamp filter-position lines, the tri-point
  scales on EQ/Pitch/Pan/Verb/Imager) is purely a derived visual computed
  from that same parameter's value in JS - never a second control, never a
  second parameter. PAN's tri-scale (`MONO`/`NATURAL`/`WIDE`) is a stereo-
  *width* scale, not L/R balance - see the PAN section below.
- **7 immutable public parameters.** See below - the UI must never grow an
  8th knob or a new APVTS parameter without a deliberate, separate
  decision.
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

### The 7 public parameters (stable IDs, do not rename)

`preamp`, `eq`, `saturation`, `pitch`, `panorama`, `reverb`, `imager` -
see [`Source/Parameters/ParameterIDs.h`](Source/Parameters/ParameterIDs.h).
Six are `0..100%` `AudioParameterFloat`s: default `50%` for `eq` (its
centred/flat "PHONE" position) and `panorama` (its centred/neutral
"NATURAL" position - see below), `0%` for the other four (fully off,
matching a console where drive/saturation/space/image all start at
zero). `pitch` is the one exception - a discrete `AudioParameterInt`,
`-12..+12` semitones, step 1, default `0` (25 fixed positions, no cents -
see [docs/DSP_PITCH.md](docs/DSP_PITCH.md)'s "Parameter and state
migration" section). `panorama`'s internal ID is a naming holdover - the
module it drives is a stereo *width* control, not an L/R balance pan, see
below. Low Cut / High Cut are **not** separate parameters - they are
internal to `Source/DSP/PreampProcessor`, derived entirely from `preamp`
(see [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md)).

## PREAMP + EQ + SAT + PITCH + PAN DSP (the only modules with real audio processing so far)

Chain order: `Input -> PREAMP -> EQ -> SAT -> PITCH -> PAN -> (future
modules) -> Output` - `PluginProcessor::processBlock()` calls
`preampProcessor.process()`, then `eqProcessor.process()`, then
`satProcessor.process()`, then `pitchProcessor.process()`, then
`panoramaProcessor.process()`, in that order. Total plugin latency is the
**sum** of every stage's own latency (`preampProcessor.getLatencySamples()
+ eqProcessor.getLatencySamples() + satProcessor.getLatencySamples() +
pitchProcessor.getLatencySamples() + panoramaProcessor.getLatencySamples()`,
computed in `prepareToPlay`) - EQ and PAN both always contribute 0; PITCH
always contributes a nonzero, sample-rate-proportional amount (unlike
PREAMP/SAT, it does not drop to 0 at high sample rates - see
docs/DSP_PITCH.md's "Fixed latency" section).

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
a Mid/Side stereo-*width* control: `MONO (0%) <- NATURAL (50%) -> WIDE
(100%)`. Mid (`0.5*(L+R)`) is passed through completely untouched -
which is what makes two things *provable*, not just measured: mono
fold-down (`(Lout+Rout)/2`) is bit-identical to the input's own mono sum
at every width setting, and NATURAL (50%) is a true identity transform
even though a fixed 180Hz crossover filter always splits the Side signal
into low/high bands (their gains are both exactly 1.0 at 50%, and
`sideLow+sideHigh == Side` algebraically regardless of the filter's own
response, so the split cancels back out). The low band gets a much
smaller width ceiling than the high band (1.15x vs 1.8x at 100%) so bass
stays close to centre even at full WIDE - frequency-dependent width, not
uniform. No delay-based widening (Haas), no chorus, no random modulation,
no oversampling - a pure gain/filter morph, zero added latency. Same
`panoramaEnabled`-driven crossfade bypass pattern as EQ (no delay-
alignment needed, PAN has no latency to align against).

**Reverb and Imager remain a strict passthrough** - do not add DSP to
either without a separate, deliberate decision. `Source/DSP/Reverb.h` /
`Imager.h` are still empty placeholders.

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
sound calibration) + EQ DSP + SAT DSP + PITCH DSP + PAN DSP. Reverb and
Imager remain deliberately passthrough.**

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
- **PAN DSP** (Mid/Side stereo width, see [docs/DSP_PAN.md](docs/DSP_PAN.md)):
  width mapping matches the product brief's target curve at 0/25/50/75/
  100% (measured 0/0.5/1.0/1.41/1.82 vs targets 0/0.5/1.0/1.4/1.8);
  NATURAL (50%) measured as a near-identity transform (RMS diff 7.9e-9,
  max diff 6.0e-8) despite the crossover filter always running, which
  the topology makes provable rather than just measured (Mid untouched,
  Side low/high gains both exactly 1.0 at 50%); MONO (0%) gives a
  correct `(L+R)/2` sum (max diff <1e-4) with anti-phase material
  cancelling as physically expected, not "fixed"; frequency-dependent
  Side gain confirmed low (60Hz: 1.27x) widens far less than high
  (5kHz+: 1.80x) at WIDE, with one honestly-documented minor artifact
  (a ~5.5% Side-gain overshoot right at the 200Hz crossover under
  differential gain - smooth, not a discontinuity, not corrected this
  pass); mono fold-down provably unchanged by width at every setting
  (Mid is never modified); mono buses (numChannels<2) left completely
  untouched at every width, no fabricated stereo; centred bass stays
  centred under WIDE even with decorrelated stereo highs present;
  correlation degrades gracefully (0.965->0.875 at 100% on correlated
  material); no runaway gain (peak stays under 0.6 on a deliberately
  Side-heavy source across the full width range); click-free automation
  and bypass; zero added latency confirmed at every sample rate/block
  size/width/enabled state, and confirmed the total plugin latency is
  unchanged from the pre-PAN PITCH baseline; PITCH+PAN integration
  (identical-L/R through PITCH's two-engine architecture stays nearly
  identical through PAN at NATURAL); full 5-module chain integration;
  backward-compatible state migration from the old, DSP-less 0% default
  to the new 50% (NATURAL) default (schema v4).
- **Real VST3 host validation.** A purpose-built harness using the real
  `AudioPluginFormatManager`/`VST3PluginFormat` hosting code (not
  `UNI76AudioProcessor` directly) loaded the built `.vst3`, confirmed all 7
  parameters (plus one host-added generic bypass parameter, standard VST3
  hosting behaviour, not a UNI 76 parameter), confirmed PITCH defaults to
  0 ST and PAN defaults to 50% NATURAL, drove PREAMP/EQ/SAT/PITCH/PAN
  through real `processBlock()` calls across the full `-12..+12` ST range,
  the full 0-100% PAN width range, and through a combined full-chain
  setting with real bass, confirmed measurable/bounded/finite output,
  correct correlation behaviour at WIDE, and correct total (summed)
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

Not verified (say so plainly rather than guessing):

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
  the other 6 modules' knobs were not re-exercised this way (unchanged
  since the PREAMP/EQ/SAT pass, not expected to need it). PAN's new
  DSP/default-value work (this entry) was likewise not exercised this
  way - verified via the non-GUI VST3 host harness plus the DSP/APVTS
  test suite, not a live mouse-driven WebView2 session. The UI-visible
  change for PAN (knob resting position, `50%` label) is a static markup
  default (`index.html`'s inline `--knob-angle`/`.knob__value`), the same
  kind of change PITCH's own default-position markup got, just not
  independently re-verified by screenshot this time.

## Next steps (not started - waiting for a separate go-ahead)

DSP for the remaining 2 modules (Reverb, Imager - PREAMP, EQ, SAT, PITCH
and PAN are done, see [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md),
[docs/DSP_EQ.md](docs/DSP_EQ.md), [docs/DSP_SAT.md](docs/DSP_SAT.md),
[docs/DSP_PITCH.md](docs/DSP_PITCH.md) and [docs/DSP_PAN.md](docs/DSP_PAN.md)),
presets browser, copy protection, licensing system. See
[docs/RELEASE.md](docs/RELEASE.md) for the pre-public-release checklist.
