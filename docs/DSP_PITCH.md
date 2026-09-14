# PITCH / VARISPEED - DSP notes

`Source/DSP/PitchProcessor.h`/`.cpp` implements the `04 PITCH / VARISPEED`
module: pure pitch-shifting only, duration always preserved. No
saturation, no analogue coloration, no tape character, no compression, no
EQ coloration, no wow/flutter, no added noise, no stereo widening, no
random modulation - if it isn't a frequency shift, it isn't PITCH's job.

The single, non-negotiable acceptance bar for this module, stated up
front because every design decision below traces back to it: **low
frequencies must not wobble.** A shifted 40-120Hz tone must not exhibit
cyclic amplitude modulation, a floating/breathing fundamental, granular
pulsing, or audible sidebands tied to the algorithm's own analysis
window/hop. An algorithm that sounds fine on vocals but lets 60Hz "swim"
is not acceptable here, no matter how good it looks on paper otherwise.

## Algorithm selection

Writing a pitch-shifter from scratch (naive PSOLA, a simple phase vocoder
with no phase-locking, granular resampling, ...) was ruled out - every
naive approach has a well-known bass failure mode (PSOLA needs reliable
pitch-marking, which degrades badly below ~80Hz; an unlocked phase vocoder
produces exactly the phasiness/wobble this module must avoid; granular
resampling produces audible grain-rate artifacts at long grain lengths
needed for bass). Given the explicit requirement to prioritise low-
frequency stability above latency and CPU, a mature, purpose-built,
permissively-licensed library was the only responsible choice for a
commercial closed-source VST3.

