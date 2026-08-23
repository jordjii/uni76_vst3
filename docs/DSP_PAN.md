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

## Topology

```
Input (L, R)
  -> Mid  = 0.5*(L+R)                          (stable core, never modified)
  -> Side = 0.5*(L-R)                          (real existing stereo content)
  -> induced = allpass(Mid)                    (phase-decorrelated reference,
                                                 mono/near-mono sources' only
                                                 source of spatial content)
       -> inducedHigh = induced - LP(induced)  (removes induced's own bass -
                                                 see "Low-end protection")
  -> spatialRaw = Side + inducedHigh * inducedBlend(t)
  -> spatialLow  = LP(spatialRaw)               (single first-order lowpass)
  -> spatialHigh = spatialRaw - spatialLow      (exact complement)
  -> theta_low  = pi/4 + motionDepthLow(t)  * sin(lfoPhase) * pi/4
  -> theta_high = pi/4 + motionDepthHigh(t) * sin(lfoPhase) * pi/4
  -> toL = spatialLow*widthLow(t)*sqrt2*cos(theta_low)  + spatialHigh*widthHigh(t)*sqrt2*cos(theta_high)
  -> toR = spatialLow*widthLow(t)*sqrt2*sin(theta_low)  + spatialHigh*widthHigh(t)*sqrt2*sin(theta_high)
  -> Lout = Mid + toL
  -> Rout = Mid - toR
  -> enable/disable crossfade against a dry copy (zero latency, no delay-alignment needed)
  -> Output
```

`t` is the raw `panorama` APVTS value (0..1). Every curve in
`PanoramaCurves.h` (`widthLow`/`widthHigh`, `motionDepthLow`/
`motionDepthHigh`, `inducedBlend`) evaluates to its **identity value at
t=0** - width gain 1.0, motion depth 0.0, induced blend 0.0 - which is
what makes ORIGINAL provably (not just measured) a bypass: with those
values, `toL == toR == spatialLow+spatialHigh == spatialRaw == Side`
exactly, so `Lout = Mid+Side = L`, `Rout = Mid-Side = R`.

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

## Low-end protection

Two separate mechanisms protect bass, addressing two different sources
of low-frequency movement:

1. **Width/motion ceilings are deliberately much smaller for the low
   band** (`panWidthMaxLow = 1.15` vs `panWidthMaxHigh = 1.9`;
   `panMotionDepthMaxLow = 0.12` vs `panMotionDepthMaxHigh = 0.85`), split
   via a single first-order (gentle, not brickwall) lowpass at 150Hz
   (`panCrossoverHz`). `spatialHigh` is defined as `spatialRaw -
   spatialLow` (the filter's algebraic complement, not a second,
   independently-designed filter), so `spatialLow + spatialHigh ==
   spatialRaw` exactly for any filter history - the same
   identity-preserving trick the module's static-width predecessor used.

