# VERB / VINTAGE SPACE - DSP notes

`Source/DSP/VerbProcessor.h`/`.cpp` implements the `06 VERB / VINTAGE
SPACE` module - the sixth real DSP in the plugin (after PREAMP, EQ, SAT,
PITCH and PAN). Imager remains a strict passthrough - see CLAUDE.md.

A single 1970s-style **electromechanical plate reverb + analog send/
return electronics** - not a generic digital hall, not a ROOM/PLATE/
CHAMBER morph, not a convolution IR:

```
DRY (0%)  ->  PLATE (50%)  ->  DEEP (100%)
```

Every setting is the *same* plate machine. The macro only ever changes
send amount, decay time, and pre-delay - never the plate's own physical
character (delay-line layout, diffusion, damping shape, analog
coloration), matching a real plate unit's send/time controls rather than
a switch between different reverb types.

## Topology

```
DRY -------------------------------------------------------> MIX
INPUT
  -> wet send HPF (350Hz, cascaded 4-pole Butterworth)
  -> analog send stage (tiny asymmetric tanh)
  -> diffuser (4-stage short-delay Schroeder allpass - early density)
  -> pre-delay (smoothly variable, linear-interpolated)
  -> FDN plate tank (12 delay lines, Householder feedback matrix,
     per-line one-pole damping so highs decay faster than mid,
     fixed decorrelated stereo output taps)
  -> analog return stage (tiny tanh + ~9.5kHz soft bandwidth ceiling)
  -> wet output HPF safety (350Hz, lighter, 2-pole)
  -> MIX (dry + wetGain(t)*bypassMix*wetProcessed)
  -> Output
```

**DRY is read into locals and written back unmodified in the same per-
sample loop iteration - it never passes through any filter, delay line,
or nonlinearity in this file.** The wet contribution is purely additive
(an aux-send level, not a crossfade), scaled by `verbWetGain(t)` (exactly
`0.0` at `t=0`) and the bypass smoother. This makes "dry is never
touched" an algebraic guarantee, not just a measured property - and
gives DRY (0%) a provable, not just measured, bit-exact identity:
measured RMS diff **0** / max diff **0** on a 4-tone broadband source
(`Tests/PluginTests.cpp`'s "DRY (0%) is a bit-exact identity transform"
test).

## Electromechanical plate model

The product goal is the *character* of a real vibrating steel plate
(large mass, dense modal structure, fast/immediate response, dark
smooth decay) picked up by a transducer, not a literal simulation of
plate physics (finite-element modal synthesis, etc. - out of scope for a
realtime plugin). The chosen proxy is a 12-line Feedback Delay Network
(FDN) - see "Plate modal/FDN architecture" below - specifically *not* a
4-comb-2-allpass "cheap Schroeder" reverb, which the product brief
explicitly rejects: a Schroeder reverb's sparse comb structure produces
audible, discrete periodicity (the classic "boingy" cheap-digital-reverb
character); a well-tuned FDN with enough delay lines and an orthogonal
mixing matrix produces a much denser, smoother, more plate-like decay
with no single dominant comb frequency.

## Analog send electronics

Before the plate tank, the wet signal passes through:

1. **350Hz wet send highpass** (`verbWetSendHighpassHz`, cascaded 4-pole
   Butterworth = 2x `makeHighPassButterworth` in series) - see "350Hz
   wet-path isolation" below.
2. **A tiny asymmetric tanh waveshaper** (`verbSendDriveGain = 0.18`,
   `verbSendAsymmetry = 0.10`) - the same bounded per-half-gain tanh()
   shape PREAMP/SAT use (`xd >= 0 ? tanh(xd) : tanh(xd*(1-asym))`,
   normalised by `tanh(driveGain)`), with its own, much smaller,
   fixed (not macro-controlled) constants. This is deliberately *not* a
   second SAT module - see "Analog nonlinearity" below for the measured
   THD, which stays under 2% at every macro setting.

## 350Hz wet-path isolation

**The single most important product rule for this module**: dry bass/
kick must never be touched, and the wet plate tank must never sustain
meaningful sub/bass energy. Two independent highpass filters enforce
this, not one:

