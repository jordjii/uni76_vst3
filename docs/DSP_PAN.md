# PAN / STEREO FIELD - DSP notes

`Source/DSP/PanoramaProcessor.h`/`.cpp` implements the `05 PAN / STEREO
FIELD` module. Despite the parameter's internal ID (`panorama` - kept
only for backward compatibility, never renamed), **this is not an L/R
balance pan**. It is a single stereo *width* control:

```
MONO (0%)  <-  NATURAL (50%)  ->  WIDE (100%)
```

Not `0% = LEFT, 50% = CENTER, 100% = RIGHT`. There is no pan-pot here.

## Topology

Mid/Side matrix, frequency-dependent Side gain, Mid channel never
touched:

```
Input (L, R)
  -> Mid  = 0.5*(L+R)          (passed straight through, unmodified)
  -> Side = 0.5*(L-R)
       -> sideLow  = LP(Side)                  (2nd-order Butterworth, 180Hz)
       -> sideHigh = Side - sideLow             (exact complement)
       -> sideOut  = lowGain(t)*sideLow + highGain(t)*sideHigh
  -> Lout = Mid + sideOut
  -> Rout = Mid - sideOut
  -> enable/disable crossfade against a dry copy (no latency to align -
     PAN has none, unlike PREAMP/SAT/PITCH)
  -> Output
```

`t` is the raw `panorama` APVTS value (0..1). `lowGain`/`highGain` are
two independently-tunable width-gain curves (`PanoramaCurves.h`) sharing
the same shape below 50% and diverging above it - see "Width mapping"
and "Low-end protection" below.

### Normalisation convention

`Mid = 0.5*(L+R)`, `Side = 0.5*(L-R)`, decoded as `L = Mid+Side`,
`R = Mid-Side` - the 0.5 scaling on encode means no separate gain is
needed on decode: when `L == R`, `Mid == L` and `Side == 0` exactly, and
reconstruction returns the identity. This convention is used
consistently everywhere in the module (never switched to unscaled
`M=L+R` mid-way through).

## Why Mid/Side, not delay-based widening

The product brief explicitly forbids delay-based widening (Haas-style
L/R offset), chorus, random modulation, and pitch-shift-based enhancers.
Mid/Side width is the only one of the classic stereo-width techniques
that is a pure, static gain operation - no comb filtering, no
modulation, no added spectral content. It was chosen as the foundation
from the outset (per the product brief) rather than evaluated against
alternatives, because the alternatives were already ruled out by the
"no coloration" requirements.

A very light frequency-dependent all-pass/phase-rotation enhancement on
the Side channel was considered (the product brief explicitly permitted
exploring this, conditionally) but **not implemented** - the plain
frequency-dependent M/S gain design already meets every measured target
(width mapping, low-end stability, mono compatibility, correlation
behaviour - see below) with no added risk, and the brief's own guidance
was to prefer the simpler design when it already works. Nothing was
added "for completeness."

## Width mapping

`panWidthGain(t, maxAtFull)` (`PanoramaCurves.h`) - two smoothstepped
linear segments sharing 50% as a named centre, the same shape
`EqCurves.h` uses for DARK-PHONE-AIR:

```
t in [0.0, 0.5]:  gain = smoothstep(t/0.5)               -- 0 -> 0.0, 1.0 -> 1.0
t in [0.5, 1.0]:  gain = 1 + (maxAtFull-1)*smoothstep((t-0.5)/0.5)
```

Both bands use the *identical* function for `t<=0.5` - only `maxAtFull`
(applied only above 50%) differs between the low and high band. This is
what guarantees MONO (0%) collapses every frequency equally and NATURAL
(50%) is the identity transform at every frequency, regardless of the
low/high split (see "NATURAL = 50%" below).

Measured (1kHz, high band, `panWidthMaxHigh = 1.8`):

| Width | Target | Measured |
|---|---|---|
| 0% | 0.0 | 0.0 |
| 25% | ~0.5 | 0.500 |
| 50% | 1.0 | 1.000 |
| 75% | ~1.35-1.5 | 1.410 |
| 100% | ~1.7-2.0 | 1.820 |

