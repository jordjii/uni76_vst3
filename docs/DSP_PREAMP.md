# PREAMP / TRANSFORMER - DSP design and measurements

This is the first real DSP in UNI 76. Everything here lives in
[`Source/DSP/PreampProcessor.h`](../Source/DSP/PreampProcessor.h) /
[`.cpp`](../Source/DSP/PreampProcessor.cpp), driven by the curves in
[`Source/DSP/PreampCurves.h`](../Source/DSP/PreampCurves.h) and the small
allocation-free filter toolkit in
[`Source/DSP/Biquad.h`](../Source/DSP/Biquad.h). EQ, Saturation, Pitch,
Panorama, Reverb and Imager remain a strict passthrough - see
[`CLAUDE.md`](../CLAUDE.md).

This is **not** an emulation of any specific existing hardware (not a Neve,
API, or Telefunken clone) - it's Nostalgia Audio's own character: warm,
dense, musical, gently rounding transients, adding harmonics, without
digital-sounding clipping or obvious fuzz.

**Revision note (sound calibration pass):** the architecture below is
unchanged from the first PREAMP pass; this revision documents a full
sound-calibration pass that found and fixed a real problem in the
nonlinear model - see "What changed in the nonlinear model" below - plus
retuned the output-compensation curve and added the measurements this
document now reports (level x DRIVE matrix, null test, aliasing).

## Signal chain

```
Input
  -> DC / infrasonic protection      (fixed ~5 Hz one-pole HPF, base rate)
  -> [oversampled region: 4x @ 44.1/48kHz, 2x @ 88.2/96kHz, 1x @ 176.4kHz+]
       -> transformer coloration     (one-pole "core" rounding filter +
                                       fixed-frequency low-shelf density
                                       boost, both drive-dependent)
       -> nonlinear analog stage     (asymmetric-gain tanh waveshaper)
  -> soft Low Cut                    (drive-dependent, ~20-70 Hz, base rate)
  -> soft High Cut                   (drive-dependent, ~20k-11k Hz, base rate)
  -> output compensation             (drive-dependent trim)
  -> enable/disable crossfade        (latency-aligned dry copy)
  -> Output
```

Every stage is implemented once per channel with independent state - no
cross-channel coupling and no randomness anywhere, so stereo image is
preserved exactly (see the "signal in the left channel only never bleeds
into a silent right channel" test, and the calibration pass's own
"identical L/R input stays numerically identical after PREAMP" check, in
`Tests/PluginTests.cpp`).

## What changed in the nonlinear model (sound calibration pass)

The first PREAMP pass used an **additive quadratic asymmetry term**:
`xa = xd + asym(t) * xd * xd`. This measured fine at the one level it was
tuned against (-18dBFS), but a full level x DRIVE sweep done during
calibration found it was badly broken at hot input levels: **-6dBFS at
DRIVE=100% measured >400% THD** - not "strong saturation", but a
rectifying, fuzz-like mess. The cause: `xd * xd` is always positive
regardless of the sign of `xd`, so at high drive * hot input it pushed the
*negative* half of the wave toward the same large *positive* argument as
the positive half, instead of gently shaping it differently - effectively
half-wave rectifying the signal rather than colouring it.

**Fix:** replaced it with a bounded **per-half gain asymmetry** - a milder
`tanh()` on the negative excursion only:

```cpp
const auto shaped = xd >= 0.0f ? std::tanh (xd)
                                : std::tanh (xd * (1.0f - asym));
```

Each half is independently `tanh()`-bounded, so total output can never
exceed unity regardless of drive or asymmetry amount - the runaway mode is
structurally impossible now, not just unlikely. This is also a more
standard/textbook way to model an asymmetric analogue transfer curve (a
real transformer/tube's positive and negative swings do genuinely see
different effective gain) than an unbounded additive term.

Alongside this fix, `preampDriveGainMax` was reduced from `20.0` (~+26dB)
to `10.0` (~+16dB) - even with the bounded asymmetry fix, the old ceiling
gain was driving DRIVE=100% at -18dBFS to ~19.5% THD, more than the
product brief's "single digits to low tens, only if musical" ceiling. The
output-compensation curve was also re-derived from measurement rather than
reusing the drive curve's own exponent - see "Output compensation" below.

## Nonlinear stage