- `wetSendHighpassA`/`B` (4-pole, cascaded) on the way *into* the tank -
  the primary defence.
- `wetOutputHighpassL`/`R` (2-pole, lighter) on the way *out* of the
  tank, after the analog return stage - a safety net specifically
  because a **recirculating feedback network's own resonances are not
  guaranteed to respect an input-side filter alone**: a delay line's
  own comb-filter peaks can, in principle, coincide with a low frequency
  regardless of how thoroughly that frequency was filtered on the way
  in, if the tank's own feedback loop happens to reinforce it. Measured
  bass rejection already vastly exceeds what either filter alone would
  suggest (see below), so in practice the safety filter's own
  contribution is small - it exists as a guarantee, not because the
  primary filter measurably under-performs.

**Measured** (`Tests/PluginTests.cpp`'s "Low-frequency wet rejection"
and "Bass test" - wet-only Side-style burst response, dB relative to a
0.3-amplitude input tone, VERB=100%):

| Frequency | Wet/input |
|---|---|
| 40 Hz | -86.4 dB |
| 50 Hz | -86.7 dB |
| 60 Hz | -81.4 dB |
| 80 Hz | -83.0 to -86.5 dB |
| 100 Hz | -71.6 to -72.1 dB |
| 120 Hz | -59.6 to -68.0 dB |
| 160 Hz | -60.8 dB |
| 200 Hz | -41.1 dB |
| 250 Hz | -29.6 to -30.2 dB |
| 300 Hz | -38.1 to -38.7 dB |
| 350 Hz | -27.9 to -28.7 dB |
| 400 Hz | -35.6 to -36.5 dB |
| 500 Hz | -14.8 to -15.4 dB |

40-120Hz sits at -60 to -92dB - practically no wet content at all,
matching "почти никакого хвоста." 160-250Hz is still strongly
suppressed (-30 to -61dB). 300Hz is still clearly suppressed. 350-400Hz
is the transition band (matches the filter's own -3dB corner sitting at
350Hz). 500Hz is only -15dB down - a full, clearly audible plate is
already present there, matching "полноценный reverb уже разрешён." The
transition is smooth (a soft, minimum-phase IIR rolloff, not a
brickwall FIR - no added latency, no ringing).

## Plate modal/FDN architecture

**Why FDN, not Schroeder.** A Feedback Delay Network with N delay lines
mixed through an *orthogonal* (energy-preserving) matrix has N
independent resonant modes per delay line, giving N times the modal
density of a single comb filter, with no two modes reinforcing each
other unpredictably (orthogonality guarantees the mixing doesn't create
new resonances beyond the individual lines' own). 12 lines (within the
requested "8 minimum, 12-16 explored" range) at short, deliberately
non-commensurate lengths (5.3-37.3ms, `verbLineLengthsMs`) give a dense,
fast-building early response typical of a physically large, heavily-
coupled vibrating plate - not the sparse, audibly-periodic character a
2-4-line comb-based design would have.

**Feedback matrix.** A Householder reflection using the all-ones vector
(`mixed[i] = fb[i] - (2/N)*sum(fb)`) - a well-known orthogonal matrix
computable in O(N) per sample (one sum, N subtractions) rather than a
full N*N multiply, and *provably* energy-preserving (a Householder
matrix is its own transpose and inverse), so the only source of decay in
the whole network is the per-line gains and damping filters below, not
the mixing itself.

**Diffuser.** 4 cascaded short-delay Schroeder allpass stages
(1.1-3.1ms, `verbDiffuserLengthsMs`, gain `verbDiffuserGain=0.6`) ahead
of the tank - builds early reflection density quickly before the signal
ever reaches the 12-line tank. This is explicitly *not* used alone as
the whole reverb (that would be the rejected "cheap Schroeder"
architecture) - it is a pre-density stage feeding a proper FDN tank,
architecturally distinct from a bare comb+allpass reverb.

