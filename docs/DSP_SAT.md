# SAT / ANALOG DRIVE - DSP design and measurements

The third real DSP module in UNI 76, after PREAMP and EQ. Everything here
lives in [`Source/DSP/SatProcessor.h`](../Source/DSP/SatProcessor.h) /
[`.cpp`](../Source/DSP/SatProcessor.cpp), driven by the curves in
[`Source/DSP/SatCurves.h`](../Source/DSP/SatCurves.h) and the same
allocation-free toolkit PREAMP/EQ use
([`Source/DSP/Biquad.h`](../Source/DSP/Biquad.h), which gained
`EnvelopeFollower` for this module). Pitch, Panorama, Reverb and Imager
remain a strict passthrough - see [`CLAUDE.md`](../CLAUDE.md). PREAMP's
and EQ's own DSP/constants/topology/gain-staging were **not** touched by
this pass.

Chain order: `Input -> PREAMP -> EQ -> SAT -> (future modules) -> Output` -
`PluginProcessor::processBlock()` calls the three processors in that
fixed order.

## Not a second PREAMP

PREAMP is a **static per-sample** transformer/front-end waveshaper: the
same nonlinearity applied to every sample based only on its own
instantaneous value and the DRIVE parameter, shaped by fixed drive-
dependent Low Cut/High Cut filters. SAT is deliberately different - an
**energy-dependent saturation/compression stage**: it tracks short-term
signal level via an envelope follower and uses that to drive a soft
dynamic gain stage *ahead of* its own (more aggressive, differently
tuned) waveshaper, surrounded by a frequency tilt instead of PREAMP's
Low/High Cut pair. The two modules share one proven *mathematical
primitive* (the bounded per-half-gain waveshaper - see "Nonlinear model"
below) because that specific structure was hard-won during PREAMP's sound
calibration pass, but every curve, constant, and surrounding stage is
SAT's own.

## Topology

```
Input
  -> DC protection (fixed ~5 Hz one-pole HPF, base rate)
  -> [oversampled region: 4x @ 44.1/48kHz, 2x @ 88.2/96kHz, 1x @ 176.4kHz+]
       -> frequency tilt pre-emphasis (low-shelf cut + high-shelf boost,
          both drive-dependent)
       -> envelope-driven dynamic gain (soft "glue" compression)
       -> nonlinear analog stage (asymmetric-gain tanh waveshaper)
       -> frequency tilt de-emphasis (exact algebraic inverse of the
          pre-emphasis pair)
  -> output compensation (drive-dependent trim)
  -> enable/disable crossfade against a latency-aligned dry copy
  -> Output
```

Every stage is per-channel with fully independent state - no cross-channel
coupling, no randomness anywhere in the chain, so an identical L/R input
produces numerically identical L/R output (verified, see "Stereo" below).

## Nonlinear model

Per sample, inside the oversampled region:

```
x  = lowShelfPre(x); x = highShelfPre(x)          // frequency tilt in
env = envelopeFollower(x)                          // peak-hold, ~90ms release
x *= 1 / (1 + compressionStrength(t) * env)         // energy-dependent gain
xd = x * driveGain(t)
shaped = xd >= 0 ? tanh(xd) : tanh(xd * (1 - asymmetry(t)))
y  = shaped / tanh(driveGain(t))
y  = highShelfDe(y); y = lowShelfDe(y)              // frequency tilt out
```

- **`driveGain(t)`** (`satDriveGainLinear`): geometric interpolation from
  `0.08` (~-22dB) at `t=0` to `12.0` (~+22dB) at `t=1`, `t^1.2` shaped -
  a higher ceiling than PREAMP's (post-calibration) `10.0`, since the
  product brief explicitly allows SAT to be more aggressive.