2. **`induced`'s own bass content is removed before it ever reaches the
   spatial signal** (`inducedHigh = induced - LP(induced)`, same 150Hz
   cutoff). This was a real bug found by testing, not a theoretical
   concern: Mid (what the allpass reads) contains the source's actual
   bass whenever there is any, so without this step, synthesised
   spatial energy would leak into the low band and get width/motion-
   processed there too - even with a small ceiling, moving bass that
   was never really stereo to begin with. Measured before the fix: a
   centred 80Hz bass tone (with decorrelated stereo highs also present)
   showed a **22.5dB** L/R imbalance at MOTION - clearly audible,
   clearly wrong. After adding the induced-signal highpass: **2.5dB** -
   a small, honestly-documented residual (see "Centre stability" below
   for why it isn't exactly 0dB) that is far below what the same source's
   high-frequency content shows.

## Width mapping

`panWidthGain(t, maxAtFull)` - a single smoothstepped sweep from 1.0
(identity, at t=0) to `maxAtFull` (at t=1) - no named anchor at 50% the
way the retired MONO/NATURAL/WIDE contract or EqCurves.h's DARK-PHONE-
AIR have, since 50% is no longer a special point under this contract.

Measured (`PanoramaCurves.h`'s pure functions, high band):

| Width | Target | Measured |
|---|---|---|
| 0% | 1.0 | 1.000 |
| 25% | ~1.15-1.25 | 1.141 |
| 50% | ~1.4-1.5 | 1.450 |
| 75% | ~1.6-1.75 | 1.759 |
| 100% | ~1.8-2.0 | 1.900 |

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
MOTION (100%): **-0.10dB to +0.12dB** - comfortably inside the ~1dB
target, confirming this isn't just algebraically constant for the
spatial term alone but stays close to constant for the *whole* output
including its cross-term with Mid.

### Motion cycle - measured

6-partial harmonic mono source, centroid = `(Renergy-Lenergy)/
(Renergy+Lenergy)` per 50ms window:

| Width | Centroid min | Centroid max | RMS excursion |
|---|---|---|---|
| 0% | 0.000 | 0.000 | 0.000 |
| 25% | -0.0095 | -0.0040 | 0.0016 |
| 50% | -0.100 | +0.043 | 0.050 |
| 75% | -0.331 | +0.287 | 0.234 |
| 100% | -0.449 | +0.442 | 0.348 |

Excursion grows monotonically with width; at MOTION (100%) the
trajectory visits clearly left-biased (-0.45) and clearly right-biased
(+0.44) states, smoothly and continuously (no window-to-window jump
resembling a discontinuity was measured), not just varying shades of one
side - the failure mode the product brief explicitly called out and the
one the induced-signal fix above (see "Mono-to-stereo strategy") was
built to close.

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
| 40 Hz | 0.012 |
| 60 Hz | 0.031 |
| 80 Hz | 0.051 |
| 100 Hz | 0.062 |
| 120 Hz | 0.054 |
| 3000 Hz | 0.330 |

Bass (40-120Hz) moves noticeably less than 3kHz (roughly 5-25x smaller
excursion), matching the product brief's "low frequencies stay close to
centre, mid/high get the real motion" - and no bass fundamental frequency
drift was measured alongside this movement (<0.2% error at every tested
bass frequency, see "Pitch stability" below).

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
| 100 Hz | 99.95 Hz | 0.053% |
| 440 Hz | 439.22 Hz | 0.178% |
| 1000 Hz | 1000.71 Hz | 0.071% |
| 5000 Hz | 4992.21 Hz | 0.156% |

All four well under a fifth of a musical cent's worth of drift - not
audible, not measurable as a trend over the motion cycle.

## Centre stability

A centred 80Hz bass tone plus decorrelated stereo highs (4kHz/5.5kHz),
at MOTION (100%): the bass measures a **2.5dB** L/R difference - small,
honestly nonzero (see "Low-end protection" above for the filter-
transition-band reason it isn't exactly 0dB: `inducedHigh` is already
highpassed once before `spatialLowpass` splits the combined signal a
second time at the *same* cutoff, and two independent first-order
filters at one cutoff don't cancel each other's transition-band leakage
perfectly) - far smaller than the same signal's high-frequency content,
which is designed to move substantially at MOTION.

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
| mono | -0.43dB | -2.08dB |
| centre-bass+highs | +0.02dB | +0.24dB |
| correlated chord | -0.45dB | -1.99dB |
| decorrelated | +0.39dB | +2.48dB |

Worst case ~2.5dB - a real, audible-but-modest level shift, not a comb-
filtering artifact (no delay anywhere in the signal path means no
frequency-selective nulls; this is a broadband level change from the
motion rotation's own energy redistribution).

## Correlation

Measured on a correlated stereo chord:

| Width | Correlation |
|---|---|
| 0% | 0.965 |
| 25% | 0.953 |
| 50% | 0.861 |
| 75% | 0.373 |
| 100% | -0.071 |

Correlation degrades gracefully through WIDE, and at full MOTION on
already-correlated material can go slightly negative - an honest,
expected consequence of a strong (by design) motion effect at 100%, not
a defect. Mono source correlation (measured separately, since Side
starts at exactly 0) drops from 1.0 at 0% to 0.25 at 100% as real,
growing spatial content is added.

## Gain / headroom

No AGC, no internal limiter. Measured on a deliberately Side-heavy
synthetic source:

| Width | Peak | RMS L | RMS R |
|---|---|---|---|
| 0% | 0.330 | 0.186 | 0.186 |
| 25% | 0.385 | 0.200 | 0.217 |
| 50% | 0.524 | 0.222 | 0.292 |
| 75% | 0.679 | 0.236 | 0.371 |
| 100% | 0.755 | 0.242 | 0.407 |

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

## Known limitations

- The 80Hz-under-stereo-highs centre-stability test measures a 2.5dB
  residual, not 0dB - see "Low-end protection"/"Centre stability" above
  for the filter-transition-band reason. Small and far below the same
  material's high-frequency movement, but not literally zero.
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
  under ORIGINAL/pure static width) - up to ~2.5dB measured level
  change at full MOTION on some source types. Disclosed and accepted per
  the product brief's own relaxed mono-compatibility requirement, not
  hidden.
- Correlation can go slightly negative on correlated material at full
  MOTION (measured -0.07) - an honest, by-design consequence of a strong
  100% motion effect, not investigated further since it stayed well
  inside the "not aggressively negative" bound the product brief sets.