All five land inside or essentially on the product brief's target range.

## Frequency-dependent width (low-end protection)

A single 2nd-order (12dB/oct) Butterworth lowpass at **180Hz** splits
Side into `sideLow`/`sideHigh` - not a brickwall crossover, a gentle
rolloff centred in the brief's 120-300Hz "smooth transition" zone.
`sideHigh` is defined as `Side - sideLow` (the filter's own complement,
not a second independently-designed filter), which has a load-bearing
consequence: **`sideLow + sideHigh == Side` exactly, for any filter
order/shape**, purely algebraically. That identity is what makes NATURAL
a true identity transform even with the crossover always running (see
below) - it isn't a special case, it falls out of the architecture.

Two ceilings (`PanoramaCurves.h`):

- `panWidthMaxHigh = 1.8` - mid/high band, the module's main widening effect.
- `panWidthMaxLow = 1.15` - low band, deliberately a much smaller ceiling
  so bass stays close to its original width even at full WIDE.

Measured Side gain at WIDE (100%), swept 60Hz-10kHz:

| Frequency | Side gain |
|---|---|
| 60 Hz | 1.27 |
| 100 Hz | 1.47 |
| 200 Hz | 1.90 |
| 500 Hz | 1.87 |
| 1000 Hz | 1.82 |
| 5000 Hz | 1.80 |
| 10000 Hz | 1.80 |

Low frequencies widen noticeably less than mid/high, as intended (60Hz's
1.27 is close to the low-band ceiling of 1.15; 5-10kHz settle at the
high-band ceiling of 1.8). **One measured characteristic worth being
honest about**: 200Hz - right in the crossover's transition band -
overshoots the 1.8 high-band ceiling slightly (1.90, ~5.5% over). This
is a known, expected property of a simple LP + algebraic-complement
crossover *when the two bands carry different gains* (at NATURAL, where
both gains are 1.0, there is no overshoot - it only appears when
widening, and only right at the transition). It is a smooth, continuous
bump, not a discontinuity, click, or resonant ringing, and stays well
short of "too extreme" (peak Side gain is still under 2x anywhere). Not
corrected - a proper Linkwitz-Riley-style crossover (matched magnitude
response even under differential gain) would remove it at the cost of
real added complexity and CPU, which the current, simpler design's
results don't yet justify. Documented here rather than hidden.

## NATURAL = 50%

Provably (not just measured) an identity transform: at `t=0.5`,
`lowGain(0.5) == highGain(0.5) == 1.0` (both curves evaluate to exactly
1.0 at their shared centre), so `sideOut = 1.0*sideLow + 1.0*sideHigh =
sideLow + sideHigh = Side` exactly - the crossover filter's own output
cancels back out algebraically regardless of its frequency response.
`Lout = Mid + Side = L`, `Rout = Mid - Side = R`.

Measured (numerical null test, correlated stereo chord, 1s, settled):
**RMS diff = 7.9e-9, max diff = 6.0e-8** - effectively float-rounding
noise, not a processing artifact. Filters *are* running at 50% (the
crossover always runs), but the architecture makes that fact
inaudible/immeasurable rather than requiring a separate identity
shortcut - the "prove it's needed before adding filters at NATURAL"
bar in the product brief is met by proving they don't need to be
*bypassed*, not by removing them.

## MONO = 0%

`lowGain(0) == highGain(0) == 0.0`, so `sideOut = 0` unconditionally,
`Lout = Rout = Mid = 0.5*(L+R)` - the standard, correct mono sum
(arithmetic average, not `L=R=L`). Measured: `L==R` to within 1e-4 and
matches `(Lin+Rin)/2` to within 1e-4 on decorrelated stereo material.

