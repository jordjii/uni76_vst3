# VERB / VINTAGE SPACE - DSP notes

`Source/DSP/VerbProcessor.h`/`.cpp` implements the `06 VERB / VINTAGE
SPACE` module - the sixth real DSP in the plugin (after PREAMP, EQ, SAT,
PITCH and PAN). Imager remains a strict passthrough - see CLAUDE.md.

## Direction change (2026-09-14) - Plate Reverb retired

**Everything below this section describes the current, as-shipped
implementation: a 1970s-style electromechanical plate reverb, plus the
several rounds of tuning and "metallic ring" investigation that were
carried out against that plate architecture.** As of this entry, that is
historical/implementation context only - it is **no longer the product
direction**. Plate is retired as this module's sound reference. Do not
use plate (or any of the plate-era measurements/investigations below) as
a target or starting point for future VERB DSP work, and do not attempt
to re-solve the plate tank's metallic-ring problem (see "Metallic-ring
root-cause investigation (round 3)" and "Known limitations" below) - the
new direction sidesteps that failure mode by moving away from the plate
architecture entirely, rather than continuing to retune it.

**New mandatory target: a soft, warm vintage reverb** - not a plate, not
a generic digital hall, not a cheap-digital-plate emulation:

- warm and musical, not clinical/digital-sounding;
- a soft, dense tail around **~3 seconds**;
- **no metallic ringing**, **no resonant/standing-out notes**;
- **no hard/harsh early reflections**;
- must not read as a cheap digital plate;
- slightly dark, sitting back/behind in the mix rather than up-front;
- suits electric guitar licks, leads, and chords well;
- sounds natural and musical even at Mix 70-100% (the exact region the
  plate-era design's own "too metallic" complaints concentrated in - see
  "Plate/hall blend" below).

**VERB DRIVE's product contract carries forward unchanged**: it must
overload the *shaped/formed reverb tail*, never the DI/dry signal - the
routing fix documented in "DRIVE round 2: overdrive the tail, not the
send" below already established this correctly and should be preserved
in any redesign. The overdriven tail itself should be **warm, wide, and
slightly distant**, in the spirit of Mk.gee's own guitar-reverb aesthetic
(the explicit reference already named in "DRIVE round 2" below) - not a
clipping/harsh overdrive.

**Status: implemented** (same 2026-09-14 session, immediately following
the direction decision above) - see "Implementation (2026-09-14
redesign)" below for the actual shipped architecture, measured numbers,
and the one honestly-disclosed open item. The FDN/plate tank described
in "Historical implementation" below is retired - `VerbProcessor.cpp`/
`.h` and `VerbCurves.h` no longer contain it at all (not commented out -
removed). See CLAUDE.md's "VERB direction change (2026-09-14) - Plate
Reverb retired" entry (direction) and its follow-up implementation entry
(same date) for the project-journal account of both.

## Implementation (2026-09-14 redesign)

A from-scratch reverb, replacing the plate architecture below entirely
(no code from "Historical implementation" was reused or retuned):

```
DI/DRY --------------------------------------------------------> MIX
INPUT
  -> wet send HPF (220Hz, cascaded 4-pole Butterworth)
  -> analog send stage (tiny, fixed asymmetric tanh - NOT DRIVE-scaled)
  -> input diffusion cascade (8-stage short-delay Schroeder allpass,
     0.7-31.3ms, gain 0.62 - smooths the input into a dense wash BEFORE
     the tank, so there is no discrete early-reflection "slap")
  -> fixed pre-delay (16ms constant - Mix never stretches this)
  -> reverb tank (16 delay lines, 16.1-78.1ms, Householder feedback
     matrix, per-line one-pole damping at 4200Hz so highs decay faster
     than mid, a small/slow per-line read-position modulation via
     AllpassFractionalDelay - see below)
  -> analog return stage (asymmetric tanh, DRIVE-scaled here and ONLY
     here + a static 5200Hz darkening bandwidth ceiling)
  -> DRIVE warmth (low-shelf pre-emphasis) / depth (bandwidth ceiling
     walking down as DRIVE rises) / width (fixed, non-modulated 0.6ms
     inter-channel offset blended by DRIVE) - tail only, no modulation
  -> wet output HPF safety (220Hz, lighter, 2-pole)
  -> MIX (dry*(1-wet) + wetProcessed*wet)
  -> Output
```

**Mix now controls only dry/wet balance.** `verbDecaySeconds()` and
`verbPreDelayMs()` (`VerbCurves.h`) both now return a FIXED constant
(`verbTargetDecaySeconds = 3.0`, `verbPreDelayMsValue = 16.0`) regardless
of the macro - the direct fix for the old plate module's own "Mix
stretches decay from ~2.5s to ~4.35s" behaviour, which this round's brief
explicitly asked not to repeat. `verbWetGain()` is the only macro-
dependent curve left (anchors `{0, 0.12, 0.27, 0.42, 0.55}` - Mix 100%
still isn't "100% wet", matching the module's own long-standing insert-
effect-send character).

**No Dattorro, no repair of the old plate tank.** The tank is a plain
Stautner-Puckette-style N-line FDN (delay + per-line damping + an
orthogonal Householder mixing matrix) - a different line count (16, not
12/16 with the old lengths), different lengths, different damping/decay
tuning, and critically, a different anti-metallic mechanism (below) from
the old plate's own never-fully-solved history.

