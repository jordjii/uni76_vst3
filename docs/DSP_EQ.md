# EQ / PHONE TONE - DSP design and measurements

## Redesign (UX polish pass - live-testing feedback) - supersedes the
## DARK/PHONE/AIR topology below

The five-stage DARK/PHONE/AIR morph documented in the rest of this file
(Topology through Known compromises) was **replaced** in a later pass
with a simpler, steeper, always-on two-cut "telephone band" filter,
modelled directly on a real reference plugin's cut-band settings
(FabFilter Pro-Q3): a highpass and a lowpass, each 48dB/octave (4
cascaded 2nd-order RBJ stages per cut - see `EqProcessor.cpp`'s
`hpStages`/`lpStages`), whose corner frequencies sweep together as the
macro moves, always keeping the same ratio between them (~15.7-15.9x,
~3.97-3.99 octaves of passband), while each cut's own Q stays fixed
(0.765 HP / 0.676 LP) at all three anchors:

```
t=0.0   (displayed -50%, knob fully left)   -> HP  72Hz / LP  1132Hz
t=0.5   (displayed   0%, knob centred)       -> HP 461Hz / LP  7288Hz
t=1.0   (displayed +50%, knob fully right)   -> HP 748Hz / LP 11855Hz
```

The `eq` APVTS parameter itself is **unchanged** - still `0..100%`,
default `50%` (see `ParameterLayout.cpp` and CLAUDE.md's "8 immutable
public parameters") - this was a DSP-behaviour and UI-label change only
(knob label renamed TONE -> PHONE TONE, displayed value remapped to
`-50%..+50%` in the frontend, see `app.js`'s `formatEqPercent`), not a
parameter-contract change, so it needed no schema bump. Every setting is
now a band-pass "phone" character by construction (there is no fully-open
DARK/AIR extreme any more) - turning the knob moves *where* that
telephone band sits in frequency, not whether one exists at all.

New source of truth: `Source/DSP/EqCurves.h`'s `EqCutParams`/
`eqAnchorLeft`/`eqAnchorCenter`/`eqAnchorRight`/`eqParamsAt()`, and
`Biquad.h`'s new `makeHighPassQ`/`makeLowPassQ` (same RBJ shape as the
existing Butterworth-fixed helpers, but with an explicit caller-chosen Q
instead of the fixed `1/sqrt(2)`). No oversampling, no nonlinearity, zero
added latency (unchanged from the original design) - only the filter
topology and the knob's displayed range changed. **Measured
frequency-response verification of the new anchors was not performed as
part of this pass** - the exact Hz/Q/slope values were supplied directly
and implemented as specified; a dedicated frequency-response regression
test (matching the rigor of every other module's measured-data sections
below) is a natural follow-up, not yet done.

---

The rest of this document (below) describes the **original**
DARK/PHONE/AIR five-stage topology and its own measured data, kept as
historical record rather than deleted - see CLAUDE.md's own convention
for superseded designs (e.g. docs/DSP_PAN.md's MONO/NATURAL/WIDE
history). None of it describes the plugin's current EQ behaviour.

---

The second real DSP module in UNI 76, after PREAMP. Everything here lives
in [`Source/DSP/EqProcessor.h`](../Source/DSP/EqProcessor.h) /
[`.cpp`](../Source/DSP/EqProcessor.cpp), driven by the curves in
[`Source/DSP/EqCurves.h`](../Source/DSP/EqCurves.h) and the same
allocation-free filter toolkit PREAMP uses
([`Source/DSP/Biquad.h`](../Source/DSP/Biquad.h)). Saturation, Pitch,
Panorama, Reverb and Imager remain a strict passthrough - see
[`CLAUDE.md`](../CLAUDE.md). PREAMP's own DSP/constants were **not**
touched by this pass.

Chain order: `Input -> PREAMP -> EQ -> (future modules) -> Output` -
`PluginProcessor::processBlock()` calls `preampProcessor.process()` then
`eqProcessor.process()`, in that order, matching the approved concept.

EQ is **not** a `frequency = lerp(min, max, t)` control and not a 3-preset
switch. It's one macro parameter (`eq`, 0..100%, default 50%) that morphs a
fixed five-stage filter network continuously across two linked regions
sharing a named centre:

```
t in [0.0, 0.5]  ->  DARK  ---> PHONE
t in [0.5, 1.0]  ->  PHONE ---> AIR
```

## Topology

```
Input -> HP -> Low Shelf -> Bell/Presence -> High Shelf -> LP -> Output Trim -> Output
```

All five stages are **always present** - the macro parameter only ever
morphs each stage's own frequency/gain/Q continuously; there is never a
topology switch. A stage reads as "inert" at some anchor simply because
its gain there is 0dB (shelf/bell) or its cutoff is far outside the
audible band (HP/LP) - not because it's bypassed or swapped out. This is
exactly why the crossing through PHONE (t=0.5) has no coefficient jump:
every stage's parameters are continuous functions of `t` by construction.

- **HP / LP**: 2nd-order Butterworth (Q=1/sqrt(2), maximally flat, no
  resonant peak, ~12dB/octave) - the same shape PREAMP's Low Cut/High Cut
  use, reused here for consistency (`Biquad.h`'s
  `makeHighPassButterworth`/`makeLowPassButterworth`).
- **Low Shelf / High Shelf**: RBJ shelf filters, broad slope (`S=1`), no
  resonance.
- **Bell (presence)**: RBJ peaking EQ, deliberately low/moderate Q
  (0.7-0.9) at every anchor - broad and musical, never a surgical notch.

No nonlinearity or saturation anywhere in this module - PREAMP (and the
future SAT module) own harmonic character; EQ only shapes frequency/phase
response, exactly as scoped.

## Morph curves

[`EqCurves.h`](../Source/DSP/EqCurves.h) defines three named anchors
(`eqAnchorDark`/`eqAnchorPhone`/`eqAnchorAir`), each a full set of five
stages' parameters, and `eqParamsAt(t)` morphs between the appropriate
pair for whichever half of the range `t` falls in:

- **Frequencies interpolate geometrically** (log-linear) - an even sweep
  to the ear, not a linear Hz ramp.
- **Gains and Q interpolate linearly** - standard practice.
- **Each region's local blend is smoothstepped** (`3t^2-2t^3`), not raw
  linear - this makes the morph's *rate of change*, not just its value,
  reach zero at every anchor. That's what makes the crossing through
  PHONE genuinely seamless (matching slope from both sides), not just
  continuous-but-kinked.

Filter coefficients are recomputed once per block from the *previous*
block's settled (smoothed) `t` value - the same one-block-lag pattern
PREAMP uses, inaudible at any reasonable block size, avoiding per-sample
trig cost.

## DARK (t=0)

Full bass retained; top gently rounded via a broad LP (not a shelf cut);
a touch of low-mid warmth. Explicitly **not** an underwater effect - the
LP sits at 5.5kHz, not 1-2kHz.

| Stage | Value |
|---|---|
| HP | 28 Hz |
| Low shelf | 220 Hz, +1.6 dB |
| Bell | inert (0 dB) |
| High shelf | inert (0 dB) |
| LP | 5500 Hz |

## PHONE (t=0.5)

A real, clearly audible vintage voice-band character - HP/LP define the
band; a broad, low-Q bell adds a touch of concentrated presence so the mid
stays readable rather than just sounding like "less bass and treble."

| Stage | Value |
|---|---|
| HP | 300 Hz |
| Low shelf | inert (0 dB) - HP already defines the low end |
| Bell | 1500 Hz, +2.2 dB, Q=0.85 |
| High shelf | inert (0 dB) - LP already defines the top end |
| LP | 3400 Hz |
| Output trim | +1.2 dB (see "Loudness / gain staging") |

## AIR (t=1)

Bass stays open (gentle HP only); broad high shelf for "air"; a small
presence lift; no big treble spike - not a brittle 10kHz boost.

| Stage | Value |
|---|---|
| HP | 70 Hz |
| Low shelf | inert (0 dB) |
| Bell | 4000 Hz, +1.2 dB, Q=0.7 (gentle presence) |
| High shelf | 8500 Hz, +4.0 dB |
| LP | 20000 Hz (clamped to a safe fraction of Nyquist - see "Sample rates") |
| Output trim | -0.8 dB (see "Loudness / gain staging") |

## Loudness / gain staging

No AGC/envelope-follower anywhere - `PHONE`'s and `AIR`'s anchors each
carry one small **static** output trim (`outputTrimDb` in `EqCurves.h`),
tuned from a broadband multi-tone measurement (7 tones, 80Hz-12kHz),
morphed the same continuous way as every other stage parameter:

| EQ | RMS vs 0% (broadband multi-tone) |
|---|---|
| 0% (DARK) | 0 dB (reference) |
| 25% | -0.26 dB |
| 50% (PHONE) | -1.16 dB |
| 75% | +0.05 dB |
| 100% (AIR) | +0.80 dB |

Total spread across the whole range is under 2dB - comfortably inside the
brief's "a few dB" target. PHONE measured *quieter* than DARK even before
any trim was added (the band-limiting outweighs the presence bell's
concentration), so the trim there is a small positive nudge (+1.2dB), not
the negative correction the brief anticipated might be needed - the actual
measurement is what decided the sign, not the assumption.

## Smoothing

`juce::SmoothedValue` linear ramp, **35ms**, applied to the `t` (EQ/TONE)
parameter itself before it drives coefficient recomputation - filter
coefficients only ever move as fast as this already-smoothed value can,
which is what keeps automation (including sweeping directly through 50%)
click-free. Verified with a 49%->50%->51% transition, a 0->100->0 sweep,
and rapid jumps - see "Tests" below.

## Bypass

`eqEnabled` (`Core/ModuleEnableState.h`, index 1): **20ms** linear
crossfade between the fully processed ("wet") signal and a dry copy - not
an instant `if` switch. Unlike PREAMP's bypass, **no delay-alignment is
needed**: EQ adds zero algorithmic latency, so the dry and wet paths are
already sample-for-sample time-aligned before the crossfade.

## Latency

**Zero additional samples, always** - a fixed IIR filter network with no
oversampling and no linear-phase FIR has no algorithmic latency to report.
`EqProcessor::getLatencySamples()` always returns `0`, verified at every
EQ/enabled combination and every supported sample rate
(`Tests/PluginTests.cpp`: "EQ adds zero latency at every setting"). The
plugin's total declared latency is unchanged from the PREAMP-only figure
(still 6/6/4/0 samples across 44.1/48/96/192kHz - see
[docs/DSP_PREAMP.md](DSP_PREAMP.md)).

## Sample rates

All filter design functions take the real sample rate in Hz and compute
`w0 = 2*pi*f/fs` directly - there is no hardcoded normalised digital
frequency anywhere. Every stage's target frequency is additionally clamped
to `0.9 * Nyquist` before use, so a nominal target that would sit
uncomfortably close to (or above) Nyquist at a low sample rate - AIR's
~20kHz LP at 44.1kHz, for instance - stays stable and meaningful instead
of producing degenerate/unstable coefficients. Verified at 44.1/48/88.2/
96/176.4/192kHz: PHONE's HP/LP still suppress 100Hz and 10kHz well below
1kHz at every one of those rates (`Tests/PluginTests.cpp`: "PHONE mapping
sounds semantically the same across sample rates").

## Measured frequency response

`Tests/PluginTests.cpp`'s `UNI76EqFrequencyResponseTests` - gain in dB
relative to a fed sine, measured via Goertzel through the real
`EqProcessor`, at 44.1kHz. Numbers below are copied directly from an
actual `UNI76Tests.exe` run's stdout:

| Frequency | EQ 0% (DARK) | EQ 25% | EQ 50% (PHONE) | EQ 75% | EQ 100% (AIR) |
|---|---|---|---|---|---|
| 30 Hz    | -0.85 dB  | -18.05 dB | -38.80 dB | -27.17 dB | -15.66 dB |
| 60 Hz    | +1.39 dB  | -6.69 dB  | -26.76 dB | -15.24 dB | -5.35 dB |
| 100 Hz   | +1.51 dB  | -0.94 dB  | -17.93 dB | -7.13 dB  | -1.73 dB |
| 300 Hz   | +0.36 dB  | +0.83 dB  | -1.69 dB  | +0.01 dB  | -0.80 dB |
| 1000 Hz  | 0.00 dB   | +1.56 dB  | +2.59 dB  | +0.68 dB  | -0.65 dB |
| 3400 Hz  | -0.52 dB  | -0.55 dB  | -1.19 dB  | +1.62 dB  | +0.40 dB |
| 5000 Hz  | -2.19 dB  | -3.90 dB  | -6.42 dB  | +1.12 dB  | +0.59 dB |
| 10000 Hz | -13.03 dB | -16.81 dB | -20.52 dB | -3.64 dB  | +2.29 dB |
| 16000 Hz | -29.13 dB | -33.07 dB | -36.85 dB | -18.66 dB | +2.85 dB |
| 20000 Hz | -49.44 dB | -53.38 dB | -57.17 dB | -38.96 dB | -1.25 dB |

(300Hz and 3400Hz sit almost exactly on PHONE's own HP/LP cutoffs, so the
~-1.2 to -1.7dB reading there - not a full -3dB - reflects the bell's
partial offset, not a measurement error; 20kHz's dip back to -1.25dB at
AIR is the LP stage's own roll-off reasserting right at the edge of
hearing, discussed under "Known compromises" below.)

## PHONE acceptance (measured, not just intended coefficients)

All from the table above / `Tests/PluginTests.cpp`:

- **Low end below voice band is suppressed**: 100Hz measures -17.93dB
  relative to input, and -19.6dB (test: "PHONE should suppress 100Hz well
  below 1kHz") relative to PHONE's own 1kHz reading.
- **High end above voice band is suppressed**: 10kHz measures -20.52dB
  relative to input, -23.1dB relative to PHONE's own 1kHz reading.
- **Mid stays readable**: 1kHz measures +2.59dB - boosted, not buried -
  from the presence bell, while never exceeding a few dB (test asserts
  the mid stays within -3 to +6dB, comfortably inside a "boosted but not
  blaring" range).
- **Transitions are soft**: HP/LP are 2nd-order Butterworth (no resonant
  peak by construction, ~12dB/octave); the bell's Q (0.85) is broad, not
  surgical.
- **No high resonant peaks anywhere**: verified structurally (Butterworth
  HP/LP have Q=1/sqrt(2) by construction, and the bell's Q never exceeds
  ~0.9 at any anchor) and empirically (no measurement in the response
  table shows a sharp, narrow spike - PHONE's own 1kHz bell reading,
  +2.59dB, is the largest boost anywhere in the table, and it's broad).

## Phase / stability

Minimum-phase by construction (every stage is a standard RBJ biquad or
2nd-order Butterworth) - linear phase was explicitly out of scope. What
*was* checked:

- No unstable poles at any anchor or sample rate: every filter design
  function's frequency argument is clamped to `0.9 * Nyquist` before
  computing `w0`, keeping every biquad's pole radius safely inside the
  unit circle at every supported sample rate (44.1kHz through 192kHz).
- No coefficient-explosion or huge resonant overshoot during automation:
  the 49%->50%->51% transition test and the 0->100->0 sweep test both
  assert a bounded maximum sample-to-sample jump (< 0.3 for a 0.5-
  amplitude sine) - if a coefficient recompute produced a pathological
  transient, either test would catch it.
- NaN/Inf input never permanently poisons filter state: a dedicated test
  feeds NaN/Inf samples into `EqProcessor::process()`, confirms they never
  reach the output, and confirms a clean block processed immediately
  afterward is also clean (state wasn't left corrupted).

## Numerical safety

Input samples are sanitised (non-finite -> 0) at the start of `process()`,
before anything else touches them, and the same check runs again on the
final output - the same narrow-safeguard pattern PREAMP uses, not a
blanket amplitude clamp. `juce::ScopedNoDenormals` (already present in
`PluginProcessor::processBlock`) covers denormal protection for the whole
chain, EQ included.

## Stereo

Verified: an identical mono signal fed to both L and R produces
numerically identical L/R output after EQ (each channel has fully
independent filter state, but no randomness or cross-channel coupling
exists anywhere in the chain) - no stereo coloration.

## PREAMP + EQ integration

`Tests/PluginTests.cpp`'s `"PREAMP + EQ combinations..."` test runs all 6
combinations of PREAMP {0%, 50%} x EQ {0%, 50%, 100%} through the real
`UNI76AudioProcessor::processBlock()` with an 800Hz test tone: no NaN/Inf,
no gain explosion (peak stays well under 4x), and both level meters keep
reporting. PREAMP's own DSP/constants/gain-staging were not touched by
this pass.

## Tests

`Tests/PluginTests.cpp` adds `UNI76EqProcessorTests` (direct
`EqProcessor` unit tests: bypass, DARK/PHONE/AIR response shape, PHONE's
relative suppression, DARK-vs-PHONE bass retention, AIR's high-frequency
gain and bound, the 49/50/51% and 0/100/0 automation transitions, bypass-
transition cleanliness, silence, mono, stereo/no-drift, all 6 sample
rates, PHONE's cross-sample-rate consistency, 5 block sizes, NaN/Inf
safety + state recovery, and zero-latency-always), `UNI76EqIntegrationTests`
(real-processor bypass, the 50% default, the 6 PREAMP+EQ combinations,
state save/restore at 0/50/100%), and `UNI76EqFrequencyResponseTests`
(the measurement table above). All existing PREAMP tests remain
unmodified and passing (see [docs/DSP_PREAMP.md](DSP_PREAMP.md)) except
one integration test that now explicitly disables EQ too, so it continues
to isolate PREAMP's own bypass behaviour rather than also measuring EQ's
now-real PHONE default coloration.

## Known compromises

- AIR's LP target (~20kHz) sits close enough to Nyquist at 44.1kHz that
  after the `0.9 * Nyquist` safety clamp, the filter's own roll-off
  reasserts right at the very top of the measured range (20kHz reads
  -1.25dB instead of continuing the +2 to +3dB shelf trend from 16kHz) -
  a real tradeoff of needing headroom for a stable 2nd-order filter that
  close to Nyquist, not a bug. The audible "air" band (up to ~16kHz, where
  most listeners' sensitivity already drops off) is unaffected.
- PHONE's output trim (+1.2dB) and AIR's (-0.8dB) are creative constants
  tuned against the broadband measurement above, not derived from a
  closed-form loudness model - consistent with the brief's explicit "no
  AGC" instruction.
- The bell/shelf stages use a fixed shelf slope (`S=1`) and fixed Q at
  each anchor rather than their own independent morph curves for
  slope/Q - simpler, and the measured response table shows this is
  already broad/musical at every position without needing that extra
  degree of freedom.
