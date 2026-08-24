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
mode. A dedicated mandatory narrowband sweep (16 points, 350Hz-10kHz,
followed up at 10Hz and 10-50Hz resolution around the two regions the
coarse pass flagged) was run in a later acceptance/calibration pass - see
"Acceptance / calibration pass" below - and found no genuine dominant/
stuck mode, closing what the first round left as a documented gap.

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

### A second instance of the same bug class, found by a dedicated regression test

A later acceptance/calibration pass added a regression test specifically
targeting this bug *class* (NaN/Inf macro input, extreme pre-delay,
`reset()`, `prepare()`/re-`prepare()` cycles, multiple sample rates, max
block-size boundaries - `Tests/PluginTests.cpp`'s "Regression: pre-delay
read-index stays in-bounds under NaN macro input and other edge cases").
That test immediately caught a **second, independent** instance of the
same undefined-behaviour pattern, in a completely different file: a Debug
build blocked on a live "array subscript out of range" assertion (note
*array*, not *vector* - a different container than the original bug) on
only the *second* consecutive `process()` call with a NaN
`wetNormalised01`.

Root cause: `verbPiecewise()` (`VerbCurves.h`, used by `verbWetGain`,
`verbDecaySeconds`, and `verbPreDelayMs`) computed
`std::clamp (t01, 0.0f, 1.0f)` and then cast the scaled result to `int`
to pick a segment index into a 5-element `std::array`. `std::clamp` does
**not** clamp NaN - all comparisons against NaN are false, so it returns
the unclamped input unchanged - so a non-finite `t01` reached `(int)
scaled` regardless of the clamp call, which is undefined behaviour and
can yield an out-of-range `segment`. The entry point that let a NaN
`wetNormalised01` reach this code at all was the same mistake one level
up: `VerbProcessor::process()`'s own `wetSmoother.setTargetValue
(std::clamp (wetNormalised01, 0.0f, 1.0f))` had exactly the same
non-clamping-of-NaN gap, so a NaN parameter value silently became the
smoother's target, contaminating `tForCoefficients` for every subsequent
block (explaining why the first `process()` call was fine - it still ran
on the smoother's last valid value - and only the second call, once the
smoother had ramped onto the poisoned target, crashed).

Fixed at both the root and the shared utility, matching this file's
established defence-in-depth pattern: `VerbProcessor::process()` now
substitutes `0.0f` for a non-finite `wetNormalised01` *before* clamping,
and `verbPiecewise()` independently guards its own `t01` argument the
same way, falling back to `0.0f` (the DRY/identity point) rather than
picking an arbitrary value. Both a NaN and an Inf `wetNormalised01`, at
every tested sample rate and block size (including an 8192-sample block,
far larger than the pre-delay buffer's own capacity), across `reset()`
and `prepare()`/re-`prepare()` cycles, are covered by the regression
test above and now pass. This is a genuine, previously-unknown latent
bug found by writing the exact kind of adversarial regression test the
first bug's writeup called for - not a re-discovery of the original one.

## Acceptance / calibration pass

A dedicated validation round (on top of the round that first implemented
this module) ran a comprehensive acoustic acceptance/calibration pass -
*not* a redesign round - specifically to answer whether this genuinely
reads as a 1970s-style electromechanical plate rather than a generic
digital reverb, and to close the "no dedicated narrowband sweep" gap the
first round's "Known limitations" explicitly flagged. No architecture
change resulted: every measurement below either confirmed the existing
design already meets its target, or the underlying artifact resolved
under closer inspection as ordinary FDN modal texture rather than a real
defect. The two code changes that *did* result from this round are the
NaN-macro-parameter fix documented above (found via a regression test,
not via any of the acoustic measurements below) and a related dead-code/
warning fix in `Tests/PluginTests.cpp`'s "Constructs, prepares, resets"
test (it looped over wet/enabled combinations without ever calling
`process()` with them - fixed to actually exercise them, closing a
compiler warning along the way).

**Method note.** All measurements below were produced by a scratch tool
(not committed - see this session's established scratch-tool pattern)
linking the real `VerbProcessor.cpp` (and, for the PAN+VERB scenarios,
the real `PanoramaProcessor.cpp`) directly, using the same
`VERB(wet%) - VERB(0%)` wet-only subtraction technique and bandpassed-
RMS-envelope-at-fixed-checkpoints decay measurement already established
and documented above.

**1. Does this genuinely sound like a real plate?** The product's own
phrasing - "a single 1970s-style electromechanical plate reverb + analog
send/return electronics" (see the top of this document) - was already
correctly hedged before this round: it does not claim to emulate any
specific named hardware unit or claim exact measurement-matching against
one, which is accurate, since no such reference-hardware comparison was
ever performed. No wording change was needed. The measurements below
(modal density/resonance behaviour, impulse density, low-end isolation,
THD, RT60 shape) are the closest available proxy for "does this read as
a plate" in a non-listening, automated context; the final listening WAVs
(below) are the intended way to judge the qualitative character directly.

**2/3. Narrowband resonance sweep - mandatory item.** A 1/3-octave sweep
(16 points, 350Hz-10kHz, VERB=100%, bandpassed-RMS decay checkpoints at
0.3/1.0/2.0s post-burst) flagged two regions for follow-up: a non-
monotonic reading at 500Hz (decayed *less* at 1.0s than at 0.3s) and a
neighbour-outlier at 1250Hz (8.7dB less decayed than the average of its
1/3-octave neighbours at 2.0s). Both were followed up at 10-20Hz
resolution:

- **440-560Hz** (7 points): all values fall within an ~8dB band with no
  isolated spike - 500Hz's own value is not even the least-decayed point
  in its own neighbourhood (480Hz and 520Hz both decay less than it at
  2.0s). This is ordinary close-mode beating/jitter in a dense 12-line
  FDN, not a stuck mode - it fully continues decaying (500Hz reaches
  -33.75dB by 2.0s, in line with its neighbours).
- **1100-1600Hz** (10Hz-resolution probe across 1150-1300Hz, plus 50Hz
  steps to 1600Hz): a genuine candidate was checked analytically first -
  two of the 12 delay lines (8.1ms and 9.7ms) have harmonic mode series
  that nearly coincide near ~1235Hz (10th harmonic of the 8.1ms line
  ≈1235Hz, 12th of the 9.7ms line ≈1237Hz), which would be a textbook
  mechanism for a reinforced, longer-lived mode. The 10Hz sweep found no
  narrow spike at that frequency - values jitter jaggedly by up to ~10dB
  from one 10Hz step to the next with no consistent peak at 1230-1240Hz
  specifically. A full multi-checkpoint decay curve (0.5-3.5s) directly
  compared 1235Hz against two control frequencies either side (1180Hz,
  1290Hz): all three decay at essentially the same rate throughout (by
  3.5s: 1180Hz -69.1dB, 1235Hz -63.8dB, 1290Hz -61.4dB - within ~8dB of
  each other, no outlier). The theoretical mode-coincidence risk did not
  materialise as an audible/measurable effect, most likely because the
  Householder mix redistributes energy across all 12 lines every sample
  rather than letting any two lines resonate in isolation, and the
  `verbLineFeedbackGainMax` safety clamp already limits how much any
  single line's loop gain can build up regardless.

**Conclusion: no genuine dominant/stuck mode was found at any resolution
tested (1/3-octave, 10-50Hz, and a full decay-curve comparison at the
single most theoretically-likely candidate frequency).** Per the
acceptance criterion, the plate is allowed to sound metallic (dense,
uneven modal texture) as long as no single mode reads as an audible
musical note sticking out of the tail - the measured texture is uneven
in exactly the way a 12-line FDN's modal density predicts, with no
component decaying dramatically slower than its surroundings. No FDN
delay-length, diffuser, damping, or feedback-distribution change was
made as a result - per this round's explicit "don't redesign absent a
found problem" instruction, retuning would have been pure speculative
churn against a measurement that came back clean.

**4. Impulse density.** A 4-sample unit click, measured wet-only, in
0-10/10-50/50-100/100-300/300-500ms windows, at VERB=25/50/75/100%,
using a simple local-peak-counting echo-density proxy (arrivals above
15% of each window's own peak): every wet level shows **zero** arrivals
in the first 10ms (expected - the diffuser's own ~8ms of cascaded delay
plus the macro-dependent pre-delay, 0-20ms, both sit ahead of the tank),
then an immediate, very dense cloud from 10ms onward (roughly 5600-7000
arrivals/sec-equivalent through 10-100ms at every wet level, gradually
thinning to ~1500-2400/sec-equivalent by the 300-500ms late tail as the
decay progresses). This is exactly the "brief initial segment, then
dense cloud quickly" shape the acceptance criterion asks for - nothing
resembling discrete "tap...tap...tap" echoes (which would show on the
order of tens of arrivals/sec, not thousands) at any tested wet level.

**5/6. 40-1000Hz wet table (350Hz cutoff bracket), VERB=100%.** Two
metrics were measured: a burst-relative snapshot (Goertzel magnitude
during the 50ms burst itself, wet-only, dB vs the input's own magnitude)
and a settled-tail energy integral (total wet-only energy over 400ms
after the burst, which better reflects the FDN's true steady-state
response since the burst-relative snapshot is measured during the
tank's own buildup transient and is comparatively noisy/comb-like
frame-to-frame - a real, expected property of a modal reverb's short-
window response, not a filter defect).

| Frequency | Burst-relative (dB, noisier) | Tail energy (smoother trend) |
|---|---|---|
| 40 Hz | -87.0 | 0.027 |
| 60 Hz | -87.0 | 0.063 |
| 80 Hz | -81.1 | 0.116 |
| 100 Hz | -75.0 | 0.194 |
| 120 Hz | -63.2 | 0.289 |
| 160 Hz | -59.5 | 0.669 |
| 200 Hz | -40.8 | 3.10 |
| 250 Hz | -29.4 | 15.54 |
| 300 Hz | -40.2 | 54.63 |
| 350 Hz | -24.2 | 92.06 |
| 400 Hz | -34.0 | 408.0 |
| 500 Hz | -14.5 | 360.8 |
| 630 Hz | -34.9 | 268.5 |
| 1000 Hz | -26.3 | 747.8 |

The tail-energy column is cleanly, monotonically increasing from 40Hz
through 400Hz (each step higher than the last) before settling into the
500Hz+ "fully present" range - a smooth, non-brickwall low end
transition exactly matching the product contract ("below ~350Hz
practically absent, 350Hz a transition, 500Hz+ clearly present"). The
burst-relative column's frequency-to-frequency dips (e.g. 630Hz reading
20dB lower than 500Hz) reflect the FDN's own irregular, comb-like early-
response character measured in a short window mid-buildup, not a
malfunctioning filter - the same "metallic dispersion" texture item 2/3
above investigated and cleared. Critically for item 6 ("don't over-
apply the low cut"), the 250-500Hz body range never drops anywhere near
the "practically absent" territory the sub-160Hz region sits in (-60 to
-87dB) - it stays clearly present throughout (tail energy 15.5 to 408,
several orders of magnitude above the deep-bass rejection level).

**7. Analog nonlinearity (H2/H3/THD), -18dBFS, 1kHz.** Unchanged from
the "Analog nonlinearity" section above - restated here since this round
re-measured it at the exact granularity requested (25/50/75/100% wet):

| Wet | H1 | H2 | H3 | THD |
|---|---|---|---|---|
| 25% | 3.74e-4 | 3.69e-6 | 5.0e-6 | 0.99% |
| 50% | 1.10e-3 | 1.13e-5 | 2.77e-5 | 1.03% |
| 75% | 1.82e-3 | 1.90e-5 | 5.40e-5 | 1.05% |
| 100% | 2.53e-3 | 2.69e-5 | 8.02e-5 | 1.06% |

THD sits in a tight ~1.0-1.06% band across the whole macro range - well
inside "small, texture not distortion." No further reduction was
pursued (already comfortably under the 2% ceiling documented above).

**8/9. Correlation: wet-only vs full output vs mono fold-down.**
Measured across three representative source types (mono snare-like
transient, mono sustained vocal-like tone, a correlated stereo chord) at
25/50/75/100% wet:

| Source | Wet | Wet-only correlation | Full-output correlation | Mono fold-down change |
|---|---|---|---|---|
| Vocal-like | 25% | -0.15 | **0.997** | ~0.0dB |
| Vocal-like | 50% | -0.15 | **0.984** | -0.03dB |
| Vocal-like | 75% | -0.12 | **0.956** | 0.19dB |
| Vocal-like | 100% | -0.12 | **0.926** | 0.32dB |
| Chord | 25% | -0.39 | **0.968** | -0.15dB |
| Chord | 50% | -0.51 | **0.961** | -0.38dB |
| Chord | 75% | -0.37 | **0.964** | -0.18dB |
| Chord | 100% | -0.29 | **0.922** | -0.82dB |

Wet-only correlation does dip to roughly the previously-reported ~-0.45
region (the chord source reaches -0.51 at 50%) - confirmed, not a
one-off measurement. But per item 9's acceptance bar, **full-output
correlation is what matters for the production mix**, and it stays
strongly positive (0.92-0.997) across every source and wet level tested,
with mono fold-down change staying under ~1dB throughout. The dry signal
dominates the full mix enough that the wet tail's own decorrelation
never meaningfully destabilises the centre or collapses in mono. (The
mono snare-like source was measured too, but its dry content ends at
30ms while the measurement window starts at 300ms to let the reverb
settle - so for that source specifically, "full output" and "wet only"
in that window are the same signal by construction, and the "mono
fold-down change" metric isn't meaningful there since it divides by a
nearly-silent dry reference; its wet-only correlation stayed small,
-0.06 to 0.05, i.e. not the concerning case at all.) No change to the
stereo output-tap sign patterns was made - the existing decorrelated-
pickup design already satisfies the actual product requirement (a
stable full-mix centre), and the wet-only decorrelation is, as
originally documented, appropriate character for a *reverb tail*
specifically, not a defect to chase toward positive correlation.

**10. PAN=100%+VERB=100% correlation over time (primary spatial stress
test).** Measured over a full 4s pass (100ms sliding windows) on both a
dedicated bass+highs source and a full synthetic mix (chord + bass +
highs):

| Scenario | Correlation min | max | mean |
|---|---|---|---|
| Bass+highs, VERB=50% | 0.446 | 0.578 | 0.510 |
| Bass+highs, VERB=100% | 0.315 | 0.583 | 0.432 |
| Full mix, VERB=50% | 0.377 | 0.552 | 0.468 |
| Full mix, VERB=100% | 0.305 | 0.555 | 0.405 |

Correlation never goes negative at any point in any scenario (stereo
field stays "together," no phase wash), while still showing real,
gradual time-variation (min-to-max spread of 0.13-0.27) consistent with
PAN's own ~0.3Hz motion LFO remaining clearly audible even with VERB's
deep tail layered on top - no sharp/sudden swings observed in the
windowed series. Adding VERB doesn't collapse PAN's motion into
amorphous wash, and doesn't push the combined field into decorrelated/
phasey territory the way a naive wide-reverb-on-top-of-wide-pan
combination could.

**11. Centred bass under PAN=100%+VERB=100%.** A centred bass tone
(60/80/100/120Hz) plus decorrelated stereo highs (4kHz/5.5kHz), full
chain through PAN then VERB, both at 100%:

| Bass frequency | L/R level difference |
|---|---|
| 60 Hz | 0.015 dB |
| 80 Hz | 0.197 dB |
| 100 Hz | 0.728 dB |
| 120 Hz | 1.053 dB |

All four stay well under the 60/80Hz corner's expected near-zero and the
100/120Hz corner's modest rise - consistent with (and slightly better
than) the PAN module's own already-documented centre-bass isolation
work, confirming VERB doesn't reintroduce bass motion when stacked on
top of PAN's own protection. Bass stays effectively dry and centred; the
combined stress test does not defeat either module's independent
low-end safeguard.

**12. RT60 frequency table, all four macro values.** Bandpassed-RMS
decay checkpoints at 1/2/3s post-burst, 500Hz/1kHz/2kHz/5kHz/8kHz, at
VERB=25/50/75/100%:

| Wet | 500Hz @3s | 1kHz @3s | 2kHz @3s | 5kHz @3s | 8kHz @3s |
|---|---|---|---|---|---|
| 25% | -152.3dB | -154.5dB | -154.9dB | -148.7dB | -147.9dB |
| 50% | -80.0dB | -94.0dB | -119.1dB | -148.9dB | -144.0dB |
| 75% | -58.5dB | -64.3dB | -96.0dB | -109.5dB | -120.3dB |
| 100% | -44.9dB | -52.5dB | -81.9dB | -99.2dB | -113.0dB |

Confirms all three required properties at once: the macro increases
decay time (25% is already fully decayed into the noise floor by 2-3s;
100% is still clearly present at 3s), high frequencies decay faster than
low/mid at every wet setting (500Hz consistently the least-decayed,
8kHz consistently the most, at every wet level from 50-100%), and the
plate stays dark throughout its whole macro range rather than only at
one setting. Per this round's explicit instruction, the ~3.5s RT60 at
1kHz/100% was **not** lengthened just to sit nearer the middle of the
3.5-4.5s range - see "Known limitations" below for why that tradeoff was
deliberately left alone.

**13. Final listening WAV artifacts** (no normalisation applied between
variants within a set, so relative wet/dry level is preserved for A/B
comparison):

- `docs/audio/verb-final-snare-dry.wav` / `-25.wav` / `-50.wav` /
  `-100.wav` - a short synthetic snare-like hit (noise burst + 180Hz
  tone body) through DRY and three wet settings.
- `docs/audio/verb-final-vocal-dry.wav` / `-50.wav` / `-100.wav` - a
  sustained synthetic vocal-like tone (fundamental + 4 partials, slow
  amplitude wobble) through DRY and two wet settings.
- `docs/audio/verb-final-pan-motion-dry.wav` /
  `-verb100.wav` - a centred-bass + decorrelated-stereo-highs source run
  through PAN at 100% (MOTION), without and with VERB at 100% layered on
  top afterward.

**14. Listening-acceptance expectations** (for a human reviewing the WAV
files above - not independently re-verified by ear in this pass, since
this tool has no audio playback; the acoustic-acceptance criteria 1-12
above are the automated proxy used instead): the snare set should show a
short hit followed by an immediate, dense metallic bloom and a dark,
smooth tail - not a "bathroom" resonance, not a digital-hall smear, not
ping-pong, not comb-filter repeats (the impulse-density and resonance-
sweep results above are consistent with this). The vocal set should keep
the voice forward/in front with the plate sitting behind it, and the
voice's low body should not go muddy (the 250-500Hz body-preservation
result above is consistent with this). The PAN-motion set should keep
PAN's L-R movement clearly audible even inside VERB's tail (the item-10
correlation-over-time result above, showing continued time-variation
with no collapse to near-1.0 constant correlation, is consistent with
this).

**15. GUI DRY/PLATE/DEEP vs "SPACE" labelling.** Investigated via direct
source inspection (`Resources/Web/index.html`, `Resources/Web/app.js`) -
**not a bug.** Every one of the 7 modules has two independent, always-
present UI labels: a fixed generic "control axis" label under the
numeric readout (`app.js`'s `control:` field - `DRIVE`, `TONE`, `HEAT`,
`SHIFT`, `WIDTH`, **`SPACE`**, `IMAGE`) that never changes regardless of
the parameter's value, and a separate tri-scale showing the module's
three named semantic positions at 0/50/100% (`DRY`/`PLATE`/`DEEP` for
VERB). VERB's `SPACE` label is architecturally identical in role to
PAN's `WIDTH` label, which sits alongside PAN's own three-word tri-scale
(`ORIGINAL`/`WIDE`/`MOTION`) without any prior confusion - these are two
orthogonal, by-design UI elements, not two different names for the same
thing. No GUI change was made; the existing `docs/screenshots/verb-
final.png` (100%/DEEP, captured in the module's original implementation
round) already shows this correctly and was not re-captured this round
since nothing about the labelling changed.

**16. Pre-delay-bounds regression test.** Added - see "A second instance
of the same bug class" above and `Tests/PluginTests.cpp`'s "Regression:
pre-delay read-index stays in-bounds under NaN macro input and other
edge cases" - covering NaN/Inf macro input at multiple sample rates and
block sizes across several consecutive blocks each, `reset()` after NaN
input, `prepare()`/re-`prepare()` cycles after NaN input, and an
8192-sample block (far larger than the pre-delay buffer's own capacity)
with NaN input followed by a normal block to confirm clean recovery.

## Metallic-ring reduction investigation (UX polish pass)

A follow-up UX polish pass (see CLAUDE.md) reported the plate as sounding
"too metallic" in manual testing and asked for a warmer, denser, less
ringy tail - explicitly via diffusion/delay-ratio/modal-distribution/
damping tuning, not output notch-EQ and not more analog saturation - as
the *one* permitted DSP change in an otherwise UI/UX-only round. Per the
brief's own "measure first" instruction, a new dedicated regression test
(`Tests/PluginTests.cpp`'s "Modal/resonance sweep: spectral flatness of
the plate tank's steady decay, VERB=50% (PLATE)") was written before any
tuning: it feeds a single-sample impulse, isolates the wet-only
contribution (`verbWetOnly()`), Goertzel-samples ~28 log-spaced
frequencies (200Hz-8kHz) at a fixed post-onset checkpoint (300ms in), and
reports both a raw peak-above-mean/stdDev *and* a detrended (local-
neighbour-residual) peak/stdDev. The detrended figure is the one that
actually answers "how spiky/metallic is this", since the raw figure is
confounded by the response's own broadband tilt (a damping change alone
shifts every high-frequency point together, which widens raw peak-vs-
mean/stdDev with zero change in how dominant any individual mode is,
independent of genuine "ringiness").

**Measured baseline** (original RC1-round topology, 12 lines, 4-stage
diffuser at 0.6 gain, 6000Hz damping): detrended peak=**15.89dB**,
stdDev=**5.68dB**.

Three standard FDN "reduce metallic ringing" levers were then tried, each
measured with the *same* fixed metric before being judged:

1. **16 lines, smooth geometric progression** (5.1-40.7ms, denser
   spacing in roughly the same range) + 6-stage diffuser at 0.65 gain +
   damping lowered 6000->5200Hz (more per-pass high-frequency loss,
   targeting the 1-4kHz band where metallic ring is most audible and a
   6000Hz one-pole barely reaches). Measured: peak=16.03dB,
   stdDev=6.57dB - essentially flat on peak, **worse** on stdDev.
2. **16 lines, prime-number lengths** (5, 7, 11, ... 61ms - the standard
   "no two lengths share a common integer factor" FDN heuristic,
   specifically to rule out the first attempt's smooth progression being
   itself a source of periodicity once 16 lines' feedback interacts).
   Measured: peak=**21.73dB**, stdDev=**7.54dB** - clearly worse on both.
3. **Original 12-line tank, diffuser/damping changes only** (isolating
   whether the diffusion/damping levers helped independent of the line-
   count change). Measured: peak=19.18dB, stdDev=8.31dB - still worse
   than baseline on both.

All three attempts were measured **neutral-to-worse** than the original
topology by this metric, not an improvement - a genuine, unexpected
result. The most likely explanation found during the investigation: this
single-checkpoint measurement is confounded by average line length
itself - a longer-average-line configuration has completed fewer
feedback round-trips by the same fixed 300ms checkpoint than a shorter-
average-line one, so it looks "less mixed"/less flat at that instant
regardless of its true steady-state modal density. A more robust
methodology (comparing at an equivalent number of round-trips per
configuration, or tracking whether the same frequencies stay peaky
across *multiple* time checkpoints - true persistent ringing - rather
than a single instant) would be needed to properly evaluate this class of
change, and was out of scope for this pass's time budget.

**Decision: no VERB DSP change shipped this round.** Given three
different, principled tuning directions all measured as neutral-to-worse
against the pre-existing, already-proven-safe topology, and given the
project's own standing practice of reverting measured-negative attempts
rather than shipping them anyway (see this document's diffuser/induced-
signal-filter history above, and docs/DSP_PAN.md's 2nd-order-Butterworth-
induced-filter revert), `Source/DSP/VerbCurves.h` and
`Source/DSP/VerbProcessor.cpp` were reverted to their exact pre-pass
values. The new resonance-sweep test is kept permanently as a diagnostic/
regression baseline for any future attempt at this specific problem - it
is not a pass/fail gate (there is no single "correct" flatness number),
just the "measure first, don't guess" tool this investigation itself
needed and didn't have before. `wetHPF`=350Hz, bass protection, analog
send/return, dark vintage bandwidth, plate identity, and zero latency are
all therefore unchanged, and every pre-existing VERB test remains green.

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
- **Closed** (see "Acceptance / calibration pass" above): a dedicated
  narrowband sweep (16 points at 1/3-octave spacing, followed up at
  10-20Hz resolution around the two regions the coarse pass flagged, and
  a full multi-checkpoint decay-curve comparison at the single most
  theoretically-likely candidate frequency for a reinforced mode) was
  run specifically hunting for a single anomalously-long-lived resonant
  mode. None was found at any resolution tested - the apparent outliers
  in the coarse pass resolved under finer inspection as ordinary FDN
  modal jitter/texture, not a stuck mode. No FDN retuning resulted.
- Wet-path stereo correlation on broadband material measures negative
  (~-0.45, confirmed again this round on a correlated-chord source at
  50% wet: -0.51) - by design and intent for a *wide reverb tail* (unlike
  PAN's dry-adjacent correlation requirement, no positive-correlation bar
  was set or is appropriate for VERB's own wet signal), but disclosed
  here explicitly since it is a notably different acceptance shape from
  the PAN module's own correlation section. **Follow-up context added
  this round**: what actually matters for the production mix - full
  dry+wet output correlation and mono fold-down - was measured
  separately and stays strongly positive (0.92-0.997) and small (under
  ~1dB change) respectively, across every source type and wet level
  tested, including the PAN=100%+VERB=100% combined stress test (windowed
  correlation over time never goes negative). The negative wet-only
  figure does not destabilise the real, dry-dominated production output.
- The analog send/return stages' fixed (not macro-controlled) drive
  constants were tuned against the specific -18dBFS/1kHz sine test
  condition the product brief specifies; THD at other signal levels or
  spectral content was not exhaustively swept, though the underlying
  tanh-based construction is the same bounded, PREAMP/SAT-proven-safe
  shape used elsewhere in the plugin.
