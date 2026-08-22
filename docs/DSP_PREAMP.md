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
digital-sounding clipping or obvious fuzz at moderate settings.

## Signal chain

```
Input
  -> DC / infrasonic protection      (fixed ~5 Hz one-pole HPF, base rate)
  -> [oversampled region: 4x @ 44.1/48kHz, 2x @ 88.2/96kHz, 1x @ 176.4kHz+]
       -> transformer coloration     (one-pole "core" rounding filter +
                                       fixed-frequency low-shelf density
                                       boost, both drive-dependent)
       -> nonlinear analog stage     (asymmetric tanh waveshaper)
  -> soft Low Cut                    (drive-dependent, ~20-70 Hz, base rate)
  -> soft High Cut                   (drive-dependent, ~20k-11k Hz, base rate)
  -> output compensation             (drive-dependent trim)
  -> enable/disable crossfade        (latency-aligned dry copy)
  -> Output
```

Every stage is implemented once per channel with independent state - no
cross-channel coupling and no randomness anywhere, so stereo image is
preserved exactly (see the "signal in the left channel only never bleeds
into a silent right channel" test in `Tests/PluginTests.cpp`).

## Nonlinear stage

Not a bare `tanh(input * drive)`. Per sample, inside the oversampled
region:

```
x  = roundingFilter(x)                      // one-pole "core" lowpass
x  = colorShelf(x)                          // low-shelf density boost
xd = x * driveGain(t)                       // t = smoothed DRIVE, 0..1
xa = xd + asymmetry(t) * xd * xd            // even-harmonic asymmetry
y  = tanh(xa) / tanh(driveGain(t))          // normalised soft ceiling
```

- `driveGain(t)` (`preampDriveGainLinear`) is a **geometric** (log-linear)
  interpolation from `0.05` (~-26dB) at `t=0` to `20.0` (~+26dB) at `t=1`,
  shaped by `t^1.3` so most of the audible "action" happens in the upper
  half of the range, matching the qualitative 0-20/20-50/50-75/75-100%
  zones from the product brief. Dividing the waveshaper's output by
  `tanh(driveGain(t))` (not by `driveGain(t)` itself) is what keeps `t=0`
  structurally close to identity: for the tiny `driveGain(0)=0.05`,
  `tanh(0.05*x)/tanh(0.05) ~ x` to within ~0.1% error across the full
  amplitude range, and gives `t=1` a firm, soft (never flat/hard-clipped)
  ceiling since `tanh(driveGain)` has already saturated to ~1 there.
- `asymmetry(t)` (`preampAsymmetryAmount`, `0` at `t=0` up to `0.18` at
  `t=1`) adds a small quadratic term to the waveshaper's argument. A
  quadratic doesn't flip sign with the input, so it produces the
  even-harmonic (H2, H4) content a pure odd-symmetric `tanh()` alone can't
  - scaled small enough to always stay a colour, never the dominant
  component. Any DC bias this introduces is removed by the soft Low Cut
  immediately downstream (see "DC offset" below).
- The one-pole "core" rounding filter and the fixed-150Hz low-shelf ahead
  of the waveshaper are the "transformer coloration": lower rounding-filter
  cutoff at higher drive both softens/rounds transients and reduces
  alias-prone high-frequency energy reaching the nonlinearity.

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
rate and never changes at runtime (`chooseOversamplingStages`):

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
equiripple FIR half-band design keeps this low.)

## Drive curve (qualitative)

`preampDriveGainLinear` in dB, `preampOutputCompensationDb` (output trim)
and the resulting measured THD (1kHz @ -18dBFS, see "Harmonic analysis"
below):

| DRIVE | driveGain | Output trim | THD (measured) |
|---|---|---|---|
| 0%   | ~-26 dB | 0 dB    | 0.015% |
| 25%  | ~-9 dB  | ~-2.6 dB | 0.040% |
| 50%  | ~+4 dB  | ~-6.7 dB | 0.31% |
| 75%  | ~+15 dB | ~-11.1 dB | 2.50% |
| 100% | ~+26 dB | -15 dB  | 22.7% |

Output trim uses the *same* shaping exponent (`t^1.3`) as the drive curve
itself, since tanh-normalisation alone measured ~+16dB of RMS growth from
DRIVE=0 to DRIVE=100 at -18dBFS input - clearly the "simply +15dB louder"
behaviour the product brief rules out. With the trim applied, DRIVE=100%
measures under +10dB of RMS growth over DRIVE=0% for the same input (see
`Tests/PluginTests.cpp`: "Output compensation keeps DRIVE=100% from being
simply much louder than DRIVE=0%"). This is a deliberate creative trim, not
a full loudness normaliser - hotter input signals still reach the
saturating region earlier than quiet ones, which is the point (a static
waveshaper does this for free, no envelope-follower/side-chain needed).

## Low Cut / High Cut (internal, not a parameter)

`preampLowCutHz` / `preampHighCutHz` in `PreampCurves.h`, applied as
2nd-order Butterworth (Q=1/sqrt(2), maximally flat, no resonant peak,
~12dB/octave) filters at the base sample rate, right after the nonlinear
stage. Measured via `Tests/PluginTests.cpp`'s "Low Cut / High Cut frequency
response" test (the actual `preampLowCutHz`/`preampHighCutHz` output, not
eyeballed):