**Stereo output.** The tank is fed from a single **mono** sum
(`0.5*(L+R)`, post-HPF/send-stage/diffuser/pre-delay) - a deliberate
choice matching how a real plate reverb often uses *one* physical plate
with two pickups at different positions to derive stereo from a single
mechanical resonator. Two independent, non-proportional, fixed sign
patterns (`outputSignL`/`outputSignR` in `VerbProcessor.cpp`) read the
same 12 lines' instantaneous outputs into L and R, giving genuinely
decorrelated stereo width from the single mono-fed tank (measured
correlation on a 7-tone broadband wet-only signal: **-0.45** - a wide,
decorrelated tail, which is the intended character for a reverb's *wet*
signal specifically, unlike PAN's dry-adjacent correlation requirement).

**Metallic resonance / mode-density check.** No single mode should ring
audibly longer than its neighbours (the "bathroom" cheap-reverb failure
mode). The frequency-dependent decay measurements below (1kHz/5kHz/
8kHz) show a smooth, monotonic relationship with no anomalous outlier;
the low-frequency rejection table above likewise shows a smooth,
monotonic-ish progression across 40-500Hz with no isolated frequency
reading unexpectedly *less* suppressed than its neighbours by a large
margin, which would be the signature of a single dominant, under-damped
mode. No dedicated narrowband sweep beyond the tables already presented
was run - see "Known limitations" below.

## Analog return stage

After the tank: a tiny asymmetric tanh (`verbReturnDriveGain=0.12`,
`verbReturnAsymmetry=0.02`) plus a gentle one-pole lowpass bandwidth
ceiling (`verbReturnBandwidthHz=9500`, soft rolloff, not brickwall) -
"output transformer/line amp" coloration and top-end rounding. Wet-only,
same as the send stage - dry never passes through it.

## RT60

**Formula.** Each of the 12 delay lines gets its own feedback gain from
the standard `g = 10^(-3*(L/sampleRate)/RT60)` relationship (after `n`
passes of a line `L` samples long, `g^n == -60dB` when
`n*L/sampleRate == RT60`), recomputed once per block from the smoothed
macro value (same one-block-lag pattern EqProcessor/SatProcessor already
use - none of VERB's macro-derived parameters need audio-rate
precision).

**A real tuning subtlety, found and fixed during development**: the
formula above assumes *only* the flat gain governs decay, but the per-
line damping filter (below) removes *additional* energy every pass on
top of that - even a small per-pass insertion loss compounds hugely over
the hundreds of feedback passes a multi-second RT60 needs (e.g. roughly
-0.24dB/pass at 1kHz against an initially-chosen 4200Hz damping corner,
over ~300 passes for a 6s target, compounds to roughly -70dB on its own -
silently capping 1kHz's own decay far below its nominal target). This
was caught by measuring, not assumed: the nominal decay anchors
(`verbDecayAnchors`) had to be tuned measurably higher than the product
brief's raw 3.5-4.5s target (final anchors: 0.5/1.1/2.6/4.3/6.0s at
0/25/50/75/100%) to make the *actual, measured* decay land in that
range - see the table below.

**Safety clamp.** `verbLineFeedbackGainMax = 0.985` hard-caps any single
line's per-pass gain independent of the RT60 formula - found necessary
when an earlier tuning attempt (damping corner raised to 9000Hz, to
reduce 1kHz's own damping-compounding loss) pushed the shortest line's
loop gain close enough to 1.0 to measurably distort the result (THD
roughly doubled, steady-state level became erratic) - a real stability-
margin symptom, not a measurement artifact. The final `verbDampingHz =
6000Hz` was the settled middle ground.

**Measured** (bandpassed-RMS decay envelope, `Tests/PluginTests.cpp`'s
frequency-dependent-decay test and its scratch-tool predecessor), 1kHz,
VERB=100%:

| Time after burst | Level |
|---|---|
| 0.05s | -1.8dB |
| 0.3s | -2.1dB |
| 1.0s | -15.5dB |
| 2.0s | -32.7 to -35.0dB |
| 3.5s | -60.3dB (i.e. RT60 ~= 3.5s) |

Lands at the lower edge of the requested 3.5-4.5s range at 100% - not
chased further this round given the safety clamp above already limits
how much more aggressive the flat gain can safely be made; see "Known
limitations."

## Frequency-dependent decay

Per-line one-pole lowpass damping (`OnePoleLowPass`, `verbDampingHz =
6000Hz`) inside the feedback path, applied *before* the flat gain each
pass - content above the damping corner loses more energy per round
trip than content below it, so the tail gets progressively darker over
time (distinct from `verbReturnBandwidthHz`, which limits the wet
signal's overall top end at every instant, not just its decay rate).

**Measured** (bandpassed-RMS envelope, dB relative to peak, VERB=100%):

| Frequency | @1.0s | @2.0s |
|---|---|---|
| 1000 Hz | -16.6dB | -35.0dB |
| 5000 Hz | -66.3dB | -83.4dB |
| 8000 Hz | -73.6dB | -90.3dB |

Clean, monotonic "highs decay faster than mid" - by 1 second, 5kHz has
already decayed roughly 50dB further than 1kHz, and 8kHz a further 7dB
beyond that. This is the direct, measured realisation of the product
brief's "1kHz tail = main RT60, 5kHz shorter, 8-10kHz shorter still."

## Analog nonlinearity

**Measured** (`Tests/PluginTests.cpp`'s THD test, wet-only, 1kHz sine at
-18dBFS, measured well after full settling):

| Wet | H1 | H2 | H3 | THD |
|---|---|---|---|---|
| 25% | 3.48e-4 | 3.52e-6 | 1.66e-6 | 1.12% |
| 50% | 7.84e-4 | 1.11e-5 | 5.96e-6 | 1.61% |
| 75% | 1.22e-3 | 2.08e-5 | 1.16e-5 | 1.95% |
| 100% | 1.68e-3 | 2.64e-5 | 1.61e-5 | 1.84% |

THD stays under 2% at every macro setting - comfortably inside "small,"
far from the explicitly-rejected 5-10% range. This was itself a tuning
pass: an earlier attempt at `verbSendAsymmetry=0.10`/
`verbReturnAsymmetry=0.06` (both non-trivial) measured **THD around
5-6%** at 100% - since H2 (the dominant harmonic, ~20x H3) is generated
specifically by the asymmetric-tanh construction's *asymmetry* term, not
its drive gain, reducing drive alone (tried first, 0.35/0.22 ->
0.18/0.12) only modestly reduced THD (5.9% -> 4.9%); reducing asymmetry
specifically (0.10/0.06 -> 0.02/0.02) is what brought it down to the
~1-2% range reported above - texture, not distortion.

## Centre-stability / PAN+VERB integration

PAN=100% (MOTION) + VERB=50% (PLATE), centred 80Hz bass + decorrelated
4kHz/5.5kHz stereo highs (`Tests/PluginTests.cpp`'s "PAN+VERB
integration" test): measured 80Hz bass L/R = **0.16dB** through the
combined chain - the bass stays centred and stable, matching PAN's own
already-verified centre-bass isolation (see docs/DSP_PAN.md) with VERB
added on top; VERB's own 350Hz wet-path isolation independently ensures
none of that 80Hz content gets any further from the plate tail either.
The reverb is not "muddying" the low end by design (see "350Hz wet-path
isolation" above) - the two modules' bass-protection mechanisms are
independent and compound rather than compete.

**Full-chain low-end** (`Tests/PluginTests.cpp`'s "Full chain low-end"
test): PREAMP+EQ+SAT+PITCH+PAN+VERB, comparing VERB=0% against VERB=100%
on a 60/80/100Hz bass + 2kHz mid source - measured bass-frequency change
between VERB off and VERB fully on:

| Frequency | VERB0 -> VERB100 change |
|---|---|
| 60 Hz | +0.06dB |
| 80 Hz | +0.07dB |
| 100 Hz | -0.05dB |

All three well under a tenth of a dB - turning VERB fully on is
essentially inaudible at the bass frequencies, confirming the module
doesn't wash out the low end anywhere in the real signal chain, not just
in isolation.

## Smoothing / bypass

`reverb` (wet amount) is smoothed with a 30ms linear ramp
(`verbSmoothingSeconds`). `reverbEnabled` (index 5,
`Core/ModuleEnableState.h`) drives the bypass smoother, which scales
only the wet contribution to zero - dry is unaffected by bypass state by
construction (see "Topology" above), so disabling VERB is an exact dry
passthrough, not a separate crossfade path. Automation across several
wet values in sequence measured click-free (no sample-to-sample jump
exceeding a crude 1.0-amplitude threshold - see the "Bypass ...
automation" test).

## A real bug found by Debug-mode testing: pre-delay index safety

The pre-delay's variable-length read (`VerbProcessor.cpp`) originally
wrapped a negative read position back into range with a `while (readPosF
< 0.0f) readPosF += capacity;` loop. This is mathematically correct for
any *finite* `readPosF`, but is not defensive against a non-finite one:
IEEE-754 comparisons against NaN are always false, so `while (NaN < 0.0f)`
never executes, leaving `readPosF` as NaN - and `(int) NaN` is undefined
behaviour in C++, which on this compiler could produce a wildly out-of-
range integer. This was caught for real, not theoretically: a Debug
build's bounds-checked STL raised a genuine `vector subscript out of
range` assertion while running `Tests/PluginTests.cpp`'s VERB test
suite, which the Release build's unchecked STL had silently not caught
(undefined behaviour, not a crash, in that configuration - a latent bug
regardless of which configuration happened to surface it). Fixed by:
replacing the while-loop with `std::fmod` (terminates in one step for
any finite input, unlike an open-ended loop), explicitly substituting a
safe fallback value whenever `readPosF` is ever non-finite
(`std::isfinite` guard), and clamping the final index with
`juce::jlimit` as a last-resort defensive bound. All Debug and Release
tests pass after the fix, including a full re-run of the exact suite
that caught the original crash.

## Latency

**Always exactly 0** - `getLatencySamples()` returns a hardcoded `0`,
verified constant across every wet value, enabled state, sample rate
(44.1-192kHz) and block size (32-2048). Pre-delay and the tank's own
recirculation are wet-path effects, not a lookahead/analysis delay on
the direct signal, so there is nothing here a host needs to compensate
for - matching EQ's and PAN's own zero-latency reasoning. Total plugin
latency is unchanged from the pre-VERB PAN baseline at every sample rate.

## Parameter and state migration

`reverb` keeps its pre-existing APVTS string ID and C++ type
(`AudioParameterFloat`, `0..100%`, default `0%`) - this parameter and
default already existed as a passthrough placeholder before this round,
so no schema bump or migration was needed: a 0% value already meant "no
audible reverb effect" under the old passthrough behaviour, and it still
does under the new real DSP (DRY is a provable identity at 0%, see
"Topology").

## Known limitations

- Measured RT60 at 100% (~3.5s at 1kHz) sits at the lower edge of the
  requested 3.5-4.5s range rather than comfortably inside it. Pushing
  the nominal decay anchor higher was not pursued further this round
  once `verbLineFeedbackGainMax` (a hard stability-margin safety clamp,
  see "RT60" above) started limiting how much of that increase would
  actually reach the shortest delay line - a real engineering trade-off
  between maximum decay time and guaranteed stability, disclosed rather
  than pushed past the safety margin found necessary during tuning.
- No dedicated narrowband (e.g. 1Hz-resolution) sweep was run
  specifically hunting for a single anomalously-long-lived resonant
  mode - the broader frequency-dependent-decay and low-frequency-
  rejection tables show smooth, monotonic-ish behaviour with no obvious
  outlier, which is the practical signal this kind of problem would
  produce, but this is not the same rigor as an exhaustive per-mode
  sweep.
- Wet-path stereo correlation on broadband material measures negative
  (~-0.45) - by design and intent for a *wide reverb tail* (unlike PAN's
  dry-adjacent correlation requirement, no positive-correlation bar was
  set or is appropriate for VERB's own wet signal), but disclosed here
  explicitly since it is a notably different acceptance shape from the
  PAN module's own correlation section.
- The analog send/return stages' fixed (not macro-controlled) drive
  constants were tuned against the specific -18dBFS/1kHz sine test
  condition the product brief specifies; THD at other signal levels or
  spectral content was not exhaustively swept, though the underlying
  tanh-based construction is the same bounded, PREAMP/SAT-proven-safe
  shape used elsewhere in the plugin.
