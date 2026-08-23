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
  second parameter.
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
centred/flat "PHONE" position), `0%` for the other five (fully off,
matching a console where drive/saturation/width/space/image all start at
zero). `pitch` is the one exception - a discrete `AudioParameterInt`,
`-12..+12` semitones, step 1, default `0` (25 fixed positions, no cents -
see [docs/DSP_PITCH.md](docs/DSP_PITCH.md)'s "Parameter and state
migration" section). Low Cut / High Cut are **not** separate parameters -
they are internal to `Source/DSP/PreampProcessor`, derived entirely from
`preamp` (see [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md)).

## PREAMP + EQ + SAT + PITCH DSP (the only modules with real audio processing so far)

Chain order: `Input -> PREAMP -> EQ -> SAT -> PITCH -> (future modules) ->
Output` - `PluginProcessor::processBlock()` calls `preampProcessor.process()`,
then `eqProcessor.process()`, then `satProcessor.process()`, then
`pitchProcessor.process()`, in that order. Total plugin latency is the
**sum** of every stage's own latency (`preampProcessor.getLatencySamples()
+ eqProcessor.getLatencySamples() + satProcessor.getLatencySamples() +
pitchProcessor.getLatencySamples()`, computed in `prepareToPlay`) - EQ
always contributes 0; PITCH always contributes a nonzero, sample-rate-
proportional amount (unlike PREAMP/SAT, it does not drop to 0 at high
sample rates - see docs/DSP_PITCH.md's "Fixed latency" section).

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

**Panorama, Reverb and Imager remain a strict passthrough** - do not add
DSP to any of them without a separate, deliberate decision.
`Source/DSP/Panorama.h` / `Reverb.h` / `Imager.h` are still empty
placeholders.

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
sound calibration) + EQ DSP + SAT DSP + PITCH DSP. Panorama, Reverb,
Imager remain deliberately passthrough.**

Verified on this machine (Windows, Visual Studio Community 2026 /
MSVC 19.51):

- Clean configure + build for both `windows-debug` and `windows-release`
  presets, **zero compiler warnings**, including `juce_dsp` (linked for
  PREAMP's and SAT's independent oversampling instances; EQ itself needs
  no oversampling) and the vendored Signalsmith Stretch/Linear headers
  (PITCH) - their own upstream warnings are suppressed at the include
  site (`#pragma warning (push, 0)` / GCC diagnostic push around the
  vendored include in `PitchProcessor.cpp`), not patched into the
  vendored source itself.
- Real `.vst3` produced at
  `build/windows-release/Source/Plugin/UNI76_artefacts/Release/VST3/UNI 76.vst3`.
- All `UNI76Tests` (JUCE `UnitTest`-based) pass in both Debug and Release,
  including the full PREAMP DSP suite (see
  [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md)), the full EQ DSP suite (see
  [docs/DSP_EQ.md](docs/DSP_EQ.md)), the full SAT DSP suite (see
  [docs/DSP_SAT.md](docs/DSP_SAT.md)), and the full PITCH DSP suite (see
  [docs/DSP_PITCH.md](docs/DSP_PITCH.md)): PREAMP transparency near
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
- **Real VST3 host validation.** A purpose-built harness using the real
  `AudioPluginFormatManager`/`VST3PluginFormat` hosting code (not
  `UNI76AudioProcessor` directly) loaded the built `.vst3`, confirmed all 7
  parameters (plus one host-added generic bypass parameter, standard VST3
  hosting behaviour, not a UNI 76 parameter), confirmed PITCH defaults to
  0 ST, drove PREAMP/EQ/SAT/PITCH through real `processBlock()` calls
  across the full `-12..+12` ST range and through a combined full-chain
  setting with real bass, confirmed measurable/bounded/finite output and
  correct total (summed) latency, ran automation sweeps (incl. rapid
  discrete PITCH jumps) during continuous audio, round-tripped state
  through a real `getStateInformation()`/`setStateInformation()`
  save+restore, and confirmed identical behaviour from a `.vst3` copy in a
  completely isolated directory (no `Resources/Web` nearby), proving the
  UI resources and all four DSP modules' code are genuinely self-contained
  in the binary. `preampEnabled`/`eqEnabled`/`saturationEnabled`/
  `pitchEnabled` bypass are each verified at the C++ level
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
- **Manual mouse-driven interaction** (literally dragging a knob with a
  mouse) wasn't exercised - the validation harness drives parameter
  changes through the host API, which exercises the same JS<->JUCE sync
  path a real drag would, but isn't a substitute for a human trying the
  actual pointer/wheel/keyboard interactions in a real DAW. This applies
  in particular to PITCH's new discrete-stepped knob behaviour (drag/
  wheel/keyboard snapping to exactly 1 semitone per gesture) - that logic
  was verified by reading `knob.js`'s discrete-mode code paths, not by a
  live pointer-drag session.
- **The WebView2 editor itself was not opened this session** - unlike the
  previous PREAMP/EQ/SAT validation pass, this session's VST3 host
  validation harness deliberately did not instantiate `createEditor()`:
  doing so reliably in a headless console harness (no running message
  loop driving WebView2's async initialisation) was judged too fragile to
  automate safely in the time available, versus a real risk of hanging
  the validation run. The `index.html`/`knob.js`/`app.js` changes for
  PITCH's discrete knob were verified by code inspection and by the
  DSP/parameter-level test suite (ARIA min/max/step, default position,
  format-value logic), not by a live render.

## Next steps (not started - waiting for a separate go-ahead)

DSP for the remaining 3 modules (Panorama, Reverb, Imager - PREAMP, EQ,
SAT and PITCH are done, see [docs/DSP_PREAMP.md](docs/DSP_PREAMP.md),
[docs/DSP_EQ.md](docs/DSP_EQ.md), [docs/DSP_SAT.md](docs/DSP_SAT.md) and
[docs/DSP_PITCH.md](docs/DSP_PITCH.md)), presets browser, copy
protection, licensing system. See [docs/RELEASE.md](docs/RELEASE.md) for
the pre-public-release checklist.