Not a bare `tanh(input * drive)`. Per sample, inside the oversampled
region:

```
x  = roundingFilter(x)                      // one-pole "core" lowpass
x  = colorShelf(x)                          // low-shelf density boost
xd = x * driveGain(t)                       // t = smoothed DRIVE, 0..1
shaped = xd >= 0 ? tanh(xd) : tanh(xd * (1 - asymmetry(t)))
y  = shaped / tanh(driveGain(t))            // normalised soft ceiling
```

- `driveGain(t)` (`preampDriveGainLinear`) is a **geometric** (log-linear)
  interpolation from `0.05` (~-26dB) at `t=0` to `10.0` (~+20dB) at `t=1`,
  shaped by `t^1.3` so most of the audible "action" happens in the upper
  half of the range, matching the qualitative 0-20/20-50/50-75/75-100%
  zones from the product brief. Dividing the waveshaper's output by
  `tanh(driveGain(t))` (not by `driveGain(t)` itself) is what keeps `t=0`
  structurally close to identity: for the tiny `driveGain(0)=0.05`,
  `tanh(0.05*x)/tanh(0.05) ~ x` to within ~0.1% error across the full
  amplitude range, and gives `t=1` a firm, soft (never flat/hard-clipped)
  ceiling since `tanh(driveGain)` has already saturated to ~1 there.
- `asymmetry(t)` (`preampAsymmetryAmount`, `0` at `t=0` up to `0.20` at
  `t=1`) is a fractional gain reduction applied to the waveshaper's
  negative half only - see "What changed" above for why this replaced an
  earlier unbounded quadratic term. It produces the even-harmonic (H2)
  content a purely odd-symmetric `tanh()` alone can't, without ever being
  able to blow up regardless of drive or input level.
- The one-pole "core" rounding filter and the fixed-150Hz low-shelf ahead
  of the waveshaper are the "transformer coloration": lower rounding-filter
  cutoff at higher drive both softens/rounds transients and reduces
  alias-prone high-frequency energy reaching the nonlinearity. The
  rounding filter's resting (DRIVE=0%) cutoff is `40000Hz`, not `20000Hz`
  as originally set - see "Null test" below for why.

## Oversampling / anti-aliasing

`juce::dsp::Oversampling<float>`, `filterHalfBandPolyphaseIIR`,
`isMaximumQuality=true`, `useIntegerLatency=true` - chosen over FIR
half-band filters specifically to keep latency low (IIR polyphase halfband
filters need far fewer taps than an equiripple FIR for the same alias
rejection), which matters for a real-time character effect where the user
is expected to hear it while playing, not just for offline rendering.
`isMaximumQuality=true` was kept on despite the latency cost because this
module deliberately generates strong harmonics up near Nyquist at high
DRIVE - weak alias rejection would be audible as digital-sounding
intermodulation, which is exactly the "cheap digital clipping" character
the product brief explicitly rules out.

Oversampling factor is chosen once in `prepare()` from the host sample
rate and never changes at runtime (`chooseOversamplingStages`) - **there is
no runtime on/off switch for oversampling in production code**; the
"reference, no oversampling" measurement below exists only inside the test
file, as a separate offline comparison path.

| Sample rate      | Oversampling | Rationale |
|---|---|---|
| 44.1 / 48 kHz     | 4x (2 stages) | Full anti-alias headroom where it's needed most. |
| 88.2 / 96 kHz     | 2x (1 stage)  | Reduced factor - Nyquist is already well above the harmonic content that matters. |
| 176.4 / 192 kHz+  | none (1x)     | Nyquist (88.2kHz+) is already far beyond audible harmonic content - additional oversampling would just burn CPU. |

### Latency