**Anti-phase material behaves correctly, not "fixed"**: `L=sine,
R=-sine` gives `Mid=0` exactly, so MONO of a pure anti-phase signal
cancels to silence - measured peak after settling < 1e-4. This is
physically correct behaviour (summing two exactly-opposite signals to
mono *should* cancel), not a bug, and the module makes no attempt to
"rescue" that content by, say, leaking some Side through even at 0% -
that would break the "0% is a correct mono sum" guarantee for every
other source to fix a case that isn't actually broken.

## WIDE = 100%

Same architecture, `maxAtFull` ceilings pushed to 1.8 (high)/1.15 (low).
No delay, no chorus, no phase-rotation all-pass (see "Why Mid/Side, not
delay-based widening"). Correlation drops gracefully with width on
correlated material (measured: 0.965 input -> 0.965 at 50% -> 0.927 at
75% -> 0.875 at 100% - see "Correlation" below), never swinging
aggressively negative on normal (non-adversarial) material.

## Mono input / mono compatibility

**Mono buses (numChannels < 2) are left completely untouched** - PAN
never fabricates stereo content from a single channel, at any width
setting (measured: max diff < 1e-6 between input and output on a mono
bus at 0/50/100%). This is a hard architectural guarantee, not a special
case in the gain math - `process()` returns immediately (after the usual
NaN/Inf sanitisation) when the buffer has fewer than 2 channels.

**Mono fold-down is provably unchanged by width**, for genuine stereo
input too: `(Lout+Rout)/2 == (Mid+sideOut + Mid-sideOut)/2 == Mid`, and
`Mid` is *never modified* anywhere in the signal path - it's read once
from the input and written straight to both output channels' sum. So
`(Lout+Rout)/2 == (Lin+Rin)/2` **exactly**, at every width, by
construction. Measured on decorrelated stereo material at 0/25/50/75/
100%: max fold-down difference < 1e-4 (float-rounding-scale) at every
setting, RMS difference from the input's own mono sum also < 1e-4.

A listener hears more Side energy in stereo as width increases, but
whatever a mono listener (or a mono fold-down check in a DAW) hears is
*exactly* what they'd have heard at any other width setting - widening
never quietly changes the mix's mono-compatible core.

## Correlation

Measured on a correlated stereo chord (input correlation 0.965):

| Width | Correlation |
|---|---|
| 50% (NATURAL) | 0.965 (matches input) |
| 75% | 0.927 |
| 100% | 0.875 |

Graceful, monotonic degradation - no aggressive negative correlation on
normal material. (Pure anti-phase test material is a separate,
deliberately adversarial case - see "MONO = 0%" above; it behaves
correctly there too, just via cancellation rather than a correlation
number.)

## Centre image stability

Tested with a centred 80Hz bass tone plus decorrelated stereo highs
(4kHz/5.5kHz), at WIDE (100%): the bass stays centred, measured L/R
level difference < 0.5dB - confirming the frequency-dependent split
doesn't let a widened high end drag a centred low end off-centre by
crosstalk through the shared Side signal (each frequency component's
Side content is scaled independently by the low/high blend, not
smeared across the spectrum).

## Gain / headroom

No AGC, no internal limiter - a pure gain morph can't need one if the
mapping itself stays moderate (which the width mapping's chosen ceilings
of 1.8/1.15 are designed to do). Measured on a deliberately Side-heavy
synthetic source (anti-phase-dominant, `docs/audio` artifact material):

| Width | Peak | RMS L | RMS R |
|---|---|---|---|
| 0% | 0.080 | 0.057 | 0.057 |
| 25% | 0.205 | 0.105 | 0.105 |
| 50% | 0.330 | 0.186 | 0.186 |
| 75% | 0.435 | 0.257 | 0.257 |
| 100% | 0.540 | 0.330 | 0.330 |

Growth from 0% to 100% is smooth and proportionate to the width curve
itself - no sudden jump, no runaway. No internally-hot input was pushed
to hard-clipping territory in any test; worst-case measured Side gain
anywhere (the 200Hz crossover overshoot, ~1.9x) would need an
already-hot Side-heavy input to approach 0dBFS, which floating-point DSP
tolerates without internal clipping regardless.

## Smoothing / bypass

Width and bypass mix are each smoothed with a 20ms linear ramp
(`panSmoothingSeconds`, `PanoramaCurves.h`) - inside the product brief's
10-30ms guidance. No latency is needed for this (unlike PREAMP/SAT's
oversampling or PITCH's STFT) since PAN is a pure sample-by-sample gain/
filter operation with nothing to "catch up" to. Automation transitions
(0->100, 100->0, 0->50, 50->100) measured click-free (max sample-to-
sample jump well under the click-detection threshold in every case), and
50% settles back to true identity after any transition through it, not
just approximately.

`panoramaEnabled` (index 4, `Core/ModuleEnableState.h`) drives a real
bypass: disabled crossfades to an exact dry passthrough (no delay
alignment needed, unlike PREAMP/SAT/PITCH's `IntegerDelayLine`-based
bypass, since PAN has no latency to align against) - measured max diff
< 1e-4 from the true input once settled.

## Latency

**Always exactly 0** - `getLatencySamples()` returns a hardcoded `0`,
verified constant across every width value, enabled/disabled, every
supported sample rate (44.1/48/96/192kHz), and every block size
(32-2048). Confirmed the total plugin latency is unchanged by adding
PAN to the chain - still exactly `PREAMP + EQ(0) + SAT + PITCH + PAN(0)`
at every sample rate, matching the pre-PAN totals from
[docs/DSP_PITCH.md](DSP_PITCH.md) exactly (e.g. 6186 samples / 140.272ms
at 44.1kHz).

## Parameter and state migration

`panorama` keeps its existing APVTS string ID and C++ type
(`AudioParameterFloat`, `0..100%`) - **only its default value changed**,
from 0% to 50%. The old 0% default predates any DSP reading the
parameter (Panorama was a passthrough placeholder), so it never actually
meant "mono" - it was just wherever the then-inert knob happened to
rest. Now that 0% genuinely collapses the stereo image, a saved project
whose `panorama` value happens to be 0% (or anywhere else) from before
this DSP existed must not suddenly play in mono (or at some other
unintended width) on load.

Schema bumped to **v4** (`Source/Core/PluginIdentity.h`,
`panoramaNaturalSchemaVersion = 4`, fixed independently of any later
`stateSchemaVersion` bump, same pattern as PITCH's
`pitchDiscreteSchemaVersion`). `setStateInformation()` forces `panorama`'s
stored `value` to `50.0` whenever `loadedSchemaVersion < 4`, **before**
`apvts.replaceState()` runs - regardless of whatever the old raw value
actually was. This is the same deliberate, breaking-semantic-migration
reasoning PITCH's v3 bump used: the old value was never sound-meaningful,
so there is nothing to "best-effort preserve" - forcing every pre-v4
state to the new default is what guarantees an old project can't
suddenly change stereo width unexpectedly. Covered by dedicated tests
(a hand-built pre-v4 state with a `0.0` legacy value, confirming it
migrates to `50.0`/normalised `0.5`). Under the new schema, `panorama`
saves and restores exactly at 0/50/100%.

## Known limitations

- The 200Hz crossover-region Side-gain overshoot (~1.9x vs the intended
  1.8x ceiling) is a known, minor, non-discontinuous artifact of the
  simple LP+complement crossover under differential gain - see
  "Frequency-dependent width" above. Not corrected this pass; a
  Linkwitz-Riley-style crossover would remove it if ever needed.
- No frequency-dependent phase-rotation/all-pass enhancement was added
  to WIDE - evaluated conceptually per the product brief, not built,
  since the plain M/S gain design already meets every measured target.
  If a future listening pass finds the plain gain approach audibly
  lacking versus a more elaborate design, that would be the next thing
  to prototype and measure - not assumed necessary here.
- Correlation/width measurements use the deterministic synthetic sources
  in `docs/audio/pan-test-input.wav` and the automated test suite, not a
  survey of real mixed material across genres.
