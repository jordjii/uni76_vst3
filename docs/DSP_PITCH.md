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
Input (per channel)
  -> IntegerDelayLine (dry path, delayed by the module's own added latency)
  -> SignalsmithStretch<float> (mono, independent per channel)
       - setTransposeSemitones(clamped -12..+12) every block (no allocation)
       - process(in, N, out, N) - equal in/out sample counts every call,
         so the engine only ever pitch-shifts, never time-stretches
  -> enable/disable crossfade against the delayed dry copy
  -> Output
```

Two fully independent **mono** `SignalsmithStretch<float>` instances (one
per channel) are used, not one shared multi-channel instance - see
"Stereo coherence" below for the measured reason. The vendored engine is
kept behind a PIMPL (`PitchProcessor::Engine`, defined only in the `.cpp`)
so `PitchProcessor.h` itself carries no `ThirdParty/` include dependency.

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

## Fixed latency

`getLatencySamples()` returns `stretch.inputLatency() +
stretch.outputLatency()` (both channels share one configuration, so this
is a single scalar), set once in `prepare()` - **it never depends on the
semitone value or the enabled state**, verified by a dedicated test that
sweeps `-12/-7/0/7/12` ST and enabled/disabled and asserts the reported
value never moves. The disabled/bypass path is delayed by exactly this
many samples (`IntegerDelayLine`, the same pattern PREAMP/SAT already
use), so `enabled=false` is a bit-exact delayed passthrough, not a
different-latency shortcut.

Measured (140ms/35ms configuration, `presetDefault`-free manual config):

| Sample rate | Latency (samples) | Latency (ms) |
|---|---|---|
| 44100 Hz | 6174 (DSP-level) / 6186 (full plugin, incl. PREAMP+SAT) | 140.0 ms (140.27 ms full plugin) |
| 48000 Hz | 6720 | 140.0 ms |
| 96000 Hz | 13440 | 140.0 ms |
| 192000 Hz | 26880 | 140.0 ms |

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

## Transient quality

A short (20ms), fast-attack/decay windowed burst at -12/-6/+6/+12 ST was
checked for: (a) the burst is clearly audible after the module's own
latency delay, and (b) no significant energy appears in the 5-50ms window
immediately *before* the expected onset (pre-echo) beyond 15% of the
burst's own peak. All four intervals pass - no pre-echo, no doubled/split
transient, consistent with a phase-locked (not naive unlocked) phase
vocoder design.

## Stereo coherence

**Two fully independent mono engines**, not one shared 2-channel instance
- this was a deliberate architecture decision, not the library's default
usage pattern, made after measuring the alternative: a single
`SignalsmithStretch` instance configured for 2 channels, fed bit-identical
sine data on both channels, produced **not** bit-identical output
(`maxAbsDiff=0.00021`, ~-63dB relative divergence on a 0.3-amplitude
signal). Reading the library's own source confirmed this isn't its
`std::random_device`-seeded phase-randomisation feature (that branch is
gated on `timeFactor > 2` - i.e. only engages during heavy *time-
stretching*, which this module never does, since every `process()` call
uses equal input/output sample counts); it's the multi-channel STFT's own
per-channel-indexed band/phase-tracking state, which is channel-aware by
design even for identical content. Two separate mono instances have no
cross-channel state at all, so bit-identical input is guaranteed by
construction (not luck) to produce bit-identical output - verified by a
dedicated test across `-12/-3/0/5/12` ST (`maxAbsDiff < 1e-6`, effectively
float rounding noise, not divergence). Decorrelated hard-panned stereo
material was also checked and stays finite/bounded.

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

- 0 ST is *measured* transparent (< 0.2dB gain deviation, see above), but
  is not a literal bit-exact passthrough the way some modules' "off"
  state is - the STFT analysis/resynthesis path always runs.
- Latency (140ms at every sample rate) is higher than the product brief's
  "~50-80ms" guideline. This was a deliberate, evidence-based tradeoff
  (see "Configuration benchmark") - `manual-130` (a smaller, closer-to-
  the-guideline configuration) measured a real, reproducible bass-
  stability outlier and was rejected on that basis, not on latency
  grounds. If a future requirement demands lower latency, expect to trade
  away some of the current bass-stability margin, not get both for free.
- Time-stretching (the library's other headline feature) is deliberately
  never used - every `process()` call passes equal input/output sample
  counts, so duration is always preserved by construction. This also
  means the library's internal random-phase time-stretch-artifact
  mitigation is inert here (see "Stereo coherence"), which is a
  correctness feature for this module's use case, not a limitation.
- Polyphonic/chord material, full-mix synthetic material, and formant
  behaviour were not separately measured beyond the sine/bass/transient
  suite above and the WAV artifacts in `docs/audio/`- the phase-locked
  vocoder design is the library's own answer to polyphonic coherence, but
  no dedicated multi-tone wobble measurement was run the way the
  monophonic bass matrix was.