| DRIVE | Low Cut | High Cut |
|---|---|---|
| 0%   | 20 Hz    | 20000 Hz |
| 25%  | 27.2 Hz  | 18172 Hz |
| 50%  | 38.9 Hz  | 15944 Hz |
| 75%  | 53.4 Hz  | 13535 Hz |
| 100% | 70 Hz    | 11000 Hz |

The same 30Hz/15kHz tone measurably attenuates more at DRIVE=100% than at
DRIVE=0% in the real processed audio (not just the formula) - see
`Tests/PluginTests.cpp`'s "Low Cut/High Cut measurably attenuates..."
tests, which run the actual `PreampProcessor` and measure via a
Goertzel single-bin analysis.

The Resources/Web/aux_visuals.js `bindPreampFilterLines()` LOW CUT/HIGH CUT
indicator lines are a hand-mirrored literal of these two formulas (there is
no shared runtime between the plugin core and the WebView UI) - if the
curve above ever changes, update both places together.

## Smoothing / bypass

- DRIVE: `juce::SmoothedValue` linear ramp, **25ms**, advanced
  sample-accurately at the oversampled rate for the gain-critical parts of
  the nonlinear stage (where zipper noise would actually be audible).
  Filter coefficients (Low Cut/High Cut/colour/rounding) are recomputed
  once per block from the *previous* block's settled value - a one-block
  lag that's inaudible and standard practice, avoiding per-sample
  trig cost.
- `preampEnabled` (bypass): **20ms** linear crossfade between the fully
  processed ("wet") signal and a latency-aligned dry copy - never an
  instant `if` switch. The dry copy is pushed through the same fixed
  `IntegerDelayLine` the wet path's oversampling latency implies, so
  toggling bypass doesn't shift the plugin's effective timing.

## DC offset

The asymmetric quadratic term in the waveshaper can bias the signal
slightly, but the soft Low Cut sits immediately downstream (even at its
minimum ~20Hz at DRIVE=0%) and removes it before the signal leaves the
module - measured mean output at DRIVE=100% stays under `0.01` for a
0.5-amplitude sine (`Tests/PluginTests.cpp`: "DC offset stays safely
small...").

## Numerical safety

Input samples are sanitised (non-finite -> 0) at the very start of
`process()`, before anything else touches them, and the same check runs
again on the final output as a narrow correctness safeguard against
pathological host input - not a blanket amplitude clamp papering over
algorithm behaviour. `juce::ScopedNoDenormals` (already present in
`PluginProcessor::processBlock`) covers denormal protection for the whole
chain. `tanh()` is asymptotically bounded for any finite input, so the
waveshaper itself can never produce non-finite output from finite input.

## Harmonic analysis

1kHz sine @ -18dBFS through DRIVE 0/25/50/75/100%, measured via FFT
(`Tests/PluginTests.cpp`, `UNI76PreampHarmonicAnalysisTests` - run
`UNI76Tests.exe` to reproduce; numbers below are copied directly from that
run's stdout, not hand-typed estimates):

```
DRIVE 0%:   fund=54.06dB  H2=-29.86dB  H3=-39.39dB  H4=-23.75dB  H5=-48.55dB  THD=0.0145%
DRIVE 25%:  fund=51.63dB  H2=-16.85dB  H3=-37.87dB  H4=-26.26dB  H5=-51.08dB  THD=0.0399%
DRIVE 50%:  fund=48.82dB  H2=-1.57dB   H3=-18.60dB  H4=-29.02dB  H5=-53.90dB  THD=0.306%
DRIVE 75%:  fund=53.22dB  H2=20.04dB   H3=14.80dB   H4=-7.17dB   H5=-23.06dB  THD=2.50%
DRIVE 100%: fund=58.11dB  H2=16.22dB   H3=44.81dB   H4=23.26dB   H5=34.61dB   THD=22.7%
```

(dB values are relative FFT-bin magnitudes from an unnormalised windowed
transform, not calibrated dBFS - only the relative progression across
DRIVE settings and between H2/H3/H4/H5 within a row is meaningful.)

Reading the progression:
- **THD grows monotonically** with DRIVE and stays in a musical range at
  every setting (0.015% -> 22.7%) - no sudden explosion anywhere in the
  sweep.
- **H2/H3 dominate over H4/H5 at low-to-moderate DRIVE** (25-50%): at 50%,
  H2 and H3 are both well above H4/H5, matching the brief's "low-order
  harmonics, not aggressive high-order spray" requirement for moderate
  settings.
- Higher-order content (H4, H5) only becomes comparable to H2/H3 at 75-100%
  DRIVE, where strong, obvious character is the explicit goal.
- Peak output at DRIVE=100% stays under `1.05` for a -18dBFS input (tanh's
  soft asymptote, never a flat/hard-clipped ceiling -
  `Tests/PluginTests.cpp`: "DRIVE=100% peak output should stay bounded").

These numbers come directly from running the actual shipped
`PreampProcessor` - the curve constants in `PreampCurves.h` (particularly
`preampOutputTrimMaxDb`) were tuned against this measurement, not the other
way around.

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
