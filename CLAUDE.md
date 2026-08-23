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
  second parameter. PAN's tri-scale (`ORIGINAL`/`WIDE`/`MOTION`) is a
  stereo width + slow ear-to-ear motion scale, not L/R balance - see the
  PAN section below.
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
centred/flat "PHONE" position), `0%` for the other five - including
`panorama` (fully off/ORIGINAL, matching a console where drive/
saturation/width/space/image all start at zero). `pitch` is the one
exception - a discrete `AudioParameterInt`, `-12..+12` semitones, step 1,
default `0` (25 fixed positions, no cents - see
[docs/DSP_PITCH.md](docs/DSP_PITCH.md)'s "Parameter and state migration"
section). `panorama`'s internal ID is a naming holdover - the module it
drives is a stereo width + slow motion control, not an L/R balance pan,
see below. Low Cut / High Cut are **not** separate parameters - they are
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

## Next steps (not started - waiting for a separate go-ahead)

DSP for the remaining 2 modules (Reverb, Imager - PREAMP, EQ, SAT, PITCH
and PAN are done, see [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md),
[docs/DSP_EQ.md](docs/DSP_EQ.md), [docs/DSP_SAT.md](docs/DSP_SAT.md),
[docs/DSP_PITCH.md](docs/DSP_PITCH.md) and [docs/DSP_PAN.md](docs/DSP_PAN.md)),
presets browser, copy protection, licensing system. See
[docs/RELEASE.md](docs/RELEASE.md) for the pre-public-release checklist.