Oversampling factor (and therefore latency) depends **only** on sample
rate, never on DRIVE or `preampEnabled` - the enable/disable bypass is an
internal crossfade against a *latency-aligned* dry copy (see
`uni76::dsp::IntegerDelayLine` in `Source/DSP/Biquad.h`), not a change to
the actual signal path length, so the plugin's declared latency stays
constant for correct host plugin-delay-compensation at every DRIVE/enabled
combination (`Tests/PluginTests.cpp`: "Latency is constant for a given
sample rate regardless of DRIVE or enabled state"). Measured via
`AudioProcessor::setLatencySamples()` in `prepareToPlay`:

| Sample rate | Latency (samples) | Latency (ms) |
|---|---|---|
| 44100 Hz  | 6 | 0.136 |
| 48000 Hz  | 6 | 0.125 |
| 96000 Hz  | 4 | 0.042 |
| 192000 Hz | 0 | 0.0 |

(From an actual `UNI76Tests.exe` run - `latency @ ... Hz = ...` lines,
sourced directly from `juce::dsp::Oversampling::getLatencyInSamples()`, not
a guess. Sub-millisecond at every rate - the polyphase IIR choice over an
equiripple FIR half-band design keeps this low. Unaffected by this
calibration pass.)

## Aliasing measurement

`Tests/PluginTests.cpp`'s `"Aliasing: production oversampled path
suppresses fold-back energy vs a non-oversampled reference"` test compares
the real production path (with oversampling) against a **test-only**
reference implementation of the identical coloration+waveshaper math
applied directly at the base rate with no up/downsampling - probing a
frequency band below each test tone (where a stable nonlinearity applied
to a pure tone can only produce energy via aliasing, never a real
harmonic) at DRIVE=100% (the worst case, most harmonic energy generated),
44.1kHz:

| Test tone | Probe frequency | Reference (no oversampling) | Production (oversampled) | Suppression |
|---|---|---|---|---|
| 4 kHz  | 2 kHz | -62.5 dB | -67.7 dB | -5.2 dB |
| 8 kHz  | 4 kHz | -39.4 dB | -68.7 dB | -29.3 dB |
| 12 kHz | 6 kHz | -49.2 dB | -68.4 dB | -19.2 dB |

Production measured **equal-or-lower** fold-back energy than the reference
at every probed frequency (the test asserts this directly, not just prints
it). The 4kHz/2kHz case shows the smallest gap because there simply isn't
much real fold-back energy landing at exactly that probe frequency for a
4kHz fundamental at this sample rate/oversampling combination in the first
place - both measurements are close to the -60 to -70dB noise floor there,
not because oversampling is failing to help. The 8kHz and 12kHz cases,
where real fold-back is actually significant without oversampling, show
oversampling suppressing it by 19-29dB.

## Output compensation (retuned during calibration)

The nonlinear stage's own `tanh()` normalisation gives quiet signals more
relative boost as DRIVE increases (see "Level x DRIVE dependency" below) -
by itself, **raw/untrimmed** RMS growth at -18dBFS measured close to flat
through DRIVE=50% (0, -0.2, +0.1 dB) and then rose sharply toward
DRIVE=100% (+5.2dB at 75%, +16.8dB at 100%).

The first calibration attempt reused the drive curve's own `t^1.3`
shaping exponent for the trim - this mismatched the *actual* (much more
back-loaded) raw growth shape above, over-trimming the already-flat 25-50%
region (a measured -6dB dip there) while barely touching the real growth
at the top. The trim now uses its **own**, steeper exponent fitted to the
measured raw curve instead:

```cpp
inline constexpr float preampOutputTrimMaxDb = -15.0f;
inline constexpr float preampOutputTrimExponent = 4.0f;
// trim(t) = preampOutputTrimMaxDb * t^preampOutputTrimExponent
```

Net RMS change at -18dBFS (the product brief's primary reference level),
measured end-to-end through the real `PreampProcessor`:

| DRIVE | Net RMS change vs DRIVE=0% |
|---|---|
| 25%  | -0.25 dB |
| 50%  | -0.89 dB |
| 75%  | +0.46 dB |
| 100% | +1.82 dB |

Comfortably inside the product brief's +/-2 to 3dB target, and a single
smooth curve (each step differs from its neighbour by well under 1dB)
rather than the earlier version's sharp -6dB dip-then-recover artifact -
note this sequence is *not* monotonic (it dips slightly through 50% before
rising), just smooth; "monotonic" would be the wrong word for it. This is
a deliberate creative
trim, not a full loudness normaliser (there is no envelope-follower/AGC
anywhere in the chain) - hotter input signals still reach the saturating
region, and get proportionally more gain reduction, earlier than quiet
ones purely because that's what a static waveshaper does to a bigger
number, which is the point.

## Level x DRIVE dependency (1kHz, full matrix)

Measured end-to-end through the real `PreampProcessor`
(`Tests/PluginTests.cpp`'s "Input level x DRIVE matrix" test - numbers
below are copied directly from that test's stdout):

| Level (dBFS) | DRIVE | THD % | H2 (dB) | H3 (dB) | H5 (dB) | Net RMS vs 0% |
|---|---|---|---|---|---|---|
| -30 | 25%  | 1.04  | 1.9   | -62.8 | -81.6 | -0.25 dB |
| -30 | 50%  | 2.14  | 7.6   | -53.1 | -76.2 | -0.89 dB |
| -30 | 75%  | 3.29  | 12.7  | -28.9 | -71.8 | +0.56 dB |
| -30 | 100% | 4.27  | 18.1  | 2.4   | -39.0 | +3.87 dB |
| -18 | 25%  | 1.04  | 13.9  | -39.6 | -69.4 | -0.25 dB |
| -18 | 50%  | 2.13  | 19.5  | -19.8 | -63.6 | -0.89 dB |
| -18 | 75%  | 3.19  | 24.3  | 6.8   | -39.1 | +0.46 dB |
| -18 | 100% | 8.31  | 17.1  | 34.0  | 14.5  | +1.82 dB |
| -12 | 25%  | 1.04  | 19.9  | -22.8 | -63.5 | -0.25 dB |
| -12 | 50%  | 2.12  | 25.5  | -2.0  | -56.5 | -0.91 dB |
| -12 | 75%  | 3.21  | 28.7  | 24.1  | -9.9  | +0.15 dB |
| -12 | 100% | 19.31 | 19.6  | 43.6  | 32.1  | -1.65 dB |
| -6  | 25%  | 1.04  | 25.9  | -5.1  | -57.5 | -0.25 dB |
| -6  | 50%  | 2.09  | 31.1  | 15.8  | -31.5 | -0.97 dB |
| -6  | 75%  | 5.76  | 28.6  | 39.9  | 16.8  | -0.91 dB |
| -6  | 100% | 30.62 | 21.7  | 47.9  | 40.9  | -6.69 dB |

(H2/H3/H5 are relative Goertzel-magnitude dB, not calibrated dBFS - only
progression within a row and down a column is meaningful. 0% DRIVE is
omitted from the table since it measures the same near-zero THD, ~0.01%,
at every level - see the null test below for the 0%-specific numbers.)

Reading this the way the product brief asks:
- **25%/50%/75% THD is essentially level-independent** (1.04/2.1/3.2-5.8%
  regardless of input level) - this is the small-signal region of the
  waveshaper, where it's still close to proportional.
- **100% THD is strongly level-dependent** (4.3% at -30dBFS up to 30.6% at
  -6dBFS) - a hot signal genuinely saturates harder than a quiet one at
  maximum drive, exactly the analogue-style behaviour the brief asks for.
  A quiet signal at DRIVE=100% never turns "everything to mud" - it stays
  at single-digit THD.
- **-18dBFS/100% (8.3% THD) is the primary reference point** and sits
  comfortably in the brief's "single digits to low tens, if musical"
  target; -12dBFS/100% (19.3%) and -6dBFS/100% (30.6%) are hotter-than-
  reference inputs pushed at maximum drive, which is expected to be the
  most extreme corner of the module's range.
- Net RMS change stays within about +/-2dB through DRIVE=75% at every
  level tested; only DRIVE=100% at the hottest levels shows larger
  deviation (-6.7dB at -6dBFS), which is the natural leveling/compression
  effect of driving an already-hot signal hard into the waveshaper's
  ceiling, not a gain-staging bug.

## Low Cut / High Cut (internal, not a parameter)

`preampLowCutHz` / `preampHighCutHz` in `PreampCurves.h`, applied as
2nd-order Butterworth (Q=1/sqrt(2), maximally flat, no resonant peak,
~12dB/octave) filters at the base sample rate, right after the nonlinear
stage. Unchanged by this calibration pass - measured and reviewed
specifically for the "does 100% sound too closed/telephone-like" concern:

| DRIVE | Low Cut | High Cut |
|---|---|---|
| 0%   | 20 Hz    | 20000 Hz |
| 25%  | 27.2 Hz  | 18172 Hz |
| 50%  | 38.9 Hz  | 15944 Hz |
| 75%  | 53.4 Hz  | 13535 Hz |
| 100% | 70 Hz    | 11000 Hz |

Verdict: kept as-is. A real telephone band is roughly 300Hz-3400Hz; UNI
76's widest closure at DRIVE=100% (70Hz-11kHz) retains the entire
presence/clarity/sibilance region and over 2.5 octaves more top end than a
telephone band - it reads as a gentle vintage narrowing, not a bandwidth-
limited effect. The same 30Hz/15kHz tones measurably attenuate more at
DRIVE=100% than DRIVE=0% in the real processed audio (not just the
formula) - see `Tests/PluginTests.cpp`'s "Low Cut/High Cut measurably
attenuates..." tests, which run the actual `PreampProcessor` and measure
via a Goertzel single-bin analysis.

The `Resources/Web/aux_visuals.js` `bindPreampFilterLines()` LOW CUT/HIGH
CUT indicator lines are a hand-mirrored literal of these two formulas
(there is no shared runtime between the plugin core and the WebView UI) -
if the curve above ever changes, update both places together.

## Smoothing / bypass

- DRIVE: `juce::SmoothedValue` linear ramp, **25ms**, advanced
  sample-accurately at the oversampled rate for the gain-critical parts of
  the nonlinear stage (where zipper noise would actually be audible).
  Filter coefficients (Low Cut/High Cut/colour/rounding) are recomputed
  once per block from the *previous* block's settled value - a one-block
  lag that's inaudible and standard practice, avoiding per-sample
  trig cost. Verified during calibration with a 0->100->0 sweep during
  continuous audio (in both the offline test suite and the real VST3
  host): no click, zipper, explosive transient, or NaN/Inf - the 25ms
  figure was kept as-is, listening/measurement didn't show a need to
  change it.
- `preampEnabled` (bypass): **20ms** linear crossfade between the fully
  processed ("wet") signal and a latency-aligned dry copy - never an
  instant `if` switch. The dry copy is pushed through the same fixed
  `IntegerDelayLine` the wet path's oversampling latency implies, so
  toggling bypass doesn't shift the plugin's effective timing (i.e. no
  comb filtering from a wet/dry misalignment during the crossfade) -
  verified on sine, transient, and stereo material during calibration.
  Kept at 20ms.

## Null test (DRIVE=0% transparency)

`Tests/PluginTests.cpp`'s null test measures **magnitude (gain) deviation
from unity** at DRIVE=0%, via Goertzel, rather than a raw time-domain
sample subtraction. A time-domain version was tried first and discarded:
near the fixed 20Hz/20kHz filter boundaries, any real filter's own group
delay causes a fractional-sample phase shift that a naive
integer-latency-aligned subtraction reads as a large "residual" (measured
-9.6dB at 100Hz, -4.9dB at 10kHz) even though the actual coloration
(loudness at that frequency) barely changes - a measurement artifact, not
a real transparency problem. Magnitude deviation is what "practically
transparent, no unexpected coloration" actually asks about:

| Probe | Gain deviation |
|---|---|
| 100 Hz | -0.005 dB |
| 1 kHz | +0.007 dB |
| 10 kHz | -0.29 dB |
| Broadband (80/400/1200/4000/9000 Hz multi-tone) | -0.02 to -0.05 dB (9kHz: -0.007 dB) |

1kHz (comfortably mid-band) is within 0.01dB of exactly transparent. 10kHz
showed a real, if modest, -1dB deviation in an earlier version of this
measurement, traced to the "transformer coloration" rounding filter's
resting (DRIVE=0%) cutoff being fixed at exactly 20000Hz - a one-pole
filter's magnitude response is still measurably down almost an octave
below its nominal cutoff. Since that filter is meant to be a
*drive-dependent* character effect, negligible at DRIVE=0%, its resting
cutoff was raised to `40000Hz` (see "What changed" above), which brought
10kHz down to -0.29dB. The soft 20Hz/20kHz boundaries the product brief
explicitly allows some coloration near are otherwise untouched.

## DC offset

The waveshaper's asymmetric per-half gain can bias the signal slightly,
but the soft Low Cut sits immediately downstream (even at its minimum
~20Hz at DRIVE=0%) and removes it before the signal leaves the module -
measured mean output at DRIVE=100% stays under `0.01` for a 0.5-amplitude
sine (`Tests/PluginTests.cpp`: "DC offset stays safely small...").

## Numerical safety

Input samples are sanitised (non-finite -> 0) at the very start of
`process()`, before anything else touches them, and the same check runs
again on the final output as a narrow correctness safeguard against
pathological host input - not a blanket amplitude clamp papering over
algorithm behaviour. `juce::ScopedNoDenormals` (already present in
`PluginProcessor::processBlock`) covers denormal protection for the whole
chain. Each half of the waveshaper is independently `tanh()`-bounded, so
the nonlinear stage itself can never produce non-finite output from finite
input, regardless of drive or asymmetry amount (this is now a structural
guarantee, not just an empirical one - see "What changed" above).

## Stereo

Verified during calibration: an identical mono signal fed to both L and R
produces **numerically identical** L/R output after PREAMP (each channel
has fully independent filter/waveshaper state, but no randomness or
cross-channel coupling exists anywhere in the chain, so identical input
guarantees identical output) - no stereo drift.

## Harmonic analysis (1kHz @ -18dBFS, post-calibration)

`Tests/PluginTests.cpp`, `UNI76PreampHarmonicAnalysisTests` - numbers below
are copied directly from an actual `UNI76Tests.exe` run's stdout:

```
DRIVE 0%:   fund=54.06dB  H2=-29.86dB  H3=-39.39dB  H4=-23.75dB  H5=-48.55dB  THD=0.0145%
DRIVE 25%:  fund=53.81dB  H2=14.00dB   H3=-37.31dB  H4=0.99dB    H5=-48.62dB  THD=1.05%
DRIVE 50%:  fund=53.17dB  H2=19.58dB   H3=-19.82dB  H4=6.47dB    H5=-48.87dB  THD=2.14%
DRIVE 75%:  fund=54.52dB  H2=24.29dB   H3=6.77dB    H4=11.94dB   H5=-39.89dB  THD=3.20%
DRIVE 100%: fund=55.85dB  H2=17.10dB   H3=33.97dB   H4=18.53dB   H5=14.52dB   THD=8.29%
```

(dB values are relative FFT-bin magnitudes from an unnormalised windowed
transform, not calibrated dBFS - only the relative progression across
DRIVE settings and between H2/H3/H4/H5 within a row is meaningful.)

Reading the progression:
- **THD grows monotonically and stays musical**: 0.015% -> 1.05% -> 2.14%
  -> 3.20% -> 8.29% - comfortably inside the product brief's "single
  digits at -18dBFS/100%" target (was 22.7% before this calibration pass).
- **H2 dominates at 25-75% DRIVE** (14 to 24dB, clearly above H3/H4/H5 at
  those settings) - "very light" through "saturated" reads mostly as a
  gentle even-harmonic warmth, matching "H2/H3 as the primary useful
  harmonics."
- **H3 becomes dominant at 100%** (34.0dB, clearly the loudest component)
  - a stronger, more "driven" character at maximum, still led by a
    low-order harmonic, not a spray of high-order content: H5 (14.5dB) is
    ~19dB below H3 at 100%, and never exceeds H2/H3 at any setting tested.
- H4 (an even harmonic, expected from an asymmetric nonlinearity) is
  present at every setting but never dominant - it's the real, honest
  output of this specific asymmetric model, not a defect to hide.
- Peak output at DRIVE=100% stays under `1.05` for a -18dBFS input
  (each `tanh()` half's own soft asymptote, never a flat/hard-clipped
  ceiling - `Tests/PluginTests.cpp`: "DRIVE=100% peak output should stay
  bounded").

These numbers come directly from running the actual shipped
`PreampProcessor` - the curve constants in `PreampCurves.h`
(`preampDriveGainMax`, `preampAsymmetryMax`, `preampOutputTrimMaxDb`/
`preampOutputTrimExponent`) were tuned against this measurement and the
level x DRIVE matrix above, not the other way around.

## Tube-character follow-up (live-testing round - a deliberate exception
## to this module's "frozen" status)

Real, direct listening feedback after the RC1/UX-polish rounds reported
the module didn't yet read as "real tube preamp" character. CLAUDE.md
marks PREAMP frozen ("do not change without a discovered objective
regression") - this pass is a deliberate, explicit, user-authorised
exception to that rule for this specific purpose, not a casual retune.

**Research**: rather than guess, real technical/review material on
Universal Audio's UA 610-B (the closest well-documented reference for a
transformer-coupled tube mic preamp) was looked up directly. Two points
from that research directly informed the changes below: (1) the 610-B is
described as delivering "creamy harmonic distortion and gritty clipping"
specifically as Input Gain increases - i.e. progressively more overt
even-harmonic-flavoured saturation with drive, not a fixed light dusting
of colour; (2) its input and output stages *each* have their own
tube-driven gain stage, each imparting its own colour - a two-stage
character this module's own single waveshaper stage does not fully
replicate (noted as a real architectural difference, not something this
pass attempted to close - see "Known tradeoffs" below).

**Changes made**:

1. **`preampAsymmetryMax` raised 0.20 -> 0.32** (`PreampCurves.h`) - a
   stronger per-half-gain asymmetry on the waveshaper, producing more
   pronounced even-harmonic (H2) content, matching real single-ended
   triode stages' typically more dominant H2 character than the original,
   more conservative figure gave. Still the exact same *bounded*
   per-half-tanh() structure the original calibration pass proved safe up
   to full (1.0) asymmetry - the original design's real failure mode
   (>400% THD/"fuzz") came from a fundamentally different *additive*
   asymmetry model that was replaced during that pass, not from this
   approach at any asymmetry short of 1.0.
2. **New "sag" stage** (`preampSagStrength`/`preampSagReleaseMs`,
   `PreampProcessor`'s new `sagEnvelopes`) - a subtle, slow
   (220ms release, over 2x slower than SAT's own 90ms "glue" compression),
   program-dependent gain reduction ahead of the waveshaper, modelling a
   real tube stage's power-supply sag under sustained/loud content. Uses
   the exact same bounded `1/(1+strength*envelope)` form SAT's own
   dynamic gain already uses (`EnvelopeFollower`, `Biquad.h`) - proven
   safe/bounded for any non-negative envelope. Deliberately kept in the
   background: max strength (`preampSagStrengthMax=0.35`) is roughly 1/6
   of SAT's own ceiling (2.2), and scales with DRIVE so it's exactly
   inert (gain=1.0) at DRIVE=0%, preserving the module's "structurally
   near-identity at 0" contract.

**Verification**: the full pre-existing PREAMP test suite
(`UNI76PreampProcessorTests`, `UNI76PreampIntegrationTests`,
`UNI76PreampHarmonicAnalysisTests`) re-ran green against both changes
with no threshold adjustments needed - the existing tolerances (written
for the original, more conservative asymmetry) already accommodated the
stronger H2 character and the new sag stage's own bounded gain
reduction. **A dedicated before/after re-measurement of the harmonic
table specifically quantifying the new H2/H3 balance was not run this
pass** - the change is verified as *safe* (all existing pass/fail
thresholds hold) but not independently *re-measured* against a fresh
target table the way the original calibration pass was - a natural
follow-up for anyone wanting the exact new numbers on record.

## Known tradeoffs

- IIR polyphase oversampling trades a small amount of phase linearity for
  much lower latency than an equiripple FIR half-band design would need at
  the same alias rejection - acceptable (arguably appropriate) for a
  character effect, would need reconsidering for a mastering-grade
  "transparent" oversampler.
- Filter coefficients for Low Cut/High Cut/colour/rounding update once per
  block (not per sample) - a deliberate CPU/quality tradeoff, standard
  practice, with a one-block lag that's inaudible at any reasonable block
  size.
- The output compensation trim is a creative curve tuned against the
  measurements above, not a mathematically exact loudness normaliser -
  by design, per the product brief ("не пытаться математически сделать
  каждый сигнал идеально одинаковым по loudness").
- THD at DRIVE=100% is intentionally level-dependent and can reach ~30%
  at very hot (-6dBFS) input - this is treated as correct analogue-style
  behaviour (hot signals saturate harder), not a defect, since it never
  becomes unbounded/rectifying (structurally impossible now) and the
  primary -18dBFS reference point stays in single digits.
- The aliasing measurement's 4kHz/2kHz probe shows a small suppression
  figure (-5.2dB) purely because there wasn't much real fold-back energy
  at that specific probe frequency to begin with, not because
  oversampling underperforms there - the 8kHz/12kHz probes (-29/-19dB)
  are more representative of the real anti-aliasing benefit.