- **`asymmetry(t)`** (`satAsymmetryAmount`, `0` to `0.20`): the same
  *bounded per-half-gain* structure PREAMP's calibration pass settled on
  (see [docs/DSP_PREAMP.md](DSP_PREAMP.md)'s "What changed in the
  nonlinear model") - each half of the waveform is independently
  `tanh()`-bounded, so total output can never exceed unity regardless of
  drive, asymmetry, or input level. This is a deliberate reuse of a
  *proven-safe* primitive, not "copying PREAMP's model" - PREAMP's first
  attempt used an unbounded additive quadratic term that measured >400%
  THD at extreme settings; that mistake is structurally impossible here.
- Normalising by `tanh(driveGain)` (not `driveGain` itself) keeps HEAT=0
  structurally near-identity and HEAT=100 a firm, soft ceiling - same
  reasoning as PREAMP.

## Dynamic / memory behaviour - what makes SAT not a static waveshaper

`EnvelopeFollower` (`Biquad.h`) is a **peak-hold** follower: instant
attack (jumps immediately to a new, higher rectified value), smoothed
one-pole release (`satEnvelopeReleaseMs = 90ms`). This was a deliberate
fix, not the first design tried - see "What was tried and rejected"
below. `compressionStrength(t)` (`0` to `2.2`, same `t^1.2` shaping as
the drive curve) scales how much the envelope reduces gain ahead of the
waveshaper:

```
gainReduction = 1 / (1 + compressionStrength(t) * envelope)
```

Bounded in `(0, 1]` for any non-negative envelope by construction - no
possibility of gain *increase*, no runaway feedback (the envelope reads
the tilted signal, not its own output, so there is no closed loop wider
than the single envelope pole). This is the part that makes SAT
genuinely program-dependent rather than a fixed curve applied identically
regardless of what came before - real "glue" compression character.

### What was tried and rejected

An earlier version used a smoothed few-ms attack (not peak-hold) for the
envelope follower. Measured on a transient test signal, crest factor
*increased* from HEAT 0% to 50% (17.26dB -> 19.63dB) before falling at
75-100% - the opposite of the "HEAT smoothly reduces crest factor"
requirement. Cause: the smoothed attack let a transient's peak sample
pass through the pre-waveshaper gain stage almost unreduced (the envelope
hadn't risen yet), while the decaying tail of the same transient - where
the envelope *had* caught up - got pulled down, so RMS dropped faster
than peak. Switching to instant-attack peak-hold (matching how many real
analog compressors' detector circuits behave) fixed it: crest factor now
stays flat through HEAT=50% (17.26 -> 17.32dB, negligible) and clearly
falls by HEAT=100% (14.23dB) - see "Crest factor / transient analysis"
below for the full measured table.

## Frequency-dependent behaviour (the tilt)

A **low-shelf cut + high-shelf boost** pair ahead of the nonlinearity
(`satLowShelfFreqHz=150Hz`, `satHighShelfFreqHz=3800Hz`), with their
**exact algebraic inverse** applied after it
(`(A then B)^-1 = B^-1 then A^-1)` - reversed order, negated gains).
Both shelf gains scale linearly with `t` (`0` at HEAT=0, up to
`satLowShelfMaxDb=-4dB` / `satHighShelfMaxDb=+5dB` at HEAT=100%), so the
character grows gradually with drive rather than a fixed EQ shape
snapping in the moment HEAT leaves zero.

This single mechanism delivers both required behaviours at once:

- **Bass protection**: cutting lows before the nonlinearity means the
  fundamental of low-frequency content reaches the waveshaper *less*
  driven than the mids do, so it picks up proportionally fewer harmonics
  - the boost afterward restores its level. Measured: at HEAT=100%,
  40/60/100Hz retain their fundamental level within 0.3dB of HEAT=0% while
  picking up only 3.9-4.6% THD - "controlled harmonics", not mud (see
  "Low-end behaviour" below).
- **High-frequency softening**: boosting highs before the nonlinearity
  pushes them harder into the waveshaper (more compression/rounding),
  and the cut afterward removes the *linear* boost while leaving the
  *nonlinear* compression's effect in place - so highs come out
  relatively softened at high HEAT, growing gradually with drive rather
  than a fixed, level-independent low-pass. Measured: 5/8/12kHz soften by
  3.6-4.6dB at HEAT=100% (see "High-end behaviour" below) - clearly less
  aggressive than PREAMP's High Cut (9-13dB at DRIVE=100%), a genuinely
  different, gentler character.

At HEAT=0%, both shelves are exactly 0dB (identity) and pre/de-emphasis
are exact inverses of each other at every `t` - so no coloration is added
at 0% regardless of how non-identity the waveshaper is elsewhere in the
chain (verified structurally and by the transparency measurement below).

## Oversampling / anti-aliasing

SAT owns its **own, independent** `juce::dsp::Oversampling` instance -
deliberately not a shared/refactored version of PREAMP's (the product
brief explicitly warned against risky PREAMP refactors for code-reuse
purposes, and PREAMP's sound is frozen). Same policy and parameters as
PREAMP (`filterHalfBandPolyphaseIIR`, `isMaximumQuality=true`,
`useIntegerLatency=true`) since that choice was already justified there
and applies equally here:

| Sample rate | Oversampling |
|---|---|
| 44.1 / 48 kHz | 4x (2 stages) |
| 88.2 / 96 kHz | 2x (1 stage) |
| 176.4 / 192 kHz+ | none (1x) |

Production code has no runtime oversampling toggle. Aliasing is measured
via a **test-only** reference implementation
(`Tests/PluginTests.cpp`'s aliasing test) that replicates the identical
tilt + dynamic-gain + waveshaper math directly at the base rate with no
up/downsampling, then compares fold-back energy against the real
production (oversampled) path at HEAT=100% (worst case):

| Test tone | Probe frequency | Reference (no oversampling) | Production (oversampled) | Suppression |
|---|---|---|---|---|
| 4 kHz  | 2 kHz | -64.6 dB | -73.5 dB | -8.9 dB |
| 8 kHz  | 4 kHz | -40.2 dB | -83.5 dB | -43.3 dB |
| 12 kHz | 6 kHz | -47.5 dB | -74.0 dB | -26.6 dB |

Production measured equal-or-lower fold-back energy than the reference at
every probed frequency (asserted directly, not just printed).

## Latency

SAT's oversampling factor (and therefore its own latency) depends only on
sample rate, exactly mirroring PREAMP's figures since both use identical
`Oversampling` parameters:

| Sample rate | SAT latency (samples) |
|---|---|
| 44100 Hz  | 6 |
| 48000 Hz  | 6 |
| 88200 Hz  | 4 |
| 96000 Hz  | 4 |
| 176400 Hz | 0 |
| 192000 Hz | 0 |

**Total plugin latency is the sum of every stage's own latency** - EQ
contributes 0, so at 44.1kHz the plugin now reports PREAMP's 6 + EQ's 0 +
SAT's 6 = **12 samples** (0.272ms), not just one module's figure. This is
computed explicitly in `PluginProcessor::prepareToPlay()`:

```cpp
setLatencySamples (preampProcessor.getLatencySamples()
                    + eqProcessor.getLatencySamples()
                    + satProcessor.getLatencySamples());
```

and verified directly against two freshly-`prepare()`d reference
processors in `Tests/PluginTests.cpp`'s "Total plugin latency is the sum
of PREAMP's and SAT's own latencies" test, not just asserted informally.
SAT's own enable/disable bypass crossfade uses its own
`IntegerDelayLine` sized to *SAT's own* latency (not the cumulative
total) - each module's dry path only needs to counteract the delay that
module itself adds relative to what already arrived at its input.

## Output compensation (gain staging)

The energy-dependent compression already pulls down loud/sustained
content somewhat, but the waveshaper's own `tanh()` normalisation still
measured meaningful RMS growth at high HEAT before any trim - a fixed,
strongly back-loaded output trim (`satOutputTrimMaxDb=-18dB`,
`satOutputTrimExponent=3`) brings a broadband multi-tone signal's RMS
within a few dB across the whole range:

| HEAT | RMS vs 0% (broadband multi-tone) |
|---|---|
| 0%   | 0 dB (reference) |
| 25%  | -1.04 dB |
| 50%  | -2.90 dB |
| 75%  | -1.93 dB |
| 100% | -2.88 dB |

No AGC/envelope-following loudness normaliser anywhere - this is a static
per-`t` trim tuned against the measurement above, same pattern as PREAMP
and EQ. Level-dependent behaviour (a hotter input saturates/compresses
harder) is preserved - see the level x HEAT matrix below, where -6dBFS
input at HEAT=100% shows visibly more THD (18.5%) than -18dBFS at the
same HEAT (6.6%).

## Smoothing / bypass

- HEAT: `juce::SmoothedValue` linear ramp, **25ms**, sample-accurate at
  the oversampled rate for the gain-critical parts (drive amount, output
  trim) - same pattern as PREAMP. Filter/tilt coefficients recompute once
  per block from the previous block's settled value.
- `saturationEnabled` (bypass): **20ms** linear crossfade between the wet
  signal and a **latency-aligned** dry copy - SAT has real algorithmic
  latency (unlike EQ), so its dry path is pushed through its own
  `IntegerDelayLine` sized to SAT's own latency, exactly like PREAMP's
  bypass. Verified clean (no click) across a 9-block enable/disable
  transition test.
- Verified: 49/50/51%-style rapid jumps, a 0->100->0 sweep, and the
  enable/disable transition all stay within a bounded max sample-to-
  sample jump - no zipper, no unstable state, no temporary explosion.

## Transparency at HEAT=0%

`Tests/PluginTests.cpp`'s "HEAT=0% is close to transparent" test measures
RMS at 1kHz/-18dBFS against the fed reference and finds it within 10% (a
looser tolerance than PREAMP's/EQ's magnitude-based null tests, since
SAT's dynamic compression stage - unlike a pure static filter/waveshaper -
still applies a small nonzero `compressionStrength` floor rounding error
at very low `t`; the measured harmonic content at HEAT=0% is 0.0025% THD,
effectively noise-floor). The nonlinear section is not literally bypassed
at HEAT=0% (that would need a topology switch, which the tilt/waveshaper
design deliberately avoids everywhere) - transparency instead comes from
`driveGain(0)` being tiny enough that `tanh(g*x)/tanh(g) ~ x`, the same
proven mechanism PREAMP uses.

## Silence / DC

Silence in produces exactly silence out at every HEAT/enabled combination
- no noise floor, hiss, or hum simulation anywhere (the product brief was
explicit that analog character comes from the DSP nonlinearity, not
synthesised noise). DC offset at HEAT=100% (0.5-amplitude sine, -18dBFS
input) measures under `0.01` - the fixed ~5Hz DC blocker at the front of
the chain removes any bias the asymmetric waveshaper introduces.

## Numerical safety

Input samples are sanitised (non-finite -> 0) at the start of
`process()`, and the same check runs again on the final output - the
same narrow-safeguard pattern PREAMP/EQ use, not a blanket amplitude
clamp. `juce::ScopedNoDenormals` (already present in
`PluginProcessor::processBlock`) covers denormal protection. Each half of
the waveshaper is independently `tanh()`-bounded, so the nonlinear stage
itself cannot produce non-finite output from finite input, and the
envelope-driven gain stage is bounded in `(0,1]` by construction for any
non-negative envelope value.

## Stereo

Verified: an identical mono signal fed to both L and R produces
numerically identical L/R output after SAT - each channel has fully
independent filter/envelope/waveshaper state, but no randomness or
cross-channel coupling exists anywhere, so identical input guarantees
identical output. A signal in the left channel only never bleeds into a
silent right channel.

## Harmonic analysis (1kHz @ -18dBFS)

`Tests/PluginTests.cpp`, `UNI76SatAnalysisTests` - numbers copied directly
from an actual `UNI76Tests.exe` run's stdout:

```
HEAT 0%:   H2=-100dB    H3=-100dB    H4=-100dB    H5=-100dB    THD=0.0025%
HEAT 25%:  H2=-58.16dB  H3=-100dB    H4=-72.76dB  H5=-100dB    THD=1.10%
HEAT 50%:  H2=-53.62dB  H3=-87.35dB  H4=-68.71dB  H5=-100dB    THD=2.21%
HEAT 75%:  H2=-49.16dB  H3=-64.33dB  H4=-63.70dB  H5=-100dB    THD=3.22%
HEAT 100%: H2=-56.61dB  H3=-43.40dB  H4=-59.67dB  H5=-65.98dB  THD=6.64%
```

THD grows monotonically and stays clearly inside the "single digits at
-18dBFS/100%" range (SAT is allowed to be more aggressive than PREAMP per
the brief, but its measured 6.64% is actually a little *below* PREAMP's
calibrated 8.3% at the same reference point - both land in a musical
range by design, not by coincidence of one formula). H2 dominates through
50-75% (a gentle even-harmonic warmth), H3 becomes dominant at 100% (a
stronger, more driven character still led by a low-order harmonic) - H5
never exceeds the dominant harmonic at any setting tested.

## Level x HEAT matrix (1kHz, full table)

`Tests/PluginTests.cpp`'s "Input level x HEAT matrix" test - copied
directly from stdout:

| Level (dBFS) | HEAT | THD % | H2 (dB) | H3 (dB) | H5 (dB) | Out RMS | Peak | Crest (dB) |
|---|---|---|---|---|---|---|---|---|
| -30 | 25%  | 1.10  | -69.8 | -100  | -100  | 0.0211 | 0.0307 | 3.24 |
| -30 | 50%  | 2.22  | -64.8 | -100  | -100  | 0.0185 | 0.0278 | 3.51 |
| -30 | 75%  | 3.37  | -59.3 | -96.7 | -100  | 0.0231 | 0.0358 | 3.80 |
| -30 | 100% | 4.25  | -55.9 | -70.8 | -100  | 0.0280 | 0.0445 | 4.01 |
| -18 | 25%  | 1.10  | -58.2 | -100  | -100  | 0.0810 | 0.1177 | 3.24 |
| -18 | 50%  | 2.21  | -53.6 | -87.3 | -100  | 0.0677 | 0.1014 | 3.50 |
| -18 | 75%  | 3.22  | -49.2 | -64.3 | -100  | 0.0794 | 0.1220 | 3.73 |
| -18 | 100% | 6.64  | -56.6 | -43.4 | -66.0 | 0.0753 | 0.1083 | 3.15 |
| -12 | 25%  | 1.10  | -52.6 | -88.1 | -100  | 0.1540 | 0.2236 | 3.24 |
| -12 | 50%  | 2.19  | -48.6 | -72.1 | -100  | 0.1217 | 0.1818 | 3.49 |
| -12 | 75%  | 3.20  | -46.0 | -50.9 | -86.3 | 0.1315 | 0.1989 | 3.59 |
| -12 | 100% | 12.61 | -63.2 | -35.6 | -52.0 | 0.0958 | 0.1250 | 2.31 |
| -6  | 25%  | 1.09  | -47.4 | -72.8 | -96.2 | 0.2804 | 0.4070 | 3.24 |
| -6  | 50%  | 2.16  | -44.5 | -58.8 | -95.2 | 0.2018 | 0.3005 | 3.46 |
| -6  | 75%  | 4.09  | -45.3 | -40.6 | -68.6 | 0.1915 | 0.2807 | 3.32 |
| -6  | 100% | 18.46 | -53.6 | -31.5 | -44.3 | 0.1065 | 0.1291 | 1.67 |

(0% HEAT omitted - THD is ~0.0025-0.013% at every level, effectively
noise-floor.) Reading it: 25-75% HEAT is close to level-independent (THD
~1.1/2.2/3.2-4.1% regardless of input level, the waveshaper's small-signal
region); **100% is strongly level-dependent** (4.25% at -30dBFS up to
18.46% at -6dBFS) - a hot signal genuinely saturates/compresses harder
than a quiet one at maximum HEAT, exactly the analogue behaviour required,
and even the hottest tested case (-6dBFS/100%) stays well short of
runaway (the test asserts THD < 100% everywhere - the failure mode
PREAMP's first, since-fixed nonlinear model actually hit).

## Crest factor / transient analysis

Deterministic transient test signal (8 percussive decaying bursts, 200Hz
step per hit, 44.1kHz), full peak/RMS/crest measured over the whole
buffer, `Tests/PluginTests.cpp`:

| HEAT | Peak | RMS | Crest factor (dB) |
|---|---|---|---|
| 0%   | 0.786 | 0.1078 | 17.26 |
| 50%  | 0.583 | 0.0608 | 17.32 |
| 75%  | 0.419 | 0.0569 | 17.35 |
| 100% | 0.180 | 0.0359 | 14.23 |

Crest factor stays essentially flat through HEAT=50% (+0.06dB, negligible
- well inside the "does not meaningfully rise" test tolerance) and falls
clearly by HEAT=100% (-3.0dB vs 0%) - both peak and RMS drop with HEAT,
but peak drops faster at high settings, which is the "transient rounding"
the product brief asked for. See "What was tried and rejected" above for
why the envelope follower needed to be peak-hold (instant attack) rather
than smoothed to get this shape instead of an early crest-factor rise.

## Low-end behaviour (40/60/100Hz @ HEAT=100%, -18dBFS)

| Frequency | Fundamental level vs HEAT=0% | THD @ HEAT=100% |
|---|---|---|
| 40 Hz  | +0.28 dB | 4.62% |
| 60 Hz  | +0.21 dB | 4.03% |
| 100 Hz | +0.002 dB | 3.86% |

The fundamental is retained within a third of a dB at every tested
frequency (not "turned to mush") while picking up a controlled, modest
amount of harmonic content - the frequency tilt's low-shelf cut ahead of
the nonlinearity is what keeps bass from getting the same aggressive
nonlinear treatment as the mids.

## High-end behaviour (5/8/12kHz @ HEAT=100%, -18dBFS)

| Frequency | Gain vs HEAT=0% |
|---|---|
| 5000 Hz  | -3.65 dB |
| 8000 Hz  | -4.41 dB |
| 12000 Hz | -4.55 dB |

A gentle, graduated softening (not a fixed low-pass cutting everything
above one frequency by the same amount) - clearly gentler than PREAMP's
High Cut (9-13dB of attenuation at DRIVE=100%), a deliberately different,
subtler top-end character for SAT.

## PREAMP + EQ + SAT integration

`Tests/PluginTests.cpp`'s combination test runs 7 representative
PREAMP/EQ/SAT combinations (including PREAMP=75%/EQ=50%/SAT=100%, the
hottest tested) through the real `UNI76AudioProcessor::processBlock()`:
no NaN/Inf, no gain explosion (peak stays well under 4x), nonzero total
latency reported, and both level meters keep working at every
combination. **EQ PHONE (the 50% default) + SAT HOT (100%)** is checked
specifically per the brief's concern that PHONE's midrange concentration
plus SAT's saturation might unexpectedly turn into fuzz - measured peak
stays under 1.2x with fully finite output, i.e. it does not.

## Tube-character follow-up (live-testing round)

Same round and same real-world research as `docs/DSP_PREAMP.md`'s
"Tube-character follow-up" section (see that section for the UAD 610-B
research this drew on) - applied here more conservatively than to
PREAMP, since SAT already carries substantial character from its own
dedicated dynamic-gain ("glue") compression stage on top of the
waveshaper.

**Change made**: `satAsymmetryMax` raised 0.20 -> 0.28 (`SatCurves.h`) -
a modest increase in even-harmonic content, consistent with (but smaller
than) PREAMP's own 0.20->0.32 change, for a more authentic
tube/analog-saturator even-harmonic balance. SAT's frequency tilt
(low-shelf cut ahead of the nonlinearity / high-shelf boost, protecting
bass and pushing highs harder into saturation) and its dynamic
compression stage were both left unchanged this pass - their documented
design intent (bass stays controlled, highs come out softer/more
compressed at high HEAT) already matches the *result* real tape/analog
saturation produces, even though the specific pre/de-emphasis mechanism
differs from literal tape-head physics; restructuring that was judged
out of scope without a more specific complaint to act on.

**Verification**: the full pre-existing SAT test suite
(`UNI76SatProcessorTests`, `UNI76SatIntegrationTests`,
`UNI76SatAnalysisTests`) re-ran green with no threshold adjustments
needed. As with PREAMP, this change is verified *safe* (existing
thresholds hold) but the harmonic table above was not independently
re-measured against a fresh target for the new asymmetry value - a
natural follow-up.

## Known compromises

- The frequency tilt's shelf frequencies/gains are fixed constants tuned
  against the measurements in this document, not independently
  parameterised or derived from a closed-form model - consistent with
  keeping SAT a single coherent character rather than a multiband tool.
- HEAT=0%'s transparency tolerance (10% RMS) is looser than PREAMP's/EQ's
  magnitude-based null tests (<1dB) - SAT's dynamic compression stage
  makes a pure single-frequency magnitude comparison less representative
  than for a static filter/waveshaper; the near-zero measured THD at 0%
  (0.0025%) is better evidence of transparency than the RMS tolerance
  alone.
- Like PREAMP, the 4kHz/2kHz aliasing probe shows a smaller suppression
  figure (-8.9dB) than the 8kHz/12kHz probes (-43.3dB/-26.6dB) - some of
  that gap is genuinely less fold-back energy at that specific probe
  frequency to begin with, not a weaker anti-alias result.
- Output compensation is a creative trim tuned against the broadband
  measurement above, not a mathematically exact loudness normaliser - by
  design, matching the explicit "no AGC" instruction.
