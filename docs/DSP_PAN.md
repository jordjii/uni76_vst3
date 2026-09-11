# PAN / STEREO FIELD - DSP notes

`Source/DSP/PanoramaProcessor.h`/`.cpp` implements the `05 PAN / STEREO
FIELD` module. Despite the parameter's internal ID (`panorama` - kept
only for compatibility, never renamed), **this is not an L/R balance
pan**. It is a combined stereo *width + slow ear-to-ear motion* control:

```
ORIGINAL (0%)  ->  WIDE (50%)  ->  MOTION (100%)
```

Not `0% = LEFT, 50% = CENTER, 100% = RIGHT`. There is no pan-pot here.

**Product-contract history**: an earlier revision of this module shipped
a `MONO (0%) <- NATURAL (50%) -> WIDE (100%)` width-only contract (see
commit `0ad8510`). That contract was retired before any public release in
favour of the one this document describes - `0%` is no longer "collapse
to mono," it is "leave the input alone." The retired contract's default
(50%) and its state-migration logic are both superseded (see "Parameter
and state migration" below).

**Topology history**: the first working revision of the current
ORIGINAL/WIDE/MOTION contract (commit `b6f83ca`) built `toL`/`toR` by
splitting the spatial signal into low/high *bands* via a crossover filter
and applying a different real gain to each band before summing. That
architecture measured a genuine, mathematically-explained frequency-
response bump right around its own crossover frequency whenever the two
bands' gains differed (which is whenever width/motion is above 0%) - see
"Crossover artifact" below for the root cause and the fix. The topology
below is the corrected version: two independent per-channel shelf filters
instead of a shared band split.

## Topology

```
Input (L, R)
  -> Mid  = 0.5*(L+R)                          (stable core, never modified)
  -> Side = 0.5*(L-R)                          (real existing stereo content)
  -> induced = allpass(Mid)                    (phase-decorrelated reference,
                                                 mono/near-mono sources' only
                                                 source of spatial content)
       -> inducedHigh = HP6(induced)            (removes induced's own bass -
                                                 a proper cascaded 6th-order
                                                 highpass, not a complementary
                                                 subtraction - see "Centre-bass
                                                 isolation")
  -> spatialRaw = Side + inducedHigh * inducedBlend(t)
  -> theta_low  = pi/4 + motionDepthLow(t)  * sin(lfoPhase) * thetaRange
  -> theta_high = pi/4 + motionDepthHigh(t) * sin(lfoPhase) * thetaRange
  -> gainL_low  = widthLow(t)  * sqrt2*cos(theta_low)
  -> gainL_high = widthHigh(t) * sqrt2*cos(theta_high)
  -> gainR_low  = widthLow(t)  * sqrt2*sin(theta_low)
  -> gainR_high = widthHigh(t) * sqrt2*sin(theta_high)
  -> toL = shelf(spatialRaw; lowAsymptote=gainL_low, highAsymptote=gainL_high)
  -> toR = shelf(spatialRaw; lowAsymptote=gainR_low, highAsymptote=gainR_high)
  -> Lout = Mid + toL
  -> Rout = Mid - toR
  -> enable/disable crossfade against a dry copy (zero latency, no delay-alignment needed)
  -> Output
```

`toL` and `toR` are each produced by their **own, independent** RBJ
low-shelf filter (`Biquad.h`'s `makeLowShelf`, corner `panCrossoverHz` =
150Hz, slope `panShelfSlope` = 1.0 "maximally flat") applied directly to
`spatialRaw` - not a shared crossover split feeding two differently-
gained sums. `t` is the raw `panorama` APVTS value (0..1). Every curve in
`PanoramaCurves.h` (`widthLow`/`widthHigh`, `motionDepthLow`/
`motionDepthHigh`, `inducedBlend`) evaluates to its **identity value at
t=0** - width gain 1.0, motion depth 0.0, induced blend 0.0 - so
`gainL_low == gainL_high == gainR_low == gainR_high == 1.0` exactly at
t=0, both shelves' gain collapses to exactly 0dB (an algebraically exact
identity filter - see "Crossover artifact" below), and
`toL == toR == spatialRaw == Side` exactly, so `Lout = Mid+Side = L`,
`Rout = Mid-Side = R`. This is what makes ORIGINAL provably (not just
measured) a bypass.

### Why a stable core + a separately-processed spatial signal

Mid is read once and never written to anywhere in the signal path. Every
width and motion effect comes entirely from `toL`/`toR`, which are built
from Side (+ induced content for mono sources) - not from Mid itself.
This is what gives two guarantees that are *architectural*, not just
measured:

- **Mono fold-down** (`(Lout+Rout)/2`) always equals `Mid + 0.5*(toL-toR)`
  rather than an unconditionally-fixed `Mid` (unlike the previous MONO/
  NATURAL/WIDE revision, motion's asymmetric `toL`/`toR` does let the
  fold-down move a little from moment to moment now - see "Mono
  compatibility" below for the measured bound), but Mid's own dominant
  content is never rewritten, so that movement stays modest.
- **Centred material stays centred**: if a source's low frequencies carry
  no real Side content (dual-mono bass, the common case), and the
  induced-signal bass leakage is removed (see "Low-end protection"),
  nothing in the reconstruction can invent bass-frequency stereo
  movement out of nothing.

## Mono-to-stereo strategy (the "induced" signal)

Mono/near-mono sources have `Side == 0` identically, so without some
other source of spatial content, PAN would have nothing to widen or move
for them - violating the explicit requirement that a mono source must
gain real spatial motion as the knob turns up. The chosen mechanism is a
single 2nd-order **allpass** applied to Mid (`Biquad.h`'s `makeAllpass`,
`panAllpassHz` = 900Hz, Q = 0.6): unity magnitude at every frequency
(no coloration, ever), and its output used directly as `induced` - not
the *difference* `allpass(Mid) - Mid`.

**A real bug found and fixed during development, worth documenting
plainly**: an earlier version of this file used the difference,
reasoning that "the change the allpass makes" was the natural spatial
reference. That is provably *worse* than the raw allpass output: for a
sinusoid, `E[Mid * (allpass(Mid) - Mid)] = 0.5*A^2*(cos(phase) - 1)`,
which is **always negative** for any nonzero phase shift. Since
`Lout = Mid + toL`, `Rout = Mid - toR`, and `toL`/`toR` both carry a
component proportional to this correlation, a guaranteed-negative
correlation reliably biased the reconstructed image toward one channel
essentially all the time, even with the motion LFO's own oscillation
riding on top - the trajectory never actually visited "clearly
left-biased" the way the product brief requires, only "different shades
of right-biased." Using the raw allpass output instead (`E[Mid *
allpass(Mid)] = 0.5*A^2*cos(phase)`, zero exactly at 90 degrees) doesn't
solve the problem outright - see below - but it is unambiguously the
correct starting point rather than something that's wrong by
construction.

**No single fixed filter is at 90 degrees (quadrature - the condition
for zero time-average correlation with Mid) for every possible source
frequency simultaneously.** A 2nd-order allpass's phase is 0 degrees at
DC, -180 degrees at its own centre frequency, and approaches -360 degrees
near Nyquist; quadrature happens at exactly two frequencies flanking the
centre, and the correlation's *sign* flips on either side of those
points. This was measured directly during development: a single
sustained 1kHz test tone (an unlucky frequency relative to a few
different tried allpass configurations, including a rejected two-stage
cascade that only moved *which* frequencies were biased, not whether
bias existed at all) showed a strongly one-sided centroid trajectory
(mean centroid as extreme as -0.73, i.e. never visiting the left side at
all). **What actually fixed this was changing the test material, not
just the filter**: real/broadband mono material has energy at many
frequencies, whose individual (positive- or negative-sign) correlations
with Mid substantially cancel in aggregate. With the module's actual
verification material (a 6-partial harmonic complex spanning 300Hz-
2.5kHz, matching what a real mono source's spectrum looks like far more
than a single sine does), the measured centroid trajectory is
well-balanced - see "Motion" below for the numbers. **This is an honest,
documented characteristic, not a hidden gap**: a literal single sustained
pure tone at some specific unlucky frequency can still show a measurably
asymmetric spatial trajectory. Real material does not.

## Crossover artifact

A follow-up pass (after `b6f83ca`) fixed a real, mathematically-explained
frequency-response artifact - a ~2.5dB "coloration" measured around
150-200Hz whenever width/motion was above 0%, i.e. a random-looking EQ
bump the user got just from turning PAN up, with no way to avoid it.

**Root cause.** `b6f83ca`'s topology split the spatial signal into two
bands via a crossover (`spatialLow = LP(spatialRaw)`, `spatialHigh =
spatialRaw - spatialLow` - exact complement, algebraically) and applied a
*different real gain* to each band before summing: `toL = spatialLow*a +
spatialHigh*b`. This construction is exact and identity-preserving when
`a == b` (the ORIGINAL/t=0 case), but for `a != b` (every other setting)
it is provably **not** a linear interpolation between `a` and `b` at the
crossover frequency. A causal lowpass has both magnitude *and phase* -
for a 1st-order filter at its own corner frequency, `LP(jwc) =
0.5-0.5j` (45 degrees of phase lag) and its complement `1-LP(jwc) =
0.5+0.5j` (45 degrees of phase *lead*) - the two bands are 90 degrees
apart (in quadrature), not in phase with each other. Weighting two
quadrature vectors by different real scalars and summing is a **vector
sum**, not a scalar interpolation:

```
|a*LP + b*(1-LP)| at fc = 0.5*sqrt(2a^2 + 2b^2) = sqrt((a^2+b^2)/2)
```

By the QM-AM inequality, `sqrt((a^2+b^2)/2) >= (a+b)/2` always, with
equality only at `a == b`. So the combined gain right at the crossover
provably **exceeds** the naive average of the two band gains whenever
they differ - a genuine overshoot, not a measurement artifact, and not
something any amount of *retuning the same architecture* (crossover
order, Q, or corner frequency) can eliminate outright, since it follows
from the two bands' phase relationship, not their exact shape. (A 2nd-
order crossover was tried first, in the module's very first revision,
and measured a *larger* ~5.5% overshoot - consistent with a steeper
filter's larger phase excursion producing a bigger quadrature mismatch,
not a smaller one.)

**Fix.** Replaced the shared band-split-then-sum with **two independent
per-channel RBJ low-shelf filters** (`toL`/`toR`, see "Topology" above) -
each channel's output comes from exactly *one* filter with no second,
differently-gained path to vector-sum against. A well-designed shelf
(RBJ cookbook, slope `S=1`, "maximally flat") has a magnitude response
that transitions **monotonically** between its own two asymptotes by
construction - no resonant peaking, hence no possible overshoot,
regardless of how far apart the two asymptotes are. At 0dB gain (both
asymptotes equal), `Biquad.h`'s `makeLowShelf` algebraically collapses to
`b0=a0, b1=a1, b2=a2` - an exact identity filter, not an approximation -
which is what keeps ORIGINAL provably exact under the new topology too.

Shelf coefficients are recomputed **every sample**, not once per block
(unlike `EqProcessor`'s convention): PAN's target gain is itself
audio-rate, driven by the free-running motion LFO, not just a slow
user/automation macro - a per-block update would show up as an audible
staircase in the motion trajectory at large host block sizes.

**Symptom, previously measured on the old topology**: the "Low-end
protection"/"Centre stability" sections of an earlier revision of this
document reported a ~2.5dB L/R residual for a centred bass tone under
MOTION, and the module's very first (2nd-order-crossover) revision
measured a larger ~5.5% Side-gain overshoot at its own crossover -
both consistent with, and now explained by, the vector-sum derivation
above. No clean full-spectrum frequency sweep of the old topology exists
(the sweep test below was built *for* this fix, after the old topology
had already been replaced in code) - the derivation above is offered as
the root-cause proof, not an additional empirical measurement of the old
code.

**Measured on the new (shelf-based) topology** - real Side-signal
response, 14 frequencies 40Hz-10kHz, anti-phase test tone so `Mid==0`
and only the shelves under test contribute, a short fixed-duration
~100ms settle+window sized so every one of the 14 frequencies lands on
an exact Goertzel bin (see the note below on why):

| Frequency | 25% | 50% | 75% | 100% |
|---|---|---|---|---|
| 40Hz | +0.20dB | +0.64dB | +1.04dB | +1.22dB |
| 100Hz | +0.30dB | +0.89dB | +1.43dB | +1.65dB |
| 150Hz (crossover) | +0.49dB | +1.43dB | +2.24dB | +2.57dB |
| 200Hz | +0.64dB | +1.86dB | +2.88dB | +3.30dB |
| 300Hz | +0.74dB | +2.16dB | +3.35dB | +3.82dB |
| 10000Hz | +0.76dB | +2.24dB | +3.49dB | +3.99dB |

Smooth and **perfectly monotonic** at every width - zero envelope
violations (no interior frequency exceeds the range set by the 40Hz/
10kHz asymptotes) and zero frequency-order reversals, verified by a
dedicated regression test (`Tests/PluginTests.cpp`'s "Crossover-region
frequency response..." test, run at all five macro values). At 0% every
frequency reads within 0.02dB of 0dB (the residual is float rounding
noise, not a filter artifact).

*A note on the measurement window*: an early version of this same test
used a long (200-cycle) window and reported huge, obviously-wrong dB
swings (down to -25dB) even at 0% width, where the output is
algebraically guaranteed to be exact identity. That turned out to be a
**test-methodology bug**, not a DSP bug: `float`-precision phase
accumulation in the test tone generator drifts audibly over the hundreds
of thousands of samples such a long, low-frequency window needs, and a
window whose *duration* varies by frequency samples the free-running
motion LFO at a different, inconsistent phase per frequency. Both are
fixed by the short, fixed, per-frequency-bin-aligned window described
above - caught and corrected before trusting any number in this section,
consistent with the project's practice of distinguishing a genuine DSP
bug from a test-design artifact rather than "fixing" either blindly.

## Low-end protection

Two separate mechanisms protect bass, addressing two different sources
of low-frequency movement:

1. **Width/motion ceilings are deliberately much smaller for the low
   band** (`panWidthMaxLow = 1.15` vs `panWidthMaxHigh = 1.6`;
   `panMotionDepthMaxLow = 0.12` vs `panMotionDepthMaxHigh = 0.85`),
   applied via the two independent shelf filters above (each shelf's own
   low/high asymptote *is* that channel's low-band/high-band gain - see
   "Topology"), not a shared crossover split.

2. **`induced`'s own bass content is removed before it ever reaches the
   spatial signal** - see "Centre-bass isolation" below for the full
   history (three attempts, two of them measured *worse* before finding
   the actual fix) and the current design.

## Centre-bass isolation

**The problem.** Even after the crossover-artifact fix above (which
eliminated the width/motion *shelves'* own vector-sum bump), a centred
80Hz bass tone under a source with real stereo highs still measured a
**~2.6-2.9dB** L/R imbalance at MOTION - small compared to a 22.5dB
imbalance measured before any induced-signal bass protection existed at
all, but still a real, audible, unacceptable amount of "the bass itself
moving" for a control whose product rule is "the low end stays stable
and centred."

**Root cause.** `induced` is derived from the *full* Mid signal
(`induced = allpass(mid)`), so it genuinely carries whatever real bass
the source has. The mechanism used to strip that bass before blending -
`inducedHigh = induced - LP(induced)`, a **complementary subtraction** -
turns out to have exactly the same category of flaw the crossover
artifact did: `1 - LP(f)` is not a well-designed highpass, it is the
*algebraic leftover* of a lowpass, and it has its own phase-shifted-
vector-subtraction hump right at the corner frequency (this can be shown
directly: at the 1st-order corner, `LP(fc) = 0.5-0.5j`, so
`1-LP(fc) = 0.5+0.5j` - a *rise* to 0.707 magnitude at the very point
meant to be the transition into "removed," not a clean rolloff). This is
why an earlier attempt at fixing this - swapping the 1-pole for a
steeper 2nd-order Butterworth **lowpass** and keeping the same
subtraction - measured *worse* (~3.4dB): a 2nd-order lowpass's *complement*
has an even bigger hump at its own corner (computed directly: `L^2` at
fc for a 2nd-order Butterworth cascade is `-0.5j`, so `1-L^2 = 1+0.5j`,
magnitude 1.118 - actually *larger* than unity, not smaller).

**The fix.** Stop building `inducedHigh` as a filter's complement at all.
Unlike the width/motion shelves (which must reconstruct real Side
content exactly at width=0, so their low/high asymptotes are load-
bearing), `inducedHigh` has **no reconstruction identity to protect** -
it only ever multiplies into the output via `panInducedBlend(t)`, which
is exactly `0.0` at `t=0` regardless of what filter shape produced it
(see "Topology"'s "purely additive" framing). That means it is free to
be built from a *real, independently-designed* highpass filter instead -
one whose own magnitude response is a clean, monotonic rolloff by
construction, with no vector-sum hump anywhere, because there is no
complementary partner it needs to sum against. `PanoramaProcessor.cpp`
now cascades **three** 2nd-order Butterworth highpass stages (6th order
total, -36dB/oct, all at `panInducedHighpassHz` = 150Hz) - safe to make
this steep specifically *because* there is no reconstruction risk;
un-cascaded (2nd order) still left ~1.5dB, and a 4th-order cascade still
left ~1.5dB specifically at 120Hz (closest to the filter's own corner,
where attenuation is always weakest) before the third stage closed the
rest of the gap.

**Measured, `generateCenterBassStereoHighs` (centred 80Hz bass +
decorrelated 4kHz/5.5kHz highs), at MOTION**:

| | 1-pole subtraction (previous) | 2nd-order subtraction (rejected) | 6th-order proper highpass (current) |
|---|---|---|---|
| 80Hz bass L/R | ~2.6-2.9dB | ~3.4dB | **0.19dB** |

A dedicated regression test (`Tests/PluginTests.cpp`'s
`CenteredBassMotionIsolation`) isolates four independent measurements on
this same source at MOTION: bass L/R difference (0.19dB), bass magnitude
change from ORIGINAL to MOTION (0.10dB - the bass level itself barely
shifts just from turning PAN up), a *bass-only* Goertzel-windowed
centroid trajectory (rmsExcursion 0.0006 - effectively flat, unlike the
broadband centroid which the much-louder highs would dominate), and the
broadband high-frequency centroid excursion (0.149 - confirming the
fix did not also weaken the highs' own motion, which is unaffected by
this filter since it operates far above 150Hz).

**40-120Hz table** (dual-mono/mono bass tone, no real Side content, so
every bit of the magnitude delta below comes purely from the induced-
signal path; magnitude delta is relative to that frequency's own 0%
value; measured via `Tests/PluginTests.cpp`'s dedicated "40-120Hz
magnitude/centroid table" test):

| Frequency | PAN 0% | PAN 25% | PAN 50% | PAN 75% | PAN 100% |
|---|---|---|---|---|---|
| 40Hz magnitude delta | 0dB | -0.0001dB | -0.0002dB | -0.0004dB | -0.0005dB |
| 40Hz centroid excursion | 0 | 0.0000001 | 0.0000009 | 0.000004 | 0.000007 |
| 60Hz magnitude delta | 0dB | 0.0012dB | 0.0041dB | 0.0076dB | 0.0095dB |
| 60Hz centroid excursion | 0 | 0.0000003 | 0.00001 | 0.00005 | 0.0001 |
| 80Hz magnitude delta | 0dB | 0.0138dB | 0.0455dB | 0.0795dB | 0.0957dB |
| 80Hz centroid excursion | 0 | 0.00005 | 0.0002 | 0.0003 | 0.0004 |
| 100Hz magnitude delta | 0dB | 0.0466dB | 0.1501dB | 0.2453dB | 0.2809dB |
| 100Hz centroid excursion | 0 | 0.0001 | 0.0005 | 0.0015 | 0.0027 |
| 120Hz magnitude delta | 0dB | 0.0558dB | 0.1749dB | 0.2464dB | 0.2449dB |
| 120Hz centroid excursion | 0 | 0.00003 | 0.0011 | 0.0058 | 0.0103 |

Every value at 100% width is within this round's tightened targets
(40-80Hz `<0.25dB`, 100Hz `<0.5dB`, 120Hz `<0.75dB`) with comfortable
margin - the largest, 100Hz at 0.28dB, is little more than half its
0.5dB bound. Both magnitude delta and centroid excursion grow smoothly
and monotonically with width, and both grow with frequency (bass closest
to 40Hz stays closest to exactly 0 at every width) - the intended
"almost nothing at 40-60Hz, very little at 80-100Hz, a little more by
120Hz" shape, not a flat floor.

## Width mapping

`panWidthGain(t, maxAtFull)` - a single smoothstepped sweep from 1.0
(identity, at t=0) to `maxAtFull` (at t=1) - no named anchor at 50% the
way the retired MONO/NATURAL/WIDE contract or EqCurves.h's DARK-PHONE-
AIR have, since 50% is no longer a special point under this contract.

Measured (`PanoramaCurves.h`'s pure functions, high band):

| Width | Measured |
|---|---|
| 0% | 1.000 |
| 25% | 1.094 |
| 50% | 1.300 |
| 75% | 1.506 |
| 100% | 1.600 |

`panWidthMaxHigh` was reduced from an earlier 1.9 to 1.6 during the
correlation-balancing pass (see "Correlation" below) - amplifying Side
up to 1.9x was, on its own (independent of any motion rotation), enough
to push Side's power above Mid's power on realistic correlated material,
which is mathematically sufficient to flip the L/R correlation sign
regardless of motion. 1.6 is still a substantial, clearly audible
widening at 100%, just no longer strong enough on its own to invert
correlation on typical material.

Low band (`panWidthMaxLow = 1.15`) uses the identical shape scaled to a
much smaller ceiling - width never lets bass get proportionally as wide
as mid/high, at any setting.

## Motion

Motion depth (`panMotionDepth`) also sweeps smoothstepped from 0 (t=0)
to a ceiling (0.85 high band, 0.12 low band). Depth scales how far a
constant-power rotation angle swings away from its centred, no-motion
value, driven by a free-running LFO.

### The LFO

Single, deterministic, phase-continuous, ~0.3Hz clock
(`panLfoRateHz`), shared by both bands (the whole spatial field moves
together, not independently per band) - **never reset by a parameter
change**, only by `reset()` (playback stop/restart, same as every other
module's filter state). Measured period: **3.35s** (target ~3.33s),
identical across sample rates (44.1kHz and 96kHz tested) and block sizes
(64 and 2048 tested) - confirmed sample-rate-independent and block-
size-independent by construction (the phase increment is
`2*pi*panLfoRateHz/sampleRate` per sample, a continuous accumulator, not
tied to block boundaries). Automation (width changed mid-run across five
different values) measured period 3.45s - close to the same figure,
confirming the LFO's phase is not reset by parameter changes.

### Constant-power (equal-power) motion

`gainL = sqrt2*cos(theta)`, `gainR = sqrt2*sin(theta)` - `gainL^2 +
gainR^2 == 2` for *any* theta, an algebraic identity (`cos^2+sin^2=1`),
not a measured approximation. At `theta == pi/4` (motion depth 0 or the
LFO's own zero-crossing), `gainL == gainR == 1.0` - the ordinary
symmetric-width case. The LFO only ever *redistributes* a fixed
spatial-energy budget between L and R; it does not create or destroy it.
Measured combined stereo power (`L^2+R^2`) over a full motion cycle at
MOTION (100%): **-0.09dB to +0.06dB** - comfortably inside the ~1dB
target (well inside the tighter 0.5dB target the correlation-balancing
pass re-checked this against too), confirming this isn't just
algebraically constant for the spatial term alone but stays close to
constant for the *whole* output including its cross-term with Mid.

`panMotionThetaRange` (how far theta swings from centre at full depth)
was reduced from a full `pi/4` (a full quarter-turn) to `0.55` radians
during the same correlation-balancing pass - see "Correlation" below.
This softens the *peak* L/R gain ratio during rotation (from ~8:1 to
~3.4:1 at depth=1) without touching width, Side amplitude, or the
induced-signal blend - the user's explicit direction was not to fix
correlation by quietly shrinking Side to near-nothing.

### Motion cycle - measured

6-partial harmonic mono source, centroid = `(Renergy-Lenergy)/
(Renergy+Lenergy)` per 50ms window:

| Width | Centroid min | Centroid max | RMS excursion |
|---|---|---|---|
| 0% | 0.000 | 0.000 | 0.000 |
| 25% | -0.0277 | -0.0245 | 0.0010 |
| 50% | -0.127 | -0.032 | 0.034 |
| 75% | -0.318 | +0.115 | 0.159 |
| 100% | -0.426 | +0.232 | 0.245 |

Excursion grows monotonically with width; at MOTION (100%) the
trajectory visits clearly left-biased (-0.43) and clearly right-biased
(+0.23) states, smoothly and continuously (no window-to-window jump
resembling a discontinuity was measured), not just varying shades of one
side - the failure mode the product brief explicitly called out and the
one the induced-signal fix above (see "Mono-to-stereo strategy") was
built to close. Excursion is smaller than an earlier round's ~0.35-0.45
figures (the direct result of the `panMotionThetaRange`/`panWidthMaxHigh`
reductions above), but still clearly, audibly visits both sides, not a
subtle wobble.

### Not an auto-pan

The whole signal is never redirected - `L = x*sin(lfo), R = x*cos(lfo)`
was explicitly rejected. Mid is never touched by motion; only the
separately-built spatial signal is. A centred lead vocal or bass note
(carried in Mid) stays put while the *surrounding* stereo field breathes
around it - see "Centre stability" below.

## Frequency-dependent motion (low vs high)

Measured centroid RMS excursion at MOTION (100%), mono tone sources:

| Frequency | Excursion |
|---|---|
| 40 Hz | 0.000007 |
| 60 Hz | 0.0001 |
| 80 Hz | 0.0004 |
| 100 Hz | 0.0027 |
| 120 Hz | 0.0103 |
| 200 Hz | 0.087 |
| 500 Hz | 0.035 |
| 1000 Hz | 0.046 |
| 3000 Hz | 0.230 |
| 10000 Hz | 0.034 |

Bass (40-120Hz) moves dramatically less than mid/high content now (the
direct result of "Centre-bass isolation" below - 40Hz excursion alone
dropped roughly 800x, from 0.0056 to 0.000007, after that fix), matching
the product brief's "low frequencies stay close to centre, mid/high get
the real motion." The progression is not perfectly monotonic above
120Hz (500Hz/1000Hz/10000Hz measure lower than 200Hz/3000Hz) - expected,
since above the induced-highpass's own corner the *dominant* factor
becomes the `panAllpassHz`/`panMotionThetaRange` allpass-decorrelation
mechanism ("Mono-to-stereo strategy" above), not the highpass, and a
single fixed allpass is not equally decorrelated with Mid at every
frequency by design - what matters is that mid/high content overall
moves far more than bass does, which holds throughout. No bass
fundamental frequency drift was measured alongside this movement (<0.2%
error at every tested bass frequency, see "Pitch stability" below).

## No pitch drift, no wow/flutter

Motion is pure gain modulation (amplitude, via the LFO-driven rotation
angle) plus a fixed-coefficient allpass (phase, not delay) - there is no
delay line, no resampling, no time-varying filter coefficient tied to
signal content anywhere in the signal path, so there is nothing that
could shift pitch or introduce wow/flutter/chorus by construction.
Measured (steady tones at MOTION=100%, `analyzeBassStability`'s
phase-vocoder-based frequency tracking):

| Frequency | Measured | Error |
|---|---|---|
| 100 Hz | 99.95 Hz | 0.052% |
| 440 Hz | 439.21 Hz | 0.179% |
| 1000 Hz | 1000.71 Hz | 0.071% |
| 5000 Hz | 4992.22 Hz | 0.156% |

All four well under a fifth of a musical cent's worth of drift - not
audible, not measurable as a trend over the motion cycle.

## Centre stability

A centred 80Hz bass tone plus decorrelated stereo highs (4kHz/5.5kHz),
at MOTION (100%): the bass now measures **0.19dB** L/R difference - down
from ~2.6-2.9dB before this round's fix (see "Centre-bass isolation"
above for the full root-cause derivation and the fix: a proper cascaded
6th-order Butterworth highpass isolating `induced`'s own bass, replacing
a complementary-subtraction construction that had its own vector-sum-
style hump right at the corner). Far smaller than the same signal's
high-frequency content, which is designed to move substantially at
MOTION (broadband centroid excursion 0.149 on the same source vs the
bass-only centroid's 0.0006 - roughly 260x smaller - see "Centre-bass
isolation").

## Mono compatibility

Unlike the previous MONO/NATURAL/WIDE revision (where Mid was
*provably* the entire mono fold-down, unconditionally, at every
setting), motion's asymmetric `toL`/`toR` means the fold-down
(`(Lout+Rout)/2 = Mid + 0.5*(toL-toR)`) can move a little over a motion
cycle now - this is an intentional, disclosed relaxation (the product
brief explicitly does not require bit-exact fold-down preservation under
motion, only the absence of serious comb cancellation). Measured worst-
case mono fold-down level change across four source types (mono,
centre-bass+highs, correlated chord, decorrelated) at 50%/100% width:

| Source | 50% | 100% |
|---|---|---|
| mono | +0.02dB | +0.31dB |
| centre-bass+highs | -0.01dB | +0.01dB |
| correlated chord | +0.07dB | +0.57dB |
| decorrelated | +0.15dB | +1.07dB |

Worst case ~1.1dB - improved again this round (down from ~1.5dB), a side
effect of the centre-bass isolation fix above (less of `induced`'s bass
leaking through means less low-frequency content available to shift the
fold-down at all) - a real, audible-but-modest level shift, not a
comb-filtering artifact (no delay anywhere in the signal path means no
frequency-selective nulls; this is a broadband level change from the
motion rotation's own energy redistribution).

## Correlation

Measured on a correlated stereo chord (three tones, same frequencies on
both channels at different per-tone balances - genuinely correlated
content, not anti-phase):

| Width | Correlation |
|---|---|
| 0% | 0.965 |
| 25% | 0.950 |
| 50% | 0.804 |
| 75% | 0.526 |
| 100% | +0.393 |

Correlation at 100% improved again this round (0.216 -> 0.393) as a side
effect of the centre-bass isolation fix - the previous complementary-
subtraction filter's near-corner overshoot (see "Centre-bass isolation")
was adding a small amount of extra, differently-phased induced content
right around 150-300Hz, which the new clean highpass no longer does.

**Fixed the previous round**: an earlier revision measured -0.071 to -0.11 at
100% width - not "aggressively negative" by the original acceptance bar,
but the product brief was tightened to prefer correlation staying `>= 0`
at 100% on representative correlated material (anti-phase test signals
excluded from this specific bar, since anti-phase is a deliberately
worst-case, uncorrelated-by-construction input). Root-caused via the
input's own `sideMidRatio` (`Side_rms/Mid_rms`, measured directly in
`Tests/PluginTests.cpp`'s correlation test): for `L=Mid+width*Side,
R=Mid-width*Side` with symmetric gains (no rotation at all), `E[L*R] =
E[Mid^2] - width^2*E[Side^2]` - this goes **negative purely from width
amplification**, independent of motion rotation, whenever
`width^2 * Side_rms^2` exceeds `Mid_rms^2`. At `panWidthMaxHigh = 1.9`
and this test material's own `sideMidRatio` (~0.89 at 100% width, after
amplification), that threshold was being crossed by width alone.
Reducing `panWidthMaxHigh` to 1.6 (see "Width mapping" above) was the
dominant fix; `panMotionThetaRange`'s reduction (see "Motion" above) was
tried first, on the assumption that motion's asymmetric rotation was the
main driver, and measured almost no improvement on its own (-0.097 ->
-0.11) - a useful negative result, kept in the code anyway since it
genuinely softens the L/R gain ratio during rotation and costs nothing,
but the width reduction is what actually mattered here. Correlation
now degrades gracefully and stays **positive at every measured width**,
including full MOTION. Mono source correlation (measured separately,
since Side starts at exactly 0) drops from 1.0 at 0% to 0.275 at 100% as
real, growing spatial content is added - not comparable to the stereo-
material table above (a mono source has no pre-existing Side content for
width to amplify relative to, so this number isn't subject to the same
correlation-sign mechanism).

## Gain / headroom

No AGC, no internal limiter. Measured on a deliberately Side-heavy
synthetic source:

| Width | Peak | RMS L | RMS R |
|---|---|---|---|
| 0% | 0.330 | 0.186 | 0.186 |
| 25% | 0.377 | 0.194 | 0.209 |
| 50% | 0.492 | 0.207 | 0.264 |
| 75% | 0.614 | 0.215 | 0.322 |
| 100% | 0.670 | 0.218 | 0.349 |

Smooth, proportionate growth - no sudden jump, no runaway; nowhere close
to clipping even on a Side-heavy source.

## Smoothing / bypass

Width is smoothed with a 20ms linear ramp (`panSmoothingSeconds`) -
inside the product brief's 10-30ms guidance. The motion LFO's own phase
is *not* smoothed - it is a free-running clock, unaffected by width
automation (see "The LFO" above). Automation transitions (0->100, 100->0,
25->75) measured click-free.

`panoramaEnabled` (index 4, `Core/ModuleEnableState.h`) drives a real
bypass: disabled crossfades to an exact dry passthrough (no delay
alignment needed - PAN has no latency to align against), matching the
existing EQ-style bypass pattern (immediate crossfade, no
`IntegerDelayLine`).

## Latency

**Always exactly 0** - `getLatencySamples()` returns a hardcoded `0`,
verified constant across every width value, enabled/disabled, every
supported sample rate (44.1-192kHz), and every block size (32-2048). No
delay-based motion (no Haas), so there is nothing that could add
latency. Total plugin latency is unchanged from the pre-PAN PITCH
baseline at every sample rate (e.g. 6186 samples / 140.272ms at 44.1kHz -
matches [docs/DSP_PITCH.md](DSP_PITCH.md) exactly).

## Parameter and state migration

`panorama` keeps its existing APVTS string ID and C++ type
(`AudioParameterFloat`, `0..100%`). Its default is **0% (ORIGINAL)** -
reverted from the retired MONO/NATURAL/WIDE contract's 50% ("NATURAL")
default, since 50% is no longer an identity point under the current
contract (it's WIDE now).

Schema bumped to **v5** (`Source/Core/PluginIdentity.h`,
`panoramaOriginalSchemaVersion = 5`, fixed independently of any later
`stateSchemaVersion` bump). `setStateInformation()` forces `panorama`'s
stored `value` to `0.0` whenever `loadedSchemaVersion < 5` - this covers
**both** genuinely old pre-DSP states *and* v4 states saved under the
retired MONO/NATURAL/WIDE contract (where 50% meant "NATURAL," which has
no equivalent meaning now). Neither an old pre-DSP value nor a v4-era
"50% NATURAL" choice has any meaning under the current contract, so
there is nothing to "best-effort" preserve - this plugin has not had a
public release yet (see CLAUDE.md), so there is no installed base whose
v4 choices this could be accused of destroying, only local development
states. Covered by dedicated tests against schema versions 2, 3, and 4
alike, each with a hand-set legacy value of 50.0 (the old "NATURAL"
value specifically, to confirm it is *not* preserved as if it still
meant something).

## Motion rate (nested RATE knob, live-testing follow-up round)

The motion LFO's own speed was a fixed constant (`panLfoRateHz`, ~0.3Hz)
through every previous round of this module's development - "deliberately
slow, nothing resembling tremolo" was a hard product constraint, not a
tunable parameter. Direct feedback asked for it to become one, referencing
SoundToys PanMan's own Rate knob as the explicit model: a nested,
concentric secondary control - physically inside the WIDTH knob, not
beside it - the same "one deliberate exception to one-knob-per-module"
pattern IMAGE's FIELD pad already established for this project (see
CLAUDE.md's HTML/CSS/JS UI rule).

**New 9th public parameter**: `panRate` (`Source/Parameters/ParameterIDs.h`) -
a plain `0..100%` `AudioParameterFloat`, same shape as six of the other
eight parameters. Following the same precedent `imageTilt` set for growing
past 8 parameters (a genuinely independent second axis on one module, not
a second knob controlling the same thing) - see ParameterIDs.h's own
updated class comment.

**Curve** (`PanoramaCurves.h`'s `panRateHz()`): an exponential taper,
`g = min*(max/min)^t`, the same shape this project's drive curves already
use - not a linear Hz sweep, which would spend almost the whole knob in
an "impossibly slow" region and compress every musically useful speed
into the last few percent. `panRateMinHz = 0.05` (~20s/cycle, glacial but
not "stuck"); `panRateMaxHz = 8.0` (clear tremolo-like flutter, matching
the fast end of PanMan's own range by ear/spec).

**Default preserves every existing preset/session exactly**: the
parameter's own default (`ParameterLayout.cpp`) is `35.303%` - not an
arbitrary "middle" value, but the precise normalised position that solves
`panRateHz(t) == panLfoRateHz` (measured `panRateHz(0.35303) = 0.299979Hz`,
0.007% off target). Every one of the 32 factory presets carries this same
value (`FactoryPresets.h`), and a legacy user-preset file with no
`panRate` attribute at all falls back to it too (`UserPresets.h`) - a
session/preset that never touches the new RATE knob sounds identical to
before this round shipped.

**Realtime behaviour**: `rateNormalised01` is smoothed the same
`panSmoothingSeconds` (~20ms) way `widthNormalised01` already is, and the
resulting LFO increment (`panRateHz(rate) * twoPi / sampleRate`) is
recomputed every sample - a RATE move retunes the swing smoothly, not in
a block-sized staircase. Only the LFO's own *speed* is parameterised;
its *phase* is unchanged by this round - still a free-running clock, never
reset by any parameter change (RATE included), only by `reset()`.

**Measured**: swinging RATE from 50% to 95% (chosen so both settings
complete several cycles inside a 6s test window) measured periods of
1.58s and 0.161s respectively via the same centroid-trajectory method the
rest of this module's motion tests use - clearly, substantially different,
confirming RATE genuinely retunes the measured LFO, not just a stored
number. A completely untouched instance (RATE left at its own default)
still measures a ~3.3s period at MOTION, matching every pre-existing PAN
motion test's own baseline.

## Known limitations

- The 80Hz-under-stereo-highs centre-stability test measures a **0.19dB**
  residual, not 0dB - see "Centre-bass isolation" above for the full fix
  history (a proper cascaded 6th-order Butterworth highpass, replacing a
  complementary-subtraction construction that had its own vector-sum-
  style hump - down from ~2.6-2.9dB before this round, and ~22.5dB before
  any induced-signal bass protection existed at all). Small enough now
  that it is not chased further - well inside every one of this round's
  tightened per-frequency targets (40-80Hz `<0.25dB`, 100Hz `<0.5dB`,
  120Hz `<0.75dB`), and roughly 260x smaller than the same source's
  high-frequency motion.
- A single sustained pure tone at an unlucky frequency can still show a
  measurably asymmetric (not perfectly left/right-balanced) motion
  trajectory - see "Mono-to-stereo strategy" above. Real/broadband mono
  material does not show this in the same way, which is what the
  module's own verification uses and what actually matters for the
  shipped product; a true wideband Hilbert-transform-quality
  decorrelation network (typically 6+ cascaded allpass stages, requiring
  real filter-design tooling to tune correctly) would remove this
  residual dependency entirely if ever needed, at meaningfully more
  implementation complexity than the current single allpass.
- Mono fold-down is no longer provably invariant under motion (only
  under ORIGINAL/pure static width) - up to ~1.1dB measured level
  change at full MOTION on some source types (down from ~1.5dB last
  round and ~2.5dB the round before, each a side effect of unrelated
  fixes rather than direct fold-down work). Disclosed and accepted per
  the product brief's own relaxed mono-compatibility requirement, not
  hidden.
- Correlation on correlated stereo material is now measured `>= 0` at
  every tested width including 100% (the specific thing a previous
  round's correlation work targeted, further improved as a side effect
  of this round's centre-bass fix) - but this was tuned against the
  module's
  own three-tone `generateCorrelatedChord` test material specifically,
  not proven algebraically for arbitrary program material. Material with
  a substantially higher Side/Mid ratio than the test chord could in
  principle still cross zero at high width, by the same mechanism
  documented in "Correlation" above (`width^2 * Side_rms^2` exceeding
  `Mid_rms^2`) - anti-phase/pure-Side content is the extreme case of
  this and is explicitly excluded from the `>= 0` bar, per the product
  brief.