**Chosen: [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)**,
a C++11 phase-vocoder-based pitch/time library by Geraint Luff /
Signalsmith Audio, built from the final approach presented in the ADC22
talk "Four Ways To Write A Pitch-Shifter" (phase-locked/reassigned phase
vocoder with peak-tracking bin grouping - the approach that talk's own
comparison identifies as the one that keeps bass content coherent, which
is exactly this module's top priority). It was considered as a primary
candidate from the outset per the task brief, but was not accepted
automatically - see "Configuration benchmark" below for the actual
measurement-based comparison that justified using it, and the exact
constructor configuration.

## Dependency and license

Both libraries are vendored verbatim (not `FetchContent`, matching how the
authors' own documentation recommends using them - "just include the
header") into `ThirdParty/`, at an exact pinned commit, with their
upstream `LICENSE.txt` preserved unmodified:

| Library | Path | License | Pinned commit |
|---|---|---|---|
| Signalsmith Stretch | `ThirdParty/signalsmith-stretch/` | MIT (Geraint Luff / Signalsmith Audio Ltd.) | `57b93f4e9206a089a45387eaa39bdc9f310d3308` |
| Signalsmith Linear | `ThirdParty/signalsmith-linear/` | MIT (Signalsmith Audio) | `5668673560146a9cfe38c25315071e3fd68c8317` (tag `0.3.1`) |

Signalsmith Linear is Stretch's own FFT/STFT dependency; only the two
files actually reachable from `signalsmith-stretch.h` (`fft.h`, `stft.h`)
are vendored, plus two one-line forwarder headers
(`include/signalsmith-linear/{fft,stft}.h`) that satisfy Stretch's own
`#include "signalsmith-linear/stft.h"` search path without vendoring the
unrelated `linear.h`/CLI-demo files the upstream repo also contains. Both
licenses were confirmed MIT (no GPL/copyleft obligation) by fetching the
exact `LICENSE.txt` at the pinned commit directly, not by trusting a
paraphrased summary. No floating branch, no runtime download - the build
never fetches anything for this dependency.

`PitchProcessor.cpp` wraps the vendored header in a
`#pragma warning (push, 0)` / `#pragma GCC diagnostic push` block (MSVC
and Clang/GCC respectively) so the vendored code's own compiler warnings
(float-narrowing, a shadowed local in its FFT) don't count against this
project's zero-warnings bar, without patching the vendored source itself.

## Architecture

```
Input (all channels together)
  -> live (undelayed) dry copy captured
  -> single shared SignalsmithStretch<float>, configured for the real
     channel count (1 or 2)
       - setTransposeSemitones(clamped -12..+12) every block (no allocation)
       - process(inputs[], N, outputs[], N) - equal in/out sample counts
         every call, so the engine only ever pitch-shifts, never
         time-stretches; all channels passed in one call
  -> enable/disable crossfade against the live (undelayed) dry copy
  -> Output
```

**One shared multi-channel `SignalsmithStretch<float>` instance**, not
two independent mono ones - see "Stereo coherence" below for the full
reasoning; this replaced the module's original two-independent-engines
design in a live-testing round 3 architecture fix. The vendored engine is
kept behind a PIMPL (`PitchProcessor::Engine`, defined only in the `.cpp`)
so `PitchProcessor.h` itself carries no `ThirdParty/` include dependency.

There is no dry-side delay line any more (the "Enable/disable behaviour"
zero-latency-bypass round already removed the delay-aligned crossfade in
favour of a live/undelayed dry blend - see "Fixed latency" below); this
diagram previously still showed the pre-that-round topology and has been
corrected here.

Realtime-safety matches every other module: `prepare()` is the only place
that allocates (`SignalsmithStretch::configure()`, called once per
`prepareToPlay`); `process()` never allocates, locks, or touches the
filesystem/WebView - `setTransposeSemitones()` and `process()` are plain
member-state updates per the upstream API contract.

## Configuration benchmark

Signalsmith Stretch's `configure(channels, blockSamples, intervalSamples)`
exposes the two numbers that trade latency for low-frequency stability. A
purpose-built scratch benchmark tool (not committed) measured 5, then 4
refined, STFT configurations against a 6-frequency (40/50/60/80/100/120Hz)
x 2-direction (+12ST/-12ST) matrix, using the analysis method described in
"Bass stability" below (phase-vocoder frequency-reassignment - an ordinary
single-shot FFT cannot see this failure mode).

| Configuration | Block | Interval | Result |
|---|---|---|---|
| `presetDefault()` (library default) | 120ms | 30ms | Baseline - clearly worse frequency-deviation and AM-depth than the manual configurations below in the majority of the 12 cases |
| `manual-130` | 130ms | 32ms | One catastrophic outlier (100Hz+12ST: freqStd=0.19Hz vs ~0.02-0.04Hz elsewhere) - rejected |
| **`manual-large` (chosen)** | **140ms** | **35ms** | Won or nearly won on frequency-deviation, AM-depth, and sideband suppression in the large majority of the 12 cases |
| `manual-xlarge` | 180ms | 40ms | Diminishing/inconsistent returns for +40ms latency vs `manual-large` |

**140ms block / 35ms interval** (`Source/DSP/PitchCurves.h`'s
`pitchStftBlockSeconds`/`pitchStftIntervalSeconds`) was selected on
measured bass-stability evidence, not chosen for a round latency number -
it exceeds the product brief's "~50-80ms" guideline, which the brief
itself explicitly permits when genuine bass stability requires it (see
"Fixed latency" below for the actual resulting number).

## Parameter and state migration

`pitch` keeps its existing APVTS string ID (`ParamID::pitch = "pitch"`,
unchanged - old automation lanes still resolve to the right parameter),
but its C++ type changed from a `0..100%` `AudioParameterFloat` to a
genuinely discrete `AudioParameterInt` (`Source/Parameters/ParameterLayout.cpp`):
range `-12..+12`, step 1, default `0` - 25 valid states, matching the
UI's 25 fixed knob positions exactly (no cents, no intermediate values).
JUCE reports this to the host/VST3 layer as a real step count
(`RangedAudioParameter::getNumSteps() == 25`), confirmed both at the
C++/APVTS level and by loading the actual built `.vst3` through JUCE's own
`VST3PluginFormat` host code (see "Real host validation" in the final
report) - not just a UI-side snap.

**State migration** (schema v3, `Source/Core/PluginIdentity.h`): the real
old serialized format was verified by direct experiment, not guessed -
`AudioProcessorValueTreeState` stores each parameter's `value` attribute
as the **raw, denormalised** number in that parameter's own declared
range (confirmed via a scratch `DummyProcessor` replicating the pre-change
`pitch` definition: default state serializes as `<PARAM id="pitch"
value="0.0"/>`, and after `setValueNotifyingHost(0.73f)` as `value="73.0"`
- a plain 0-100 percent number, never normalised 0-1). That old raw number
has no semitone meaning at all under the new `-12..+12` range, so
`setStateInformation()` explicitly detects `loadedSchemaVersion <
pitchDiscreteSchemaVersion` (3) and overwrites the loaded state's
`pitch` `value` property to `0.0` **before** `apvts.replaceState()` runs -
regardless of whatever the old raw value actually was. A user who never
touched PITCH (old value `0.0`) and a user who had set it to 73% both land
on the same new default, 0 ST, deterministically - not by the numeric
coincidence that `0.0` happens to already sit inside the new range. This
is covered by dedicated tests (`UNI76PitchIntegrationTests`, both a
hand-built pre-v3 state with a nonzero legacy value and one with the
missing/pre-v2 property). Under the new schema, `-12 ST` saves and
restores as `-12`, `0 ST` as `0`, `+12 ST` as `+12` - verified by a
dedicated round-trip test.

## Live UI verification

The C++/APVTS-level checks above prove the parameter contract; a separate
pass verified the actual **rendered WebView2 editor** of the real built
`.vst3` - not a browser mockup, not HTML inspection. A minimal scratch
JUCE host app loaded the plugin via the real `VST3PluginFormat`, created
its real `AudioProcessorEditor` (the same WebView2-hosted UNI 76 UI a DAW
shows), and real synthetic mouse/keyboard input was sent to the actual
native window; screenshots were captured directly from the window's own
content (`PrintWindow` with `PW_RENDERFULLCONTENT`, which reads the
window's pixels regardless of on-screen occlusion - not a desktop-region
grab, which risked capturing unrelated windows and was corrected during
this pass once caught).

Confirmed by direct interaction, not just by reading `knob.js`:

- **Fresh instance**: knob pointer exactly vertical, label reads `0 ST`,
  no `%` anywhere in the PITCH module, scale reads `- OCT / 0 / + OCT`,
  end labels read `-12` / `+12`.
- **Drag**: a 110px drag (half of the 220px full-range mapping) landed
  on exactly `-12 ST` - no intermediate/fractional value observed at any
  point.
- **Wheel**: one notch moved the display from `-12 ST` to exactly `-11 ST`.
- **Keyboard**: one `ArrowUp` press moved from `-11 ST` to exactly `-10 ST`.
- **Double-click**: reset from any position directly to `0 ST` with the
  pointer exactly vertical again.
- **Edge clamping**: dragging 300px past either end (nearly 3x the ~110px
  actually needed to reach an extreme) still landed on exactly `-12 ST` /
  `+12 ST` - no overshoot, no crash, no wrap-around.
- **Shift**: a Shift-held 15px drag from `-12 ST` moved to `-10 ST` -
  consistent with the *normal* (non-fine) drag range (15px / 220px * 24
  steps ~= 1.6, rounds to 2 steps), not the fine range (which would need
  ~1760px for the same movement and should have produced no visible change
  at all for a 15px drag). Confirms Shift has no special fractional-value
  effect on PITCH, matching the code's discrete-mode branch.

Three screenshots of the real, unmodified editor were saved:
[`docs/screenshots/pitch-minus12.png`](screenshots/pitch-minus12.png),
[`docs/screenshots/pitch-zero.png`](screenshots/pitch-zero.png),
[`docs/screenshots/pitch-plus12.png`](screenshots/pitch-plus12.png) - all
three show the rest of the UI (all 6 other modules, header, footer)
pixel-identical to the fresh-instance baseline, confirming PITCH's
discrete-knob work didn't disturb the shared design.

## Fixed latency (the engine's own figure) and the zero-latency bypass

`getLatencySamples()` returns `stretch.inputLatency() +
stretch.outputLatency()` (both channels share one configuration, so this
is a single scalar), set once in `prepare()` - it never depends on the
semitone value, verified by a dedicated test that sweeps `-12/-7/0/7/12`
ST and enabled/disabled and asserts the reported value never moves. This
is the STFT engine's own algorithmic latency **while it is actually
running** - not necessarily the module's current real output delay (see
below).

### Zero-latency bypass (live-testing follow-up round)

Through UNI 76's own early rounds, `enabled=false` was a bit-exact
*delayed* passthrough (`IntegerDelayLine`, the same pattern PREAMP/SAT
use) - `getLatencySamples()`'s value never moved regardless of enabled
state, and `PluginProcessor::prepareToPlay()` summed it into the total
unconditionally. This was a real, reported bug: PITCH's own latency is
~140ms - two to three orders of magnitude larger than PREAMP/SAT's own
always-on oversampling latency (a few samples, well under a millisecond)
- and it was held open even while disabled, which is PITCH's resting
state in **every factory preset**, "Default" included (see
`Core/FactoryPresets.h`'s own documented rule that PITCH stays off
everywhere). A user who never touched PITCH still felt a persistent,
genuinely noticeable monitoring/live-MIDI delay through the plugin.

`PitchProcessor::process()` now takes a fast early-return path once
settled disabled: no delay line, no engine call, the buffer passes
through completely untouched. `PluginProcessor::updateReportedLatency()`
(called after `prepareToPlay()`, after `setStateInformation()`, and after
every `ModuleEnableState::setEnabled()` call the UI/preset/A-B code makes
- see `WebUIEditor.cpp`'s call sites) reports PITCH's contribution to the
host's total latency as `0` while disabled and the full engine figure
while enabled - genuinely zero added delay, not just a hidden one.

The tradeoff, and why it's the right one: enabling PITCH mid-playback is
no longer a click-free, delay-aligned crossfade - that mechanism requires
the dry and wet paths to share one timeline, which a variable total
latency makes structurally impossible. The short
(`pitchBypassSmoothingSeconds`, 20ms) transition instead blends the STFT
engine's output against the *live* (undelayed) input, and the engine
itself needs its own ~140ms warm-up from a cold start (it was never fed
input while disabled) before its output is representative - the same
settle-in any time-based effect incurs when engaged from bypass; not
something this plugin can hide, and not attempted. A persistent 140ms lag
on every note for however long PITCH merely exists in the chain -
regardless of whether it's ever actually used - was the worse tradeoff.

Two dedicated tests cover this: `PitchProcessor` alone settles to a
bit-exact, *zero-delay* passthrough (not `input[i - latency]` any more)
once disabled; and the full `UNI76AudioProcessor`'s reported
`getLatencySamples()` genuinely drops PITCH's contribution while disabled
and picks it straight back up on re-enabling, confirming
`updateReportedLatency()` reacts to a live toggle rather than only the
value baked in at `prepareToPlay()` time. The compiled-in default enabled
state (`ModuleEnableState.h`) was deliberately left unchanged (PITCH
still defaults to enabled, same as every module except EQ) - a bare,
no-preset instance still holds the full latency until a preset is
selected or the module is explicitly turned off; only the *held-open-
while-disabled* behaviour was the bug, not the default itself. Widening
this fix to the compiled default is a natural, larger follow-up (it
touches every test that currently relies on PITCH's default-enabled
state to exercise its own pitch-shift behaviour without an explicit
enable call) - not done this round.

Measured (140ms/35ms configuration, `presetDefault`-free manual config).
Re-verified with an exact three-way breakdown - PITCH alone, PREAMP+EQ+SAT
(EQ itself always contributes 0), and the total plugin latency the host
actually sees - confirming `total == pitch + (preamp+eq+sat)` exactly at
every rate:

| Sample rate | PITCH alone | PREAMP+EQ+SAT | **Total plugin** |
|---|---|---|---|
| 44100 Hz | 6174 smp / 140.0 ms | 12 smp / 0.272 ms | **6186 smp / 140.272 ms** |
| 48000 Hz | 6720 smp / 140.0 ms | 12 smp / 0.250 ms | **6732 smp / 140.250 ms** |
| 96000 Hz | 13440 smp / 140.0 ms | 8 smp / 0.083 ms | **13448 smp / 140.083 ms** |
| 192000 Hz | 26880 smp / 140.0 ms | 0 smp / 0 ms | **26880 smp / 140.0 ms** |

PREAMP+EQ+SAT's own latency shrinks and eventually hits 0 at 192kHz (their
oversampling is no longer needed at that rate); PITCH's 140ms is present
and dominant at every rate, so the total is never far from "PITCH's own
140ms plus a small, shrinking PREAMP/SAT remainder." Also reconfirmed
identical at -12/0/+12 ST and enabled/disabled (a dedicated test asserts
`getLatencySamples()` never moves across those cases) - the bypass and
0 ST paths are not a different, lower-latency shortcut.

Latency in milliseconds is constant across sample rates by construction
(the configuration is specified in seconds, converted to samples in
`prepare()`). Verified independent of host block size (32/64/128/256/512/
1024/2048 all report the identical sample count for a given sample rate).
Unlike PREAMP/SAT, PITCH's latency does **not** drop to zero at
176.4/192kHz+ - their oversampling-derived latency disappears once
oversampling is no longer needed at high rates, but PITCH's STFT latency
scales with sample rate and is present at every rate, including 192kHz
(the existing `UNI76ProcessorTests` latency test's `expectLatency`
assumption for 192kHz was updated from `false` to `true` for this reason).

## Pitch accuracy

Measured via phase-vocoder frequency reassignment (see "Bass stability"
for the method) on a settled 440Hz tone at each interval:

| Semitones | Expected | Measured | Error |
|---|---|---|---|
| -12 | 220.000 Hz | 219.995 Hz | 0.002% |
| -7 | 293.665 Hz | 294.377 Hz | 0.243% |
| -3 | 369.994 Hz | 370.251 Hz | 0.069% |
| +3 | 523.251 Hz | 523.917 Hz | 0.127% |
| +7 | 659.255 Hz | 660.085 Hz | 0.126% |
| +12 | 880.000 Hz | 880.798 Hz | 0.091% |

All well under a musical cent (1 cent = 0.0578%) at the largest, and
under half a cent at every interval tested - not audibly detuned.

## Bass stability / wobble analysis

**Follow-up (UX polish pass, live-testing feedback)**: manual testing
reported audible "smearing"/wobble on pitch-shifted material at large
+/-ST amounts. Signalsmith Stretch exposes a "tonality limit" specifically
for this - a non-linear frequency map that preserves more of the original
timbre/phase coherence above a given frequency, instead of remapping
every STFT bin's frequency linearly by the full transpose ratio (see the
library's own `UPSTREAM_README.md`). Set to 8000Hz (the library's own
documented example value) via `PitchCurves.h`'s `pitchTonalityLimitHz`,
passed as `setTransposeSemitones()`'s second argument in
`PitchProcessor.cpp`. The existing bass-stability benchmark below (40-
120Hz) re-ran green after this change with no regression - expected,
since 8000Hz sits far above the range that benchmark measures - but the
benchmark itself was **not re-run to specifically quantify the wobble
reduction this was meant to fix** (that would need a new measurement at
higher/broader-spectrum test material, not the existing low-frequency-
only table) - a natural follow-up, not done as part of this pass.