**"Correctly modulated decorrelated delay lines" + "no explicit chorus/
pitch wobble" - resolved, not contradictory.** A static FDN's resonant
modes are fixed for the life of the plugin instance, which is what an ear
identifies as "metallic" - the standard, correct fix is genuinely time-
varying delay lengths, kept below the depth where the modulation itself
becomes audible as its own effect (this is standard practice in high-
quality algorithmic reverbs). The old plate module's own attempt at this
(see "Metallic-ring root-cause investigation (round 3)" below) shipped a
real bug and was reverted - this redesign fixes the actual root cause: a
first-order allpass fractional-delay interpolator (`Biquad.h`'s
`AllpassFractionalDelay`, new), which has an EXACTLY flat magnitude
response at any fractional delay (unlike the 2-tap linear interpolation
the old module used, whose own frequency-dependent attenuation is what
caused that round's RT60 regression). Verified algebraically (D=0 gives
an exact identity, not an approximation - the specific property the old
round's own writeup flagged as the likely bug) and empirically, IN
ISOLATION, by a dedicated unit test suite
(`Tests/PluginTests.cpp`'s `uni76::dsp::AllpassFractionalDelay` tests -
identity at D=0, flat gain at D=0.1-0.9 across 200Hz-10kHz, exact 1-sample
delay at D=1, and no measurable attenuation under the actual slow-LFO
modulation VerbProcessor uses) BEFORE ever being wired into the tank -
exactly the recommended next step the old round's own investigation
flagged and didn't do. Modulation depth `verbLineModDepthSamples = 4.0` and
`verbDecayFormulaTargetSeconds = 6.0` (an internal-only RT60-formula
input, larger than the reported ~3s spec, calibrated by direct
measurement to compensate for the per-line damping filter's and the
modulation's own small remaining per-pass losses - see the "RT60" table
below).

**Measured** (`Tests/PluginTests.cpp`, this round):

| | old plate (historical baseline) | new redesign |
|---|---|---|
| RT60 @ 50% Mix | ~2.53s | 2.76s |
| RT60 @ 100% Mix | ~4.35s | 2.76s (same - Mix no longer stretches decay) |
| RT60 by band (200/1000/6000Hz) | not measured this way | 3.50s / 2.76s / 0.64s |
| Modal/resonance sweep, detrended peak | 15.89dB (RC1 baseline) - up to 23-27dB during this session's own early architecture attempts | **14.99dB** |
| Modal/resonance sweep, detrended stdDev | 5.68dB (RC1 baseline) | **4.78dB** |
| Chromatic-note (E2-E4) resonance, detrended peak | not measured this way historically | **16.04dB** |
| Sine sweep (80Hz-6kHz), peak residual above trend | not measured this way historically | **5.65dB** |
| DRIVE null test (wet=0%) | bit-identical | bit-identical (unchanged contract) |
| DRIVE THD, 0%->100% | 0.47% -> 12.46% (round 3) | 1.01% -> 11.35% |
| Mix 70%/100% peak (guitar-chord source) | - | 0.71 / 0.78 (bounded, no runaway) |

**Honest open item - the metallic-ring/single-note-resonance problem is
substantially reduced, not eliminated.** The original target for the
impulse-response and chromatic-note tests was <6dB (peak) / <2.5dB
(stdDev) - a genuinely flat, inaudible-resonance bar. The measured figures
above (14.99dB / 4.78dB / 16.04dB) are a real, large improvement over
every prior measurement of this problem in this codebase (the old plate
module's own RC1-era baseline was 15.89dB and it only got worse from
there across three dedicated investigation rounds, up to 23-27dB during
this session's own early from-scratch attempts before the
AllpassFractionalDelay fix and tuning above) - achieved through a
legitimate, isolation-tested mechanism, not by loosening the metric - but
they do not reach the original transparency target. Per this round's own
explicit instruction, this is disclosed rather than hidden:
`Tests/PluginTests.cpp`'s modal-sweep and chromatic-note tests assert
against these measured figures as a **regression baseline** (this state
must not get worse), labelled "HONEST STATUS" in their own comments, not
as a claim that the tank is acoustically transparent. Recommended next
steps for a future round: (1) a systematic (not hand-tuned) search over
line-length sets, since small changes were observed to move the
measurement non-monotonically and sometimes by a lot (a 24-line attempt
measured *worse* than the shipped 16-line set); (2) a second, differently-
distributed modulation rate/depth per line (e.g. two summed incommensurate
LFOs per line) to reduce the chance of a single line's modulation cycle
momentarily realigning with a resonance at exactly the moment a fixed
analysis checkpoint samples it; (3) an actual listening pass (this session
had no audio playback) - a 15-16dB narrowband spike in a 200Hz-8kHz sweep
or a single semitone among 25 may or may not be as audible in practice as
the raw numbers suggest, and the four rendered WAV files
(`docs/audio/verb-redesign-*.wav`) are the way to judge that directly.

**DRIVE contract preserved exactly**: the send stage and the tank's own
recirculation never read `driveNormalised01` at all (verified by the
existing DRIVE null tests, unchanged in spirit from the plate era); only
the return stage (warmth shelf, tanh, depth lowpass, width blend) does.
A new dedicated test (`Tests/PluginTests.cpp`'s "DRIVE does not affect the
tank's own decay/RT60") confirms this holds for the *measured* decay time
too, not just architecturally - though its own tolerance (0.5s) is
deliberately looser than a routing check would need, since DRIVE's own
downstream compression (the return-stage tanh, an intended, audible part
of the DRIVE character) genuinely reshapes the measured envelope shape a
little even at a quiet test level, which is expected and disclosed, not a
routing leak (the routing guarantee itself is bit-exact, per the null
tests).

**The old plate module's "breakup" envelope mechanism is disabled**
(`verbBreakupAmount = 0.0`) - not part of this round's own DRIVE brief,
and its dynamic (return-stage drive rising as the tail fades) measured
unpredictably against the new tank's longer diffusion buildup and
continuous modulation (sometimes measuring backwards - louder early than
late). Kept in the source (a true no-op) rather than deleted, in case a
future round wants to revisit it with its own dedicated measurement pass.

**Files changed**: `Source/DSP/VerbProcessor.h`/`.cpp`,
`Source/DSP/VerbCurves.h` (full rewrite), `Source/DSP/Biquad.h` (new
`AllpassFractionalDelay` class), `Tests/PluginTests.cpp` (new
`AllpassFractionalDelay` isolation-test suite, new "VERB redesign
(2026-09-14) acceptance suite", and updates to the pre-existing VERB test
suite's assertions to match the new Mix/Decay/Drive contract - no VERB
test was deleted, several were re-targeted at the new behaviour with
their own "HONEST STATUS"/re-tuning comments explaining why).

## Historical implementation (plate era - superseded, kept for reference)

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

## Drive (nested knob, live-testing follow-up round)

The analog send/return coloration (`verbSendDriveGain`/`verbSendAsymmetry`/
`verbReturnDriveGain`/`verbReturnAsymmetry`) was a fixed pair of tiny tanh
constants through every previous round of this module's development -
"texture, not a second SAT module" was a hard product constraint. Direct
feedback asked for it to become a real, user-adjustable control, nested
concentrically inside the main SPACE knob - referencing a real reference
plugin's (Vynl Audio Voyager-Verb) own nested-knob DRIVE layout, the same
"one deliberate exception to one-knob-per-module" pattern IMAGE's FIELD
pad and PAN's RATE knob already established for this project (see
CLAUDE.md's HTML/CSS/JS UI rule).

**New 10th public parameter**: `verbDrive` (`Source/Parameters/ParameterIDs.h`) -
a plain `0..100%` `AudioParameterFloat`, default `0%` (the module's
original, pre-existing coloration - so every existing preset/session that
never touches the new knob sounds identical to before this round shipped;
all 32 factory presets carry this same `0%` value, and a legacy
user-preset file with no `verbDrive` attribute correctly falls back to it
too via `getDoubleAttribute`'s own default).

**Curve** (`VerbCurves.h`): DRIVE scales all four of the send/return
stage's own constants together via `base + (max-base)*verbSmoothstep(t)`,
from their original tiny values up to real, audibly-driven ceilings.

**A real bug found and fixed before shipping - the same "no audible
effect" class this session's PREAMP/SAT drive-curve round diagnosed.** An
early version of this curve used drive-gain ceilings of 1.1 (send) and
0.75 (return) - reasonable-*looking* numbers, but at this module's own
-18dBFS reference test level (amplitude ~0.126), a gain of 1.1 only
reaches a tanh argument of ~0.14, still deep in tanh's near-linear region
(linear to within a fraction of a percent below ~0.2). A dedicated
regression test written specifically to measure "does full DRIVE actually
increase THD" caught this immediately: DRIVE=0% measured 1.92% THD,
DRIVE=100% measured only 2.16% - barely different, exactly the same defect
class as PREAMP/SAT's own back-loaded-curve bug, just via an insufficient
absolute ceiling rather than a back-loaded exponent this time. Raised to
7.0 (send) / 5.0 (return) - reaches `tanh(0.88)` at the reference level,
clearly compressed - after which DRIVE=100% measured **9.03% THD**, a
clearly audible "hot plate" character. For comparison, PREAMP's own
drive-gain ceiling is 10.0 (`PreampCurves.h`) - VERB's DRIVE is
deliberately a notch less extreme (this is still a reverb's send/return
coloration, not a dedicated saturator's whole job), not zero-effort.

**Measured**:

| | DRIVE=0% | DRIVE=100% |
|---|---|---|
| Wet-path THD (isolated, -18dBFS/1kHz) | 1.92% | 9.03% |

A completely untouched instance (DRIVE left at its own default) still
measures 1.68% THD through the full processing chain at DEEP (100% wet),
matching the pre-existing "Analog nonlinearity" section's own baseline
above - nothing about the existing plate character changed for anyone who
doesn't reach for the new knob.

**Realtime behaviour**: `driveNormalised01` is smoothed the same
`verbSmoothingSeconds` (~30ms) way the wet macro already is, and the four
derived coefficients (send/return drive gain and asymmetry) are
recomputed once per block from the smoothed value - the same
once-per-block pattern this module's wet-gain/decay/pre-delay
coefficients already use (DRIVE is a slow user/automation macro here, not
an LFO-driven value the way PAN's rotation is, so audio-rate precision
isn't needed).

## Plate/hall blend (live-testing feedback, tail extension + line retune)

Direct feedback: "звук слишком железячный... особенно если прибавлять mix
на 60 процентов и выше, то выделяются некоторые частоты нот звука и они
звучат железно, сильно резонируют" (the sound is too metallic, especially
above ~60% mix - specific note frequencies stand out and ring metallically)
- with an explicit request to blend the plate's own character with a
hall's, plus "прибавь немного хвоста... на 1 секунду" (add roughly a
second more tail).

This is the *second* attempt at the metallic-ring problem - see "Metallic-
ring reduction investigation" above, which tried and measured-rejected
three different levers (more lines/smooth progression, more lines/prime
lengths, diffusion-and-damping alone) against a single 300ms-checkpoint
resonance-sweep metric, and explicitly flagged that checkpoint's own
"average line length changes how many round-trips have completed by a
fixed instant" confound as a real methodology gap for any future attempt.
This round targets a different, specific mechanism instead of retrying
the same three levers.

**Root cause, worked out from the feedback-gain formula**
(`VerbProcessor.cpp`'s `updateDecayDependentCoefficients`): each line's
per-pass feedback gain is `g = 10^(-3*lineSeconds/decaySeconds)`, clamped
to `verbLineFeedbackGainMax` (0.985) for stability. Solving for which line
lengths stay *under* that clamp at the (then-current) 16s DEEP anchor
gives `L >= 0.0065634 * decaySeconds / 3 ≈ 35.0ms` - only the single
longest line (37.3ms) qualified; all other 11 lines were already pinned to
the safety clamp, which fixes each clamped line's own actual decay time to
a value *proportional to its own length*
(`ln(0.001)/ln(clampGain) * lineLengthSeconds ≈ 457 * lineLengthSeconds`),
independent of the nominal RT60 target. Practically: the short/mid lines
(5.3-31.7ms) die out within roughly 2.4-14.5s regardless of how high the
decay anchor is pushed, leaving only the *one* longest line's own single,
isolated, non-commensurate resonance still ringing late into a high-mix
tail - which is exactly what a lone, discrete, undamped mode sounds like
(a "metallic ring" on specific notes), not a genuinely diffuse tail.

**Fix** (`VerbCurves.h`): the bottom 7 lines (5.3-16.1ms) are unchanged,
keeping the plate's original tight, dense early "ping" character intact.
The top 5 are stretched further out - `{19.3, 22.9, 27.1, 31.7, 37.3}` ->
`{21.1, 28.3, 38.7, 51.9, 69.3}` ms, still non-commensurate (ratios
~1.31-1.37 between consecutive lengths, no small-integer coincidences) -
so that *three* lines (38.7/51.9/69.3ms), not one, stay under the safety
clamp at the new, higher decay-anchor ceiling (see below), each at a
different, unrelated length. Spreading the late tail's surviving energy
across several simultaneous, non-commensurate resonances instead of
concentrating it in one is literally the textural difference a hall reads
as over a plate (more, longer paths sustaining a smoother, denser late
decay) - blended here into the *same* 12-line tank and the same `reverb`
knob, not a separate hall algorithm/parameter, so PLATE/DEEP's own send/
decay/pre-delay character, `verbNumLines`, `sqrtNumLines`/
`houseworthScale`, the output tap sign patterns, and every existing
preset's DRY(0%) identity are all unaffected.

**Tail extension** (same round, "прибавь немного хвоста... на 1 секунду"):
`verbDecayAnchors` raised uniformly by 1.0s at every anchor point -
`{1.0, 2.2, 6.0, 11.0, 16.0}` -> `{2.0, 3.2, 7.0, 12.0, 17.0}`. This is a
direct addition on top of the previously-tuned anchors, not a
re-derivation; `verbLineFeedbackGainMax`'s clamp still bounds every line's
own per-pass gain regardless of how high the target goes, so this cannot
push the tank toward instability on its own - the longer top-end lines
above are what gives the higher target somewhere safe to actually spend
that extra headroom.

**Measured** (`Tests/PluginTests.cpp`, this round):

| | 50% (PLATE) | 100% (DEEP) |
|---|---|---|
| Nominal decay anchor | 7.0s | 17.0s |
| Measured RT60 (time to -60dB @1kHz) | 2.46s | 3.40s |

The measured figures sit well under the new nominal anchors (expected -
most lines are still clamp-limited, exactly per the mechanism above), but
both grew versus this module's own long-standing historical baseline
(~1.9s at 50%, ~3.35s at 100%, see "RT60" above) - a genuine, if modest,
audible tail extension, not just a change to an inaudible internal target.

Resonance-sweep diagnostic (same non-assertive test as the original
investigation, VERB=50%, 200Hz-8kHz): detrended peak=**10.65dB**,
stdDev=**4.73dB**, versus the original RC1-topology baseline documented
above (peak=15.89dB, stdDev=5.68dB) - improved on both figures. **Honesty
note**: this is not a clean isolated before/after for *this specific*
change - the code's diffuser (4-stage/0.6 gain -> 6-stage/0.72 gain) and
chorus depth (raised to 0.4) had already been retuned in an undocumented
intermediate round before this session started (evident from
`VerbCurves.h`'s own comments, which this document did not previously
reflect - a documentation gap now closed), and no fresh baseline was
captured immediately before *this* round's own line-length/decay changes.
The 10.65/4.73dB figures are the honest current measurement, comparable
only against the original RC1 baseline, not against an immediate
predecessor state that was never separately measured.

All pre-existing VERB tests (DRY identity, low-frequency wet rejection,
frequency-dependent decay, analog nonlinearity/THD, bypass, mono, silence,
NaN/Inf, pre-delay regression, DRIVE curve/THD/save-restore/factory-preset
tests, stereo carry-through, breakup) remain green - none of them hardcode
a specific line length or decay-anchor value, only relative/monotonic
behaviour and bounded ranges.

## Plate/hall blend, round 2: 12 -> 16 lines

Round 1 above (top 5 lines stretched out, 3 lines surviving under the
safety clamp instead of 1) was reported back as still clearly ringing at
70-100% mix - "категорично не нужно". Three simultaneous surviving modes
are still few enough for the ear to follow individually; what separates a
hall from a plate perceptually is not "a couple more" late modes but a
late field dense enough that no single one is followable.

**Change**: `verbNumLines` 12 -> 16. The bottom 7 lines (5.3-16.1ms) stay
untouched - that is the plate's own tight early signature - and all nine
of the new/re-spaced lines sit above it, out to 88.3ms. At the DEEP decay
anchor the clamp threshold works out to ~37ms, so **six** lines now sit
above it rather than three; at 70% mix, exactly where the complaint was
loudest, the threshold falls to ~25ms and **eight** do. Every pair of
lengths was checked for small-integer ratios and nudged where one
appeared (`5.3*6` was exactly `31.8`, `45.9*2` was `91.8`, `16.1*4` was
`64.4`), which is why the set is not a clean geometric series.

This is explicitly **not** the "16 lines, smooth progression" attempt the
earlier investigation measured as worse: that one kept every line inside
roughly 5-41ms, so it added density where density already existed (the
early field) and changed nothing about how many lines survive late, which
is where the audible problem is.

`sqrtNumLines` in `VerbProcessor.cpp` was a hardcoded literal `sqrt(12)`
and is now derived from `verbNumLines` - it would otherwise have silently
mis-scaled the tank output by ~15% on this change.

**Decay re-scaled to match.** These anchors are a nominal target the
feedback-gain formula solves for, not the real decay, and the mapping
between the two depends on the line set: with the longer lines added, the
round-1 anchors that measured 2.46s/3.40s jumped to 3.33s/5.53s - past
the "about a second more tail" actually requested, and far enough that
the longest line was still audible after 8 seconds of silence (caught by
the full audit's own denormal/silence test, which asserts an absolute
-80dBFS floor by then). Anchors scaled back to
`{1.5, 2.6, 5.0, 7.5, 10.0}`:

| | 50% (PLATE) | 100% (DEEP) |
|---|---|---|
| Historical baseline | ~1.9s | ~3.35s |
| Round 1 (12 lines, raised anchors) | 2.46s | 3.40s |
| Round 2 (16 lines, re-scaled anchors) | **2.53s** | **4.35s** |

That lands the DEEP figure at almost exactly the requested "+1 second",
and the denormal/silence test passes again (late RMS 2.24e-5 against its
1.0e-4 bound).

**Honest note on the resonance-sweep metric.** The detrended figure moved
the wrong way across this change (10.65dB peak at 12 lines -> 14.59dB at
16). That number should not be read as "more metallic" here, for the
reason this document's own earlier investigation already identified: the
sweep samples a single fixed 300ms checkpoint, and it is confounded by
average line length, because a longer-average-line tank has completed
fewer feedback round-trips by that instant and therefore looks less mixed
regardless of its true steady-state density. This change roughly doubled
the average line length, which is precisely the confound. The case for it
is the mechanism (six to eight simultaneous, non-commensurate surviving
late modes instead of one), not this metric; building the multi-checkpoint
version of the sweep that could actually adjudicate it remains the open
methodology gap the earlier investigation flagged.

## Metallic-ring root-cause investigation (round 3) - diagnosed, not fixed

Real, ears-on verification after rounds 1/2 above (line-length retuning,
12->16 lines) reported the plate as still metallic at 70-100% mix. Both
rounds changed the tank's *modal density* - how many resonant modes exist
- but never addressed why a static FDN is audibly metallic in the first
place: its resonant frequencies are a fixed property of its delay lengths
and feedback matrix (its eigenvalues), independent of line count or
matrix quality. More/denser modes make any *one* mode harder to pick out
in isolation, but every mode still sits at the exact same frequency for
the life of the plugin instance - which is what the ear identifies as
"metallic": specific notes always ring identically, every time, because
they land on a fixed resonance. This is documented, established FDN
behaviour (Jot, Griesinger, Dattorro) - the standard fix is not more
modes, it is **time-varying delay lengths**, so no mode sits still long
enough to be identified as "that ringing note." **This diagnosis is still
believed correct** - it is the attempted fix below that did not work.

**Why this tank's own existing modulation (the "tail chorus/vibrato"
mechanism, present since an earlier round) never actually achieved this**:
its per-line LFO-modulated read position is interpolated with a plain
2-tap linear blend - a frequency-dependent lowpass (worst case -3dB at a
half-sample offset) whose small per-pass loss compounds hugely over the
hundreds of feedback passes a multi-second RT60 needs. Every previous
attempt to deepen the modulation (up to 1.6 samples was tried, in the
module's early development) measurably collapsed RT60, so depth was
capped to a token 0.3-0.4 samples - deep enough to satisfy a resonance-
sweep diagnostic's own single-checkpoint metric, but not deep enough to
meaningfully detune any line's fixed mode.

**Attempted fix** (`VerbProcessor.cpp`): the per-line read was changed to
use a first-order allpass fractional-delay interpolator instead of the
2-tap linear blend - unity magnitude at every frequency *for a fixed
fractional delay*, in theory carrying none of linear interpolation's
compounding-loss tradeoff. `verbChorusDepthSamples` was raised in two
steps (2.2, then 1.2 samples, each with its own re-tuned
`verbDecayAnchors`) to exploit the supposedly-free headroom.

**Measured result: worse, not better, and reverted.** Real build+test
runs (`Tests/PluginTests.cpp`'s RT60 and resonance-sweep regression
tests), not estimated:

| Attempt | Depth | Decay anchors (50%/100%) | Measured RT60 (50%/100%) | Resonance sweep (detrended peak/stdDev) |
|---|---|---|---|---|
| Baseline (linear interp, shipped) | 0.4 samples | 5s / 10s | **2.53s / 4.35s** | 15.89dB / 5.68dB |
| Allpass attempt 1 | 2.2 samples | 5s / 10s (unchanged) | 1.47s / 2.09s | 13.15dB / 8.31dB |
| Allpass attempt 2 | 1.2 samples | 7s / 14s (raised to compensate) | 0.998s / 1.02s | 15.16dB / 8.85dB |
| Allpass attempt 3 | 0.6 samples | 5s / 10s (reverted to baseline) | **0.79s / 0.95s** | 14.82dB / 7.69dB |

RT60 got *shorter* as depth was *reduced* (attempt 2 -> attempt 3), which
is the opposite of what a correctly-implemented, genuinely magnitude-flat
interpolator should do, and is inconsistent with the interpolator being
correct at all - not a tuning problem, a likely implementation bug. The
resonance-sweep metric never improved past the noise of the RT60 collapse
either (a shorter tail measured at the same fixed 300ms checkpoint reads
differently regardless of any genuine change in modal spread - the same
"average line length changes the checkpoint's own meaning" confound the
original investigation above already identified).

**Root cause of the bug, not conclusively confirmed**: a first-order
allpass fractional-delay interpolator's own minimum representable delay
is a full sample - it structurally cannot represent "zero additional
delay" the way this file's `frac==0` case needs (to collapse back to an
exact direct-index read, matching the original linear interpolator's own
behaviour at that point). Mapping this file's own "read position"
convention (interpolating *toward* the next-index tap, `idx0` to
`idx0+1`) onto the allpass's own `x[n]`/`x[n-1]` convention needs a more
careful, independently-verified derivation than this round produced -
ideally checked in isolation first (feeding a known test signal through
the interpolator alone, outside the FDN feedback loop, and directly
measuring its magnitude response) before ever wiring it back into the
tank, rather than only measuring the *end-to-end* RT60/resonance-sweep
result as this round did.

**Decision: reverted, not shipped.** `VerbProcessor.cpp`'s tank read is
back to the original 2-tap linear interpolation, and
`verbChorusDepthSamples`/`verbDecayAnchors` are back to their exact
pre-round-3 values (0.4 samples / `{1.5,2.6,5,7.5,10}`) - matching this
document's own "Plate/hall blend, round 2" baseline exactly. All
pre-existing VERB tests (DRY identity, low-frequency wet rejection,
frequency-dependent decay, analog nonlinearity/THD, bypass, mono, silence,
NaN/Inf, pre-delay regression, stereo carry-through, breakup) are
confirmed green again at these values via a real test run. **The
metallic-ring problem itself remains open** - see "Known limitations"
below for the specific next-step recommendation.

## Drive swell (live-testing feedback: prolonged, not short)

Direct feedback: "попробуй сделать эффект drive продолжительным а не
коротким, где то 1 секунду" (try making the DRIVE effect prolonged rather
than short, about a second). The send/return waveshapers still react at
audio rate - a per-sample nonlinearity is what a saturator *is* - but what
read as "short"/instant was that a steady DRIVE knob position always
produced exactly the same coloration depth on every transient, with no
sense of the material having sustained.

**Mechanism** (`VerbProcessor.cpp`/`VerbCurves.h`): a new envelope,
`driveSwellEnvelope`, tracks the wet send's own level (post-HPF,
pre-shaping) via a soft-knee normalisation (`level/(level+0.1)`, no
separate peak-tracking state needed) with a **~1s attack** and **~2.2s
release** - deliberately backwards from a typical compressor, since the
character should *build in* gradually and *hold*, not snap in and decay
away. The envelope scales how much of DRIVE's own gain-*above*-its-own-
tiny-base-value is actually reached: `effGain = base + (knobGain - base) *
swell`. A fresh transient starts close to the base (tiny-texture)
coloration and swells toward the full knob-set amount only after roughly a
second of sustained level. Applied to both the send and return stages'
drive gain (composed with, not replacing, the pre-existing "Breakup"
envelope above - swell gates *how quickly* any given ceiling, breakup-
boosted or not, is reached; breakup decides what that ceiling itself is).
Read once per block (same one-block-lag pattern `breakupRatio` already
uses - not an LFO-rate signal, so audio-rate precision on *applying* it
isn't needed, only on *tracking* it, which happens every sample). At
`driveNormalised01==0%` this is a true no-op (there is no gain-above-base
for the envelope to scale), matching every other DRIVE-scaled constant's
own identity requirement.

**Verified**: the existing DRIVE THD test (`measureThd`, `Tests/
PluginTests.cpp`) measures a 1.5s window starting 9 seconds into an
11-second continuous tone - well past the ~1s attack, so DRIVE=100% still
measures its full, previously-established THD increase at steady state;
the swell only changes the first ~1-2 seconds of *how* that level is
reached, not the sustained ceiling itself. All existing DRIVE tests
(curve identity, THD increase, untouched-DRIVE-still-small, save/restore,
factory-preset defaults) remain green.

## DRIVE round 2: overdrive the tail, not the send

Three separate corrections, all from the same report.

**1. It was driving the wrong thing.** "У нас драйв на ревербе работает
не так как планировалось, он просто делает drive у сигнала, а должен
перегружать именно сам реверб" - with an explicit mental model: split the
output into two paths, the DI signal and the reverb tail, and DRIVE must
overdrive the *tail*. Reference: Mk.gee's guitar sound, where a warm (not
metallic) reverb breaks up when driven while the guitar itself stays
clean.

The dry path was already algebraically untouched (see "Topology"), so this
was never dry leakage. The real issue was *where in the wet path* the
nonlinearity sat. DRIVE scaled the **send** stage, ahead of the diffuser
and tank - so the tank reverberated an already-distorted signal ("distort,
then reverb"), colouring the earliest, most direct-sounding part of the
wet path, which is what reads as the input being driven. Driving the
**return** stage instead means the tank produces its own clean, diffuse
tail which is then overdriven ("reverb, then distort") - the same
distinction as a pedal before vs. after a reverb in a real chain.

So the send stage is now permanently pinned to its own base "texture"
values and no longer reads the DRIVE knob at all (`verbSendDriveGain`/
`verbSendAsymmetry` are gone), and the return stage carries the whole
control, ceilings raised to take over the work the send used to share:
`verbReturnDriveGainMax` 5.0 -> 11.0, `verbReturnAsymmetryMax` 0.24 ->
0.30.

**2. It engaged far too late on the knob.** "Раньше достаточно было
прибавить на 20-30 процентов чтобы услышать эффект, а теперь надо крутить
до 60-70." `verbDriveLerp` used `verbSmoothstep`, which is exactly the
wrong shape for a drive control: it returns **0.156 at t=0.25** and 0.5
only at t=0.5, i.e. it deliberately withholds the first half of the knob -
the identical back-loaded-curve defect the PREAMP/SAT "drive-curve
correction" round already diagnosed. Replaced with `pow(t, 0.6)`:

| DRIVE knob | Old (smoothstep) | New (pow 0.6) |
|---|---|---|
| 10% | 2.8% | **25.1%** |
| 25% | 15.6% | **43.5%** |
| 30% | 21.6% | **48.6%** |
| 50% | 50.0% | 66.0% |
| 100% | 100% | 100% |

DRIVE=0% is still an exact no-op (`pow(0, 0.6) == 0`). A new regression
test asserts the *shape* (fraction of the base->max range reached at a
given knob position) rather than any specific gain, so retuning the
ceilings later cannot silently reintroduce the defect.

**3. The envelope had an attack it was never supposed to have.** "Я тебя
не просил делать атаку у Drive, он должен быть сразу же, но с небольшим
хвостиком, то есть релиз у него даже не 2.2 секунды, а 0.8 секунду." The
previous round's 1s attack / 2.2s release was a misreading of "prolonged,
not short" as "builds in gradually" - and it was also the second half of
the "engages too late" complaint, since on short guitar licks the envelope
never approached its target before the note ended. Now 3ms attack (not
literally zero only to avoid a per-sample step in a gain that multiplies
audio; far below the ear's own level-integration time) and 0.8s release,
and it tracks the **tank's own output** rather than the send, matching the
redesign above.

**Measured**: wet-path THD at -18dBFS/1kHz, DRIVE 0% -> 100%:
**0.47% -> 7.68%** (a 16x increase). An untouched DRIVE through the full
chain at DEEP measures 0.71%. Both figures are lower than the previous
round's (1.92%/9.03%/1.68%) because the 16-line tank scales its output by
`sqrt(16)` rather than `sqrt(12)`, putting the return stage's waveshaper
at a slightly lower operating point for the same input - the *ratio*, the
thing that determines whether the knob does something audible, is
substantially better.

## DRIVE round 3: envelope removed, curve pushed further front-loaded

Round 2's fixes (tail-only routing, front-loaded `pow(t, 0.6)` curve,
3ms-attack/0.8s-release envelope) were reported back, after real listening,
as still not reacting the way the brief asks for. Per this round's explicit
instruction not to keep machinery that isn't earning its place:

**1. The dedicated drive envelope is removed entirely.** Round 2's
3ms-attack/0.8s-release envelope (tracking the tank's own output level,
gating how much of the return-stage gain-above-base was reached) added a
second, separate ramp on top of the knob's own parameter smoothing -
extra machinery whose presence was itself part of what made DRIVE feel
indirect. `driveNormalised01` now reaches `verbReturnDriveGain()` through
nothing but `driveSmoother`'s existing ~30ms linear ramp, the same
smoothing every other macro in this module already gets - DRIVE now
engages together with the wet tail, full stop, with no separate envelope
in between. `breakup` (the tail's own drive rising as it decays relative
to its recent peak - a real, distinct analog-plate character, not a gate
on whether DRIVE is active) is unaffected and still fully composed.

**2. The curve is pushed further front-loaded.** Round 2's exponent (0.6)
still measured as insufficiently early-reacting: `pow(0.25,0.6)=0.435`.
Lowered to 0.42: `pow(0.2,0.42)=0.520`, `pow(0.25,0.42)=0.564`,
`pow(0.3,0.42)=0.603` - a fifth of knob travel now delivers half the
available range. DRIVE=0% remains an exact no-op (`pow(0,0.42)==0`) at
every exponent, by construction. The same exponent also drives the
warmth-shelf/depth/chorus curve (`driveCurve` in `VerbProcessor.cpp`), so
DRIVE's whole character now fronts up together, not just the raw gain.

**Measured**: wet-path THD at -18dBFS/1kHz, DRIVE 0% -> 100%:

| | DRIVE=0% | DRIVE=100% |
|---|---|---|
| Wet-path THD (isolated, -18dBFS/1kHz) | 0.47% | 12.46% |
| Full chain at DEEP (untouched DRIVE) | 0.71% | - |

A clearly audible, ~27x increase from a genuinely quiet baseline to a hot,
obviously-driven plate - both figures confirmed by a real build+test run.

## DRIVE warmth (live-testing feedback: deeper, not clipping)

Direct feedback: "звук... не стескающийся перегруз а более дорогой и
глубокий" (not a clipping overdrive but a more expensive/deeper one). A
plain symmetric-bandwidth tanh drives low and high content into the
nonlinearity equally, which reads as "cheap clipping" rather than a
deliberately voiced saturator.

**Mechanism** (`VerbProcessor.cpp`/`VerbCurves.h`): a low-shelf boost
(`returnWarmthShelfL`/`R`, 400Hz corner, up to +6dB at full DRIVE) ahead
of the return-stage tanh, biasing which content actually reaches the
nonlinearity's curved region toward low-mid material - the same
pre-emphasis-around-a-nonlinearity technique PREAMP's own transformer
coloration and SAT's frequency tilt already use elsewhere in this plugin
(own tuned constants here, not a shared refactor - same precedent SAT's
own waveshaper already established against PREAMP's). Deliberately *not*
undone by a matched post-cut - the resulting subtly richer low-end
presence at high DRIVE is itself part of "deeper", not a side effect to
hide; the existing `verbReturnBandwidthHz` ceiling (7200Hz) already tames
the top end, which is what keeps this reading as "warmer" rather than
simply "bassier". Scaled by the raw DRIVE knob position via
`verbSmoothstep` (not swell-gated - this is a tonal voicing, not a
level-triggered dynamic effect). At `driveNormalised01==0%` the shelf gain
is exactly 0dB, collapsing to an identity filter (`Biquad.h`'s
`makeLowShelf`), so this is a true no-op at rest - reconfirmed by the
existing "Full processor: a completely untouched DRIVE still measures
VERB's original small THD" test, which remains green.

## Driven-tail placement: depth and width

Direct feedback on how the overdriven tail should sit: "сделай чтобы этот
перегруз клиппинг был не в лицо, а вдаль вглубь, но не сильно в моно, а
пошире, возможно немного с эффектом chorus."

Two mechanisms, both scaled by the same front-loaded DRIVE curve so both
are exact no-ops at DRIVE=0%:

**Depth** (`verbDriveDepthBandwidthMinHz = 2600`) - a second bandwidth
ceiling on the driven return signal, on top of the fixed
`verbReturnBandwidthHz`, walking down from 7200Hz toward 2600Hz as DRIVE
rises. Distortion generates its own high harmonics, and high-frequency
content is the strongest distance cue the ear uses: the same signal
rolled off reads as further away. Rolling the ceiling down *as DRIVE
rises* is what keeps a hot setting reading as "deep" rather than "harsh" -
the added harmonics never get to sound forward. At DRIVE=0% its cutoff
equals `verbReturnBandwidthHz` exactly, i.e. it is a second pass of an
already-applied ceiling and changes essentially nothing.

**Width + chorus** (`verbDriveChorusBaseMs = 11`, `DepthMs = 3.2`,
`RateHz = 0.31`, `MixMax = 0.7`) - a short modulated delay on the driven
signal with the LFO applied in **opposite polarity per channel**, so the
two channels' delay times move against each other. This serves both
requests from one mechanism: it counteracts the image being pulled toward
the centre (a shared waveshaper applied to two correlated channels always
increases their correlation, which is why a driven tail tends to collapse
toward mono), and it supplies the requested chorus shimmer. Deliberately
**outside the FDN feedback loop** - it only ever reads the tank's output,
never anything that recirculates, so unlike the in-loop
`verbChorusDepthSamples` it cannot affect RT60 or loop stability. At
DRIVE=0% the mix is exactly 0 and the whole stage collapses to the
unmodulated signal.

## DRIVE routing and the null test

The actual signal flow (`VerbProcessor::process()`, unchanged in shape
since the round-2 redesign, reconfirmed here with a dedicated test):

```
DI/DRY -------------------------------------------------------> MIX
INPUT
  -> wet send HPF -> analog SEND stage (fixed, NEVER reads DRIVE)
  -> diffuser -> pre-delay -> FDN tank (recirculation NEVER reads DRIVE)
  -> analog RETURN stage (DRIVE-scaled HERE, and only here)
  -> warmth shelf / depth lowpass / width-chorus (all DRIVE-scaled, tail only)
  -> wet output HPF
  -> MIX (dry*(1-wet) + wetProcessed*wet)
```

`verbSendDriveGain`/`verbSendAsymmetry` in `VerbProcessor.cpp` are
`constexpr` aliases for the *base* constants - there is no code path left
that reads `driveNormalised01` before the tank. DRIVE only ever reaches
`verbReturnDriveGain`/`verbReturnAsymmetry` (the return stage) and the
warmth/depth/chorus block that follows it.

**Null test** (`Tests/PluginTests.cpp`'s "DRIVE null test" pair):

Both pass on a real build+test run:

- **"DRIVE null test: with reverb (wet amount) at 0%, output is
  bit-identical across verbDrive 0%->100%"** - a correlated-chord source
  run twice (DRIVE=0% and DRIVE=100%) with `reverb` (wet) pinned at 0%:
  `maxAbsDiff < 1.0e-7` across the whole buffer, both channels - the
  output is bit-identical regardless of DRIVE, confirming `verbWetGain(0)
  == 0.0` really does gate DRIVE's entire effect to exactly zero at 0%
  wet, not just approximately.
- **"DRIVE null test: the dry component itself never moves with DRIVE, at
  any wet amount"** - at `reverb=50%` (a real, nonzero wet setting) and
  DRIVE 0%/50%/100%, the earliest 5ms of output (before the diffuser/
  pre-delay lets any wet content through at all) measures `maxAbsDiff <
  1.0e-3` against a DRIVE=0% reference at every DRIVE value - the DI
  attack itself is unaffected by DRIVE even when the module is actively
  wet.

## Known limitations

**Superseded note (2026-09-14):** the metallic-ring item directly below
is exactly the problem the "Direction change" section at the top of this
document retires the plate architecture over, rather than continuing to
chase on the existing tank - do not treat the "Recommended next step"
below as the active plan.

- **The metallic-ring/resonance character is still present - open, not
  fixed.** See "Metallic-ring root-cause investigation (round 3)" above:
  the mechanism is understood (a static FDN's resonant modes are fixed
  regardless of line count/matrix quality; only genuinely time-varying
  delay lengths fix this), and the standard technique for it (a
  magnitude-flat allpass fractional-delay interpolator, letting
  modulation depth increase without RT60 cost) was attempted, but the
  implementation tried this round measured a real RT60 regression instead
  of the expected improvement and was reverted rather than shipped. The
  tank is back to its exact pre-round-3, known-good state (2-tap linear
  interpolation, 0.4-sample modulation depth). **Recommended next step**:
  before touching the FDN tank again, verify any replacement interpolator
  in isolation first (feed it a known test signal directly, outside the
  feedback loop, and measure its magnitude response against a reference)
  rather than only measuring the end-to-end RT60/resonance-sweep result,
  which is what let this round's bug ship past its own test suite until
  the dedicated RT60 regression test caught it.
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