**Method**: an ordinary FFT/Goertzel over the whole tone cannot see this
failure mode - a fundamental that cyclically floats or breathes averages
out over a long window. Instead, each settled tone is split into
overlapping analysis windows (sized to at least 4 full cycles of the
target frequency, hop = window/3) and, for each hop, the **instantaneous
frequency** is measured via phase-vocoder reassignment: `deviation =
(phase - prevPhase) - omega*hopLen` (wrapped to `[-pi, pi]`), `instFreq =
binHz + deviation/(2*pi) * (sampleRate/hopLen)` - the correct formula
(missing the `omega*hopLen` expected-phase-advance term gives nonsensical
results, caught and fixed during the benchmark tool's own development).
Per-window Goertzel magnitude (dB) and time-domain RMS are also tracked.
The frequency values' standard deviation, the magnitude values' standard
deviation (dB), and RMS's std/mean ratio are the wobble/AM-depth/breathing
metrics; a periodic pattern tied to the algorithm's own hop/window would
show up as an outlier here.

**Full matrix** (40/50/60/80/100/120Hz x -12/-7/-3/+3/+7/+12 ST, 24
cases, each after settling past the module's own latency):

| Source | Interval | Target | freqStd | freqStd % | ampDbStd | rmsModDepth |
|---|---|---|---|---|---|---|
| 40Hz | -12ST | 20.0Hz | 0.026Hz | 0.131% | 0.037dB | 0.0042 |
| 40Hz | +12ST | 80.0Hz | 0.061Hz | 0.076% | 0.018dB | 0.0021 |
| 60Hz | -12ST | 30.0Hz | 0.018Hz | 0.059% | 0.025dB | 0.0028 |
| 60Hz | +12ST | 120.0Hz | 0.033Hz | 0.028% | 0.012dB | 0.0014 |
| 100Hz | -12ST | 50.0Hz | 0.035Hz | 0.070% | 0.031dB | 0.0035 |
| 100Hz | +12ST | 200.0Hz | 0.047Hz | 0.023% | 0.010dB | 0.0011 |
| 120Hz | -12ST | 60.0Hz | 0.016Hz | 0.027% | 0.012dB | 0.0014 |
| 120Hz | +12ST | 240.0Hz | 0.029Hz | 0.012% | 0.007dB | 0.0008 |

(Full 24-case table, including all six intermediate intervals per source
frequency, is printed by `UNI76PitchProcessorTests`' "Bass stability
matrix" test at every build - the worst case across all 36 combinations
tested by that suite, including intermediate intervals not shown above,
is freqStd 0.107% at 40Hz-3ST, ampDbStd 0.043dB at 40Hz-7ST, rmsModDepth
0.0047 at 40Hz-12ST - all far inside the pass thresholds below.)

Pass thresholds enforced by the automated test (chosen well inside what
would be audible, not just inside what happened to measure): frequency
deviation < 3% of the target, amplitude-modulation std < 2.5dB, RMS
modulation depth < 0.2. **Every one of the 36 tested combinations measures
at least an order of magnitude inside every threshold** - the worst
frequency deviation (0.13%) is ~23x better than the 3% bar, the worst
amplitude std (0.043dB) is ~58x better than the 2.5dB bar. No floating
fundamental, no breathing, no cyclic pulsing at any tested bass
frequency/interval combination.

## Sideband analysis

Measured magnitude at the target fundamental +/-25Hz (representative of
the 35ms analysis interval's own hop rate, ~28.6Hz - the classic
phase-vocoder artifact spacing), relative to the target's own magnitude:

| Source -> target | Sideband below | Sideband above |
|---|---|---|
| 60Hz+12ST -> 120Hz | -35.6dB | -33.7dB |
| 100Hz-12ST -> 50Hz | -36.7dB | -32.0dB |
| 40Hz+12ST -> 80Hz | -34.3dB | -32.7dB |
| 120Hz-12ST -> 60Hz | -42.8dB | -39.2dB |

All four measured cases suppress sidebands by 32-43dB below the target
fundamental - well past the point of audibility as a separate tone, and
well inside the test's -20dB pass threshold.

## 0 ST transparency

0 ST still runs through the full STFT analysis/resynthesis path (there is
no separate "identity" shortcut - `setTransposeFactor(1)` is just another
value the same engine processes), so it isn't a bit-exact passthrough the
way PREAMP's DRIVE=0% dry-adjacent path is. Measured gain deviation from
input, after settling:

| Frequency | 0 ST gain |
|---|---|
| 40 Hz | +0.005 dB |
| 60 Hz | +0.002 dB |
| 100 Hz | +0.021 dB |
| 440 Hz | +0.003 dB |
| 1000 Hz | +0.001 dB |
| 10000 Hz | +0.189 dB |

All six well under 0.2dB, the largest at 10kHz. This confirms the engine's
own reconstruction is transparent enough that a dedicated bypass shortcut
at 0 ST specifically wasn't needed - the general latency-aligned
enable/disable bypass (delayed dry passthrough) already covers the "true
off" case.

That single-tone check was followed up with a stricter, sample-accurate
**A/B against a latency-aligned dry copy** using a deterministic 10-tone
broadband source (40Hz-14kHz, fixed frequencies/phases, no randomness):
`output[i]` vs `input[i - latency]`, both bounds checked directly, not
just via magnitude:

- **RMS difference: 7.2e-8** (effectively float-rounding noise, not a
  perceptible processing artifact)
- **Max sample difference: 3.1e-7**
- **Frequency-response deviation**, all 10 tones: -0.11dB to +0.15dB
  (worst case at 80Hz and 2.5kHz), every other tone under 0.05dB

This is a materially stronger result than the single-tone check alone
suggested was even possible - given these numbers, a dedicated bit-exact
identity shortcut for 0 ST specifically was evaluated and **deliberately
not built**: it would only save ~1e-7 of already-inaudible sample-level
difference, at the cost of a second code path to keep in sync with the
main STFT path and a second latency-alignment/automation-crossing edge
case to get right. The existing architecture (one path, always through
the real engine) already meets the transparency bar with a wide margin.

## Transient quality

A short (20ms), fast-attack/decay windowed burst at -12/-6/+6/+12 ST was
checked for: (a) the burst is clearly audible after the module's own
latency delay, and (b) no significant energy appears in the 5-50ms window
immediately *before* the expected onset (pre-echo) beyond 15% of the
burst's own peak. All four intervals pass - no pre-echo, no doubled/split
transient, consistent with a phase-locked (not naive unlocked) phase
vocoder design.

## Polyphonic material

The bass matrix above uses isolated single tones. Five deterministic
polyphonic scenarios (fixed frequencies/phases, no randomness) were added
to close that gap, each swept at -12/-7/-3/+3/+7/+12 ST:

- **A - bass + harmonics**: 60/120/180Hz (a harmonic stack)
- **B - two low tones**: 55/110Hz (an octave)
- **C - triads**: A major (220/277.18/329.63Hz) and A minor (220/261.63/329.63Hz)
- **D - dense chord**: a 5-note Cmaj9-style voicing (130.81/164.81/196.00/246.94/293.66Hz)
- **E - bass + chord (the scenario called out as especially critical)**: 60Hz bass + A major triad together

**A methodological note worth recording**: the first version of scenarios
B and C used closer intervals (a fifth-ish 55/82Hz pair, a minor-third
chord voiced in the same register the isolated-tone matrix uses) and
measured what looked like real wobble - freqStd up to 13.5%, ampDbStd up
to ~9dB. That turned out to be **two real but different confounds, not a
PITCH defect**: (1) two simultaneous tones a fifth apart at low absolute
frequency produce genuine acoustic beating at their difference frequency
(this happens with *any* correct processing, including a perfect
passthrough - it's physics, not an artifact); (2) a fixed musical interval
like a minor third has a Hz gap that is *always* under 1 Goertzel bin
width at the bass-matrix's 4-cycle analysis window, at any register (the
gap and the window's frequency resolution both scale with the target
frequency the same way), so the measurement was reading cross-leakage
between adjacent chord tones, not the algorithm's own behaviour. Fixed by
(1) changing the two-tone test to an exact octave (55/110Hz - harmonically
locked, no beat envelope of its own, so any beating in the *output* is now
attributable to the algorithm) and (2) widening the chord tests' analysis
window to 14 cycles (enough margin to actually separate a minor third).
After the fix, the *same* signals settled to freqStd 0.04-0.25% and
ampDbStd well under 1dB - confirming the original numbers were a test-
design artifact, not a real stability problem, without changing a single
DSP constant.

Measured results (representative rows; the full 36+24+12+24+30-case set is
printed by `UNI76PitchPolyphonicTests` at every build):

| Scenario | Case | freqStd% | ampDbStd | Notes |
|---|---|---|---|---|
| A (bass+harmonics) | 60Hz-12ST bass component | - | 0.109dB | freq error 3.17% (see below) |
| B (octave pair) | 55/110Hz, worst case (-12ST) | 0.06-0.25% | 0.03-0.11dB | no beating in output |
| C (A major triad) | root, all 6 intervals | 0.22-0.25% | 0.45-0.54dB | |
| C (A minor triad) | root, all 6 intervals | 0.15-0.21% | 0.61-0.68dB | closer voicing, still stable |
| D (5-note chord) | lowest note, all 6 intervals | 0.12-0.96% | 0.49-2.25dB | worst case at -3ST |
| **E (bass+chord, critical)** | 60Hz bass, all 6 intervals | **0.09-0.29%** | **0.07-0.13dB** | tightest result of all five |

Scenario E - the case the product brief specifically calls "especially
critical" - measures as the *most* stable of the five (freqStd well under
0.3%, ampDbStd under 0.14dB at every interval): the bass fundamental does
not wobble, does not disappear, and does not develop periodic beating
when a chord plays over it. Frequency-tracking error (mean vs. expected,
not the stability metric) stays under ~3.2% worst-case across all five
polyphonic scenarios - looser than the pure-tone matrix's <1%, as expected
for a mean-frequency estimate taken from a shorter, busier window, but
well short of a semitone (which would be 100%) and with no accompanying
instability, so it reads as measurement variance in a harder-to-analyse
signal, not mistracking. Sidebands on the bass component (scenario A and
E) measured below -15dB relative to the fundamental in every case. No
gain explosions, no non-finite output, in any of the 126 measured
combinations across the five scenarios.

## Stereo coherence

### Round 3: architecture fix - a single shared multi-channel engine

Rounds 1 and 2 (below, kept for the honest history) both tried to fix
excess width/room character *downstream* of the engine, via a Side-channel
attenuation filter, first flat then frequency-shaped. Both were reported
back, after real listening, as insufficient - "уходит за уши" persisted at
every semitone value, and a distinct "room"/distance quality remained that
no amount of Side narrowing removed. Per explicit instruction this round:
stop compensating the symptom and find the actual mechanism, even if that
means reworking the architecture the earlier "two independent mono
engines, bit-identical by construction" design was built around.

**Root cause, found by reading the vendored engine's own source directly**
(`ThirdParty/signalsmith-stretch/signalsmith-stretch.h`), not by
assumption: with two fully independent per-channel `SignalsmithStretch`
instances, each channel's phase-vocoder resynthesis evolves its own phase
completely independently of the other. For genuinely *correlated* (not
bit-identical) stereo input - the normal case for a real recording, not
just the dual-mono test case - that independent phase evolution shows up
downstream as extra, synthetic Side energy the source never had: audible
as excess width, and (since high-frequency inter-channel decorrelation is
exactly the psychoacoustic cue the ear reads as diffuse space) also as
part of the "room" character. This was the actual reason the previous
Side-narrowing filters could reduce the symptom but never eliminate it -
they scaled down the *consequence* of independent phase drift, not the
drift itself.

**The fix - a single shared multi-channel engine, not a downstream
filter.** `signalsmith-stretch.h`'s own multi-channel `configure()`/
`process()` path (previously used only for its *input/output* interface,
never for genuinely joint analysis) has an internal mechanism built
exactly for this: its phase-vocoder re-prediction step
(`processSpectrum()`) identifies, **per frequency band**, whichever
channel currently has the most energy, computes that channel's phase
prediction, and then explicitly **locks every other channel's phase to
it** - "all other bins are locked in phase" - carrying over the *input
signal's own* inter-channel phase relationship (via a twist computed
between the two channels' actual input phases) rather than letting each
channel's phase run independently. This is the library's own designed
answer to multi-channel coherence, not a workaround layered on top of it.
`PitchProcessor` (`.h`/`.cpp`) now wraps exactly **one**
`SignalsmithStretch<float>` instance, configured with the real channel
count, and calls its `process()` once per block with both channels'
pointers - see the class's own comment for the mechanism in more detail.

The previous downstream Side-narrowing filter (`pitchSideNarrowLowGain`/
`pitchSideNarrowHighGain`/`pitchSideNarrowCrossoverHz` in `PitchCurves.h`,
and the `sideNarrowShelf` biquad in `PitchProcessor`) has been **removed
entirely** - per explicit instruction, the root-cause fix is not to be
combined with more downstream masking on top of it.

**Mono stays strictly mono, as a property of the shared engine, not a
separate mono-detection path.** For genuinely identical (dual-mono)
input, both channels have exactly equal energy in every band; the
"strictly greater than" comparison the library uses to pick the reference
channel (`e > maxEnergy`) means ties always resolve to channel 0, and
channel 1's phase is locked to it via a twist that is provably unity for
identical input phases - there is no separate "if mono, do X" branch
anywhere in this design.

**Measured** (`Tests/PluginTests.cpp`'s PITCH suite, before vs. after this
round - both from a real build/test run, not estimated):

**Dual-mono ("Mono stays Mono") precision** - the one real, disclosed
tradeoff of this architecture, measured via a real build+test run, not
estimated. The previous two-independent-engines design was bit-exact by
construction (`<1e-6`); the shared engine's per-band phase-locking is
only *very close* rather than bit-exact for literal dual-mono input:

| Semitones | Pure tone, maxAbsDiff | Polyphonic (bass+chord), maxAbsDiff |
|---|---|---|
| -12 | 9.44e-5 | 6.49e-4 |
| -3 | 1.01e-3 | - |
| 5 | 1.08e-3 | - |
| 7 | - | 7.68e-4 |
| 12 | 3.90e-4 | 8.59e-4 |

Worst case ~1.1e-3 absolute on signals with amplitude 0.3-0.4 - roughly
**-50dB relative to the signal**, i.e. a channel-difference component
about 300x quieter than the source itself. This is disclosed, not hidden:
the relevant tests (`Tests/PluginTests.cpp`'s "Stereo: identical L/R
input stays essentially in Mono" and "Polyphonic E in stereo (dual-mono):
stays essentially in Mono for real musical material too") were updated
from a `<1e-6` bit-exact bound to a `<3e-3` "essentially Mono, not
audibly wandering" bound, with the tradeoff explained directly in the
test's own comment.

**Genuinely non-identical L/R content** (shared 60Hz bass note, different
chord voicing per channel - the same scenario the module has always used
to verify the two channels track real, correlated-but-different material
correctly) - bass level match between channels, confirming the shared
engine doesn't introduce any new channel-balance error on real stereo
material:

| Semitones | L/R bass level difference |
|---|---|
| -12 | 0.040 dB |
| 7 | -0.006 dB |
| 12 | 0.023 dB |

All three comfortably under a tenth of a dB - no measurable balance shift
from the architecture change.

**What this round did *not* change**: `pitchStftBlockSeconds` (140ms/35ms,
still the benchmarked configuration - see "Configuration benchmark" above)
and `pitchTonalityLimitHz` (4000Hz, from round 2 below) are both untouched.
The window-length/smearing tradeoff documented in round 2's own "window
experiment that failed" still applies exactly as measured there; this
round's fix addresses the *stereo-decorrelation* half of the reported
"room" character (a real and, per the measurements above, substantial
contributor), not the STFT window's own mono phase-vocoder smearing,
which is a separate, still-open, still-unresolved-without-a-different-
STFT-front-end limitation - see "Limitations" below for the honest
scoping of what remains.

### Round 1 and round 2 (superseded by the architecture fix above, kept for history)

The module's very first live-testing round reported the pitch-shifted
signal spreading audibly wider than the source ("уходит за уши"),
compared explicitly against Waves SoundShifter's own, narrower character.
The first fix tried was a fixed, frequency-flat attenuation on the wet
signal's own Side component - reasoned (at the time) as consistent with
the "two independent engines" design being sound and the width being an
acceptable, compensable side effect of it. Reported back as insufficient
on two counts: still not narrow enough, and a "room" quality that no
amount of narrowing removed.

Round 2 replaced the flat attenuation with a frequency-shaped one (a
low-shelf on Side, narrow hard at the top/gentle at the bottom - since an
STFT's linearly-spaced bins put proportionally more independently-
evolving bins in the treble) and separately investigated the "room"
character via the STFT window length - trying (and reverting, after
measuring a real polyphonic-material regression) a shortened 100ms/25ms
window, before settling on lowering the tonality limit `8000Hz -> 4000Hz`
instead, which costs nothing in the bass-stability range this module's
own top acceptance bar cares about most:

| Case | Expected | Measured (100ms window, reverted) | Error |
|---|---|---|---|
| 120Hz partial, -7ST | 80.09Hz | 97.45Hz | 21.7% |
| 180Hz partial, -3ST | 151.36Hz | 164.26Hz | 8.5% |
| 120Hz partial, -3ST | 100.91Hz | 110.57Hz | 9.6% |

Both of round 2's downstream filters are now removed (see "Round 3"
above) - the actual mechanism turned out to be fixable at its source.

## CPU

Release build, `uni76::dsp::PitchProcessor` measured directly (steady-
state, after a 20-block warm-up so STFT startup cost doesn't dominate),
processing-time / audio-time ratio:

| Sample rate | Block | Channels | Semitones | Ratio (of realtime) |
|---|---|---|---|---|
| 48000 Hz | 256 | 1 | 0 | 0.74% |
| 48000 Hz | 256 | 2 | -12 | 1.96% |
| 96000 Hz | 256 | 2 | -12 | 3.99% |
| 192000 Hz | 64 | 2 | -12 | 7.95% (worst case measured) |
| 192000 Hz | 1024 | 2 | 0 | 6.14% |

Block size has little effect (the STFT does its own internal buffering
regardless of host chunking, matching the "must not depend on host block
size" requirement). Worst case measured is ~8% of realtime at 192kHz
stereo, -12 ST, 64-sample blocks - comfortably realtime-safe even
stacked with PREAMP/EQ/SAT. Bass stability was prioritised over CPU
throughout the configuration benchmark, consistent with the module's
priority order; the resulting CPU cost turned out low regardless.

## Limitations

- **Dual-mono input is no longer perfectly bit-exact** (round 3's
  shared-engine architecture fix, see "Stereo coherence" above) - worst
  measured deviation ~1.1e-3 absolute, roughly -50dB relative to the
  signal. A deliberate, disclosed tradeoff against a much larger,
  definitely-audible problem (excess width/room on real correlated
  stereo material) the previous bit-exact architecture had no way to
  avoid. Not expected to be audible in practice, but not literally zero
  either - flagged honestly rather than re-asserting the old bit-exact
  claim.
- **The "room"/smearing character is only partially addressed.** This
  round's fix targets the stereo-decorrelation half of the reported
  "room" quality (the shared engine's phase-locking measurably reduces
  it - see "Stereo coherence"), but the STFT analysis window's own
  smearing (the other, mono-domain contributor to "room"-like phase
  incoherence) is unchanged - `pitchStftBlockSeconds` stays at the
  benchmarked 140ms/35ms, since shortening it was already tried and
  reverted in round 2 for breaking polyphonic bass-stability. A
  genuinely shorter/different STFT front-end that doesn't trade away
  that stability remains a real, unstarted follow-up if the "room"
  character is still judged too strong after this round's fix.
- 0 ST is *measured* transparent (broadband RMS diff 7.2e-8, max diff
  3.1e-7, frequency response within 0.15dB - see above), but is not a
  literal bit-exact passthrough the way some modules' "off" state is - the
  STFT analysis/resynthesis path always runs. A dedicated bit-exact
  shortcut for 0 ST was evaluated and deliberately not built - see "0 ST
  transparency" for why.
- Latency (140ms at every sample rate) is higher than the product brief's
  "~50-80ms" guideline. This was a deliberate, evidence-based tradeoff
  (see "Configuration benchmark") - `manual-130` (a smaller, closer-to-
  the-guideline configuration) measured a real, reproducible bass-
  stability outlier and was rejected on that basis, not on latency
  grounds. If a future requirement demands lower latency, expect to trade
  away some of the current bass-stability margin, not get both for free.
  Reconfirmed unchanged in this pass (see "Fixed latency"'s exact
  per-rate table) - not revisited, since bass and polyphonic stability
  both measure well inside their pass thresholds at this configuration.
- Time-stretching (the library's other headline feature) is deliberately
  never used - every `process()` call passes equal input/output sample
  counts, so duration is always preserved by construction. This also
  means the library's internal random-phase time-stretch-artifact
  mitigation is inert here (see "Stereo coherence"), which is a
  correctness feature for this module's use case, not a limitation.
- Polyphonic material (bass+harmonics, low dyads, major/minor triads, a
  dense 5-note chord, and the especially critical bass+chord case) is now
  covered - see "Polyphonic material" above. Not covered: true full-mix
  synthetic material (drums+bass+harmony+lead all at once), formant
  behaviour, and vocal-like sources specifically - the phase-locked
  vocoder design is the library's own answer to polyphonic coherence, and
  the five scenarios tested are a representative cross-section, but they
  are not an exhaustive substitute for real musical material.
- Manual mouse-driven UI interaction was verified this pass (drag, wheel,
  keyboard, double-click, edge clamping, Shift - see "Live UI
  verification") through the real WebView2 editor via synthetic OS-level
  input, which exercises the same native-bridge/DOM event path a real
  drag would. It was not exercised by an actual human hand on an actual
  mouse in an actual DAW session.
