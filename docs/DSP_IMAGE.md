# IMAGE / STEREO IMAGE - DSP notes

`Source/DSP/ImagerProcessor.h`/`.cpp` implements the `07 IMAGE / STEREO
IMAGE` module - the seventh and final real DSP in the plugin (after
PREAMP, EQ, SAT, PITCH, PAN and VERB). See CLAUDE.md.

IMAGE is a **deliberate, explicit exception** to the project's "one knob
per module" rule (see CLAUDE.md): it carries two independent public
APVTS parameters, not one.

```
imager (0%..100%):      ORIGINAL -> FOCUS -> WIDE
                         Frequency-dependent stereo width/imaging amount.

imageTilt (-100..+100):  LEFT <- CENTER -> RIGHT
                         A static (time-invariant) stereo image balance/
                         tilt - explicitly NOT a hard L/R pan.
```

Both axes are Mid/Side-domain and **orthogonal by construction**: `imager`
only ever modifies Side; `imageTilt` only ever modifies Mid. Mid/Side use
the same `0.5*(L+R)`/`0.5*(L-R)` convention as PAN and VERB.

## Topology

```
INPUT
  -> Mid = 0.5*(L+R), Side = 0.5*(L-R)
  -> IMAGE AMOUNT: single low-shelf on Side only (low-freq gain shrinks
     toward centre as the macro rises, high-freq gain grows - "bass
     gathers, highs widen")
  -> IMAGE TILT: two independent per-channel low-shelf filters on Mid
     only (low-freq asymptote always 1.0/no tilt, high-freq asymptote
     the full constant-power tilt gain for that channel)
  -> L' = filteredMidL + sideProcessed
     R' = filteredMidR - sideProcessed
  -> enable/disable crossfade against a dry copy (zero latency, no
     delay-alignment needed)
  -> Output
```

Every curve in `ImagerCurves.h` evaluates to its identity value at each
axis's own zero point: `imager(0)` gives width gain 1.0 at every
frequency (Side passes through the width shelf unmodified); `imageTilt(0)`
gives `gL == gR == 1.0` at every frequency (both tilt shelves collapse to
an exact 0dB identity filter, so Mid passes through unmodified too). This
is what makes all **three independent identity cases** below provable,
not just measured.

## Three independent identity cases

| IMAGE | TILT | Result |
|---|---|---|
| 0% | CENTER | **Provable bit-exact identity** - `L'=L`, `R'=R` |
| >0% | CENTER | Only imaging works - width shelf active, tilt shelves both collapse to identity |
| 0% | !=CENTER | Only tilt works - Side passes through the (identity) width shelf unmodified, tilt shelves reshape Mid |

Measured (`Tests/PluginTests.cpp`'s `UNI76ImagerProcessorTests`):
CENTER/0% null test RMS diff < 1e-5 on a 4-tone broadband source (same
technique as PAN's/VERB's own provable-identity tests); IMAGE=100%/
TILT=0% keeps a symmetric mono-in-stereo source's L/R ratio within 2% (no
L/R bias from width alone); IMAGE=0%/TILT!=0% output matches a
closed-form `mid*gL+side` / `mid*gR-side` reconstruction (see "Gain law"
below) to within 1.07% relative error at a test frequency far above the
tilt safety shelf's own corner.

## IMAGE AMOUNT: frequency-dependent width

A **single** low-shelf filter (not per-channel - this axis is symmetric,
it never biases L vs R) reshapes Side only:

- Low-frequency asymptote: `imagerWidthMinLow = 0.25` at 100% (bass
  gathers toward 25% of its original width - low-end centering/mono-
  compatibility).
- High-frequency asymptote: `imagerWidthMaxHigh = 2.0` at 100% (highs
  widen up to 2x - real mastering-imager-style widen).
- Shelf corner: `imagerWidthCrossoverHz = 150Hz` (same order of magnitude
  as PAN's own bass-safety crossover, for a consistent "where does this
  plugin consider content to be bass" convention - not because the two
  filters are related).
- Slope: `imagerWidthShelfSlope = 1.0` (RBJ "S" - maximally flat, no
  overshoot, same proven-safe construction PAN's width/motion shelves
  use - see docs/DSP_PAN.md's "Crossover artifact" section for the full
  derivation of why a single monotonic shelf, not a band-split-then-sum,
  is what makes this safe by construction).

Mid is **never** touched by this axis - matching PAN's own "Mid is never
modified" philosophy and making mono-source safety trivial: a mono
source (identical L/R, Side exactly 0) measures Side RMS = 0 after IMAGE
alone at any macro value (`Tests/PluginTests.cpp`'s "IMAGE amount alone
does not stereoize a mono source" test) - IMAGE only ever *reshapes*
existing Side content, it never *synthesises* new stereo information the
way PAN's induced-signal mechanism deliberately does.

## IMAGE TILT: static L/R balance

A bounded, **constant-power** gain pair applied to Mid only (never Side),
framed as an angle so the algebra is exact rather than tuned:

```
theta = tiltNorm * thetaMax           (tiltNorm in [-1,1], thetaMax = 42 degrees)
gL    = sqrt(2) * cos(pi/4 + theta)
gR    = sqrt(2) * sin(pi/4 + theta)
```

`gL^2 + gR^2 == 2` **exactly**, for *any* theta - the same algebraic
constant-power proof PAN's own motion rotation uses (see
`PanoramaProcessor.cpp`). This is what keeps combined loudness stable
across the whole tilt range by construction, not just by tuning.

### Gain law (measured, `imageTiltGains()` direct)

| Tilt | gL | gL (dB) | gR | gR (dB) | gL²+gR² |
|---|---|---|---|---|---|
| -100% | 1.412 | +3.00dB | 0.0740 | -22.61dB | 2.000 |
| -50% | 1.292 | +2.22dB | 0.575 | -4.80dB | 2.000 |
| 0% (CENTER) | 1.000 | 0.00dB | 1.000 | 0.00dB | 2.000 |
| +50% | 0.575 | -4.80dB | 1.292 | +2.22dB | 2.000 |
| +100% | 0.0740 | -22.61dB | 1.412 | +3.00dB | 2.000 |

`gL^2+gR^2` measured at exactly 2.000 at every tested tilt value,
confirming the algebraic proof holds in the actual floating-point
implementation, not just on paper.

### Why `thetaMax = 42 degrees`, not 45

At the true 45-degree "vanish point," `cos(45+45) = 0` and one channel's
Mid contribution would drop to *exactly* zero. 42 degrees leaves the
quietest channel's Mid content at `~-22.6dB` - clearly attenuated, but
never literally silent - and critically, the quietest channel's *Side*
content is completely untouched (Side is never processed by tilt at
all - see "Topology"), so even a Mid-heavy (low-Side) source still keeps
some signal on the "far" channel. Measured (`Tests/PluginTests.cpp`'s
"opposite channel never disappears" test, TILT=+-100%, a broadband
3-tone source): quieter channel stays above 6% of the louder channel's
RMS (well above the 2% pass threshold), never reduced by more than
~24dB.

### Frequency-dependent bass safety

TILT must not drag sub/bass hard into one ear - that would undo IMAGE
AMOUNT's own low-end-centering philosophy. Rather than a literal
4-breakpoint table, this reuses the project's own proven-safe technique
(PAN's width/motion shelves, VERB's dual highpass isolation): **two
independent per-channel low-shelf filters**, each with low-frequency
asymptote fixed at exactly `1.0` (no tilt at all at deep bass) and
high-frequency asymptote the full `gL`/`gR` above. A literal band-split-
then-sum implementation of a hard breakpoint table is a proven source of
a real frequency-response bump (a vector sum of two differently-gained,
phase-shifted bands - see docs/DSP_PAN.md's "Crossover artifact" section
for the derivation); a single monotonic shelf has no second path to sum
against, so no bump is possible by construction. Corner:
`imageTiltCrossoverHz = 200Hz`, slope `imageTiltShelfSlope = 1.0` (same
maximally-flat RBJ shape as IMAGE AMOUNT's own shelf).

**Measured** (L/R dB, TILT=100%, single-tone bursts):

| Frequency | L/R |
|---|---|
| 40 Hz | -0.10dB |
| 60 Hz | -0.47dB |
| 80 Hz | -1.36dB |
| 100 Hz | -2.82dB |
| 120 Hz | -4.70dB |
| 150 Hz | -7.85dB |
| 200 Hz (corner) | -12.81dB |
| 300 Hz | -19.53dB |
| 500 Hz | -24.26dB |
| 1000 Hz | -25.52dB |
| 2000 Hz | -25.61dB |
| 4000 Hz | -25.61dB |

By 2-4kHz the response has fully converged to the theoretical
`gL/gR` ratio (-25.61dB, computed from the gain-law table above) -
confirming the shelf reaches its intended asymptote cleanly. Below 80Hz,
tilt is reduced to under 1.4dB - "very small" - even at full ±100%; the
150-300Hz range is a genuine, gradual transition (not a hard switch);
full tilt amount is reached by ~500Hz-1kHz. This is a smooth RBJ-shelf
approximation of the qualitative 4-zone description from the original
product sketch, not a literal breakpoint table - the smooth version is
what the project's own established anti-vector-sum-bump philosophy
requires (see docs/DSP_PAN.md).

## Symmetry

**Proved algebraically**: `imageTiltGains()`'s construction gives
`gL(theta) == gR(-theta)` for any theta (from `cos(A) = sin(90-A)`), so
`TILT(-x)` applied to `(L,R)` and `TILT(+x)` applied to the
*channel-swapped* input `(R,L)` must produce the same result with L and
R also swapped: `f(-x,L,R).L == f(+x,R,L).R` and
`f(-x,L,R).R == f(+x,R,L).L`. (Using the *same*, unswapped input for
both tilt directions does **not** hold in general once tilt is active -
Side's sign does not flip along with the tilt sign unless the input
itself is also mirrored; an earlier version of this project's own test
suite made exactly this mistake and was corrected - see
`Tests/PluginTests.cpp`'s "TILT symmetry" test.)

**Measured**: relative difference between the two sides of the identity
above, on a genuinely asymmetric stereo source, at x = 25/50/100%: all
three landed at floating-point-precision-level agreement (< 1e-4,
several orders of magnitude below the 2% acceptance bar) - the algebraic
proof holds exactly in the real implementation.

## Centroid mapping

Using the same `(Renergy-Lenergy)/(Renergy+Lenergy)` stereo-centroid
convention PAN's own test suite uses: for a static two-tone source swept
across TILT = -100/-50/-25/0/+25/+50/+100%, centroid moves **strictly
monotonically** Left -> Center -> Right, landing at exactly 0.0 at
TILT=0 and symmetric endpoints (`-0.993` at -100%, `+0.993` at +100%) -
never quite reaching ±1.0 (fully one-sided), consistent with the
never-fully-silent guarantee above.

## Loudness stability

Combined RMS (`L^2+R^2` over the whole -100..+100 sweep, dB relative to
CENTER) on a 3-tone broadband source:

| Tilt | Combined RMS delta vs CENTER |
|---|---|
| -100% | 0.00dB (reference) |
| -75% | +0.11dB |
| -50% | +0.22dB |
| +100% | ~0.00dB |

Peak deviation under 0.22dB across the entire range - the constant-power
gain-pair proof holds up in the full signal, not just for the Mid
component in isolation.

## Mono compatibility

- **Real mono bus** (`numChannels < 2`, e.g. the plugin instantiated
  mono->mono): both axes are **completely DSP-neutral** - measured
  relative difference from the unmodified input under 1e-5 at every
  IMAGE/TILT combination tested. There is no L/R to balance or widen in
  a true mono bus, so this is a genuine no-op, not an attempted stereo
  emulation.
- **Mono source inside a stereo bus** (identical L==R, a stereo bus fed
  mono-compatible material): IMAGE AMOUNT alone does **not** stereoize it
  (Side RMS measures exactly 0 after IMAGE=100%/TILT=0% - IMAGE only
  reshapes existing Side, it never synthesises new stereo content, unlike
  PAN's own induced-signal mechanism). IMAGE TILT alone **can** and does
  bias such a source - a mono source through TILT=+100% measures the R
  channel's RMS at more than 1.5x the L channel's - this is expected,
  intentional behaviour (TILT is fundamentally a channel-balance
  operation, and a mono source has no Side content standing in the way of
  that balance being fully audible), not a bug.
- **Mono fold-down**: `(L+R)/2` measured at IMAGE=100%/TILT=+-100% (the
  combined worst case) changes by -3.05dB relative to CENTER on a
  broadband 3-tone source - a real, disclosed reduction (from the
  quieter channel's own Mid content being heavily attenuated), but far
  from a catastrophic collapse.

## Correlation

On a mono-in-stereo (Side=0) 3-tone test source - correlation is
identical across every IMAGE value tested (0/50/100%) since IMAGE has no
observable effect on a source with zero real Side content by design (see
"Mono compatibility" above); the table below therefore isolates TILT's
own effect:

| Tilt | Correlation |
|---|---|
| -100% | 0.341 |
| -50% | 0.948 |
| 0% (CENTER) | 1.000 |
| +50% | 0.947 |
| +100% | 0.341 |

Correlation never goes negative anywhere in the tested IMAGE x TILT
matrix - "amplitude/spatial matrix, not a phase effect," matching the
product brief's own framing. The drop to 0.34 at full tilt reflects the
large, deliberate L/R gain imbalance TILT creates at its extremes (see
"Gain law"), not a phase-domain artifact.

## PAN interaction

IMAGE runs **after** PAN in the signal chain (`Input -> ... -> PAN ->
VERB -> IMAGE -> Output`), so TILT's static Mid-domain bias composes
with PAN's own audio-rate motion automatically, with no special-casing
needed - the two processors are entirely independent instances with no
shared state.

**Measured** (PAN=100%/MOTION + TILT, decorrelated stereo-highs source,
centroid series over a 10-second pass):

| Tilt | Centroid mean | Centroid excursion |
|---|---|---|
| 0% (CENTER) | -0.353 | 0.225 |
| -100% | -0.686 | 0.088 |
| +100% | +0.432 | 0.083 |

TILT clearly shifts the *average* bias of PAN's own motion trajectory
(item requirement: "TILT должен смещать среднюю ось движения" - TILT
should shift the average axis of movement) while motion continues
throughout (excursion never collapses to near-zero - it settles around
37-39% of the untilted value here, which is expected: TILT's own
Mid-domain gain imbalance shifts each channel's own energy baseline
enough to compress the *centroid ratio metric's* swing without the
underlying motion becoming inaudible in absolute terms). **PAN's LFO
period is unaffected by TILT**, confirmed via zero-crossing spacing on
the (mean-removed) centroid series: 147735 samples at CENTER, 146632 at
TILT=-100% (0.75% difference), 147735 at TILT=+100% (identical) - well
inside a defensible margin for a numerically-estimated period, and
architecturally guaranteed anyway since TILT never touches PAN's own LFO
phase/state (the two are separate class instances).

## No pitch change, no time modulation, no added latency

`getLatencySamples()` returns a hardcoded `0` - IMAGE is a pure gain/
filter morph with no oversampling, no lookahead, no delay-based widening
(no Haas). TILT is explicitly **static** - unlike PAN, there is no
internal LFO or any other time-varying state; its coefficients are
recomputed once per block purely from the (smoothed) macro parameter,
identical to EQ/SAT/VERB's own established one-block-lag convention.
Measured: a steady 1kHz tone's level through IMAGE=50%/TILT=50% changes
by 0.00255dB between an early and a late measurement window 2.5s apart -
consistent with a genuinely static (non-modulating) transfer function,
not the audible ripple a hidden LFO or drifting coefficient would leave.

## Smoothing / bypass

Both `imager` and `imageTilt` are smoothed with a 30ms linear ramp
(`imagerSmoothingSeconds`), matching VERB's own convention. `imagerEnabled`
(index 6, `Core/ModuleEnableState.h`) drives a single shared bypass
smoother for **both** axes - they are one module with two axes, not two
separately-bypassable modules, matching the "shared enable flag" scope
CLAUDE.md documents for every module. The bypass crossfade is the same
immediate dry/wet blend EQ/PAN use (no `IntegerDelayLine` needed, since
IMAGE adds no latency to align against).

## Latency

**Always exactly 0** at every sample rate, block size, IMAGE/TILT value,
and enabled state - verified across 44.1-192kHz and 32-2048 sample
blocks. Total plugin latency is unchanged from the pre-IMAGE baseline
(still `PREAMP + SAT + PITCH`'s own real latencies summed; EQ, PAN, VERB
and IMAGE all contribute 0).

## Parameter and state migration

`imager` keeps its pre-existing APVTS string ID, type, range and default
(`0..100%`, `AudioParameterFloat`, default `0%`) - unchanged by this
round. `imageTilt` is a **brand-new** parameter
(`-100..100`, `AudioParameterFloat`, default `0`/CENTER) added **without**
bumping `stateSchemaVersion` - see `Source/Core/PluginIdentity.h`'s
documented reasoning: unlike PITCH's v3 and PAN's v5 migrations (which
needed explicit code because an *existing* parameter ID's stored value
stopped meaning what it used to), a brand-new parameter has no old value
to reinterpret - `juce::AudioProcessorValueTreeState::replaceState()`
already leaves a parameter at its own constructed default whenever the
incoming state simply has no matching child, with no special-case code
needed. Verified directly: a hand-built legacy `PARAMETERS` tree with
every other parameter but no `<PARAM id="imageTilt">` child at all
restores `imageTilt` to exactly 0 (CENTER)
(`Tests/PluginTests.cpp`'s "Legacy state without imageTilt defaults to 0
(CENTER)" test).

## Real VST3 host validation

A purpose-built harness (the same `AudioPluginFormatManager`/
`VST3PluginFormat` real-hosting pattern used for every previous module)
loaded the built `.vst3` and confirmed: 9 host-visible parameters (the 8
UNI 76 parameters plus one host-added generic bypass, standard VST3
behaviour); the host-visible display name reads **"Image Tilt"**
(matching the product brief's preferred DAW-facing name, item 28); `imager`
defaults to 0%, `imageTilt` defaults to its normalised midpoint (real
value 0/CENTER); a full automation sweep across both axes in combination
(7 steps, including both extremes together) stayed finite throughout via
real, properly block-size-chunked `processBlock()` calls; state
round-tripped exactly through a real `getStateInformation()`/
`setStateInformation()` save+restore.

## UI: spatial field pad (supersedes the original linear TILT slider)

A follow-up UI-only round (no DSP change) replaced the original compact
horizontal TILT slider with a small square **spatial field pad**
(`Resources/Web/field_pad.js`, markup/styles in `index.html`/`scales.css`)
that drives both `imager` and `imageTilt` from a single 2D control:

- **X axis (horizontal) = `imageTilt`**: far left = -100 (LEFT), centre =
  0 (CENTER), far right = +100 (RIGHT) - a direct, unscaled mapping of
  the parameter's own normalised value.
- **Y axis (vertical) = `imager`**, visually inverted so the control
  reads top-to-bottom the way the product brief specifies: bottom = 0%
  (ORIGINAL/centred), top = 100% (WIDE) - `padY = (1 - imagerNormalised)
  * 100%`.

Spatial reading: bottom-centre = original/centred, top-centre = wide
centred, top-left/top-right = wide + left/right bias, bottom-left/
bottom-right = a left/right bias with minimal image amount - exactly the
six-point semantic the brief specifies, and a direct visual consequence
of the X/Y mapping above rather than anything hand-tuned per corner.

**The main IMAGE knob is unchanged and remains the primary `imager`
control** - it was not removed or shrunk in function, only visually
compacted to share its panel with the new pad (see "Layout" below). Two
controls now write the same parameter because that is exactly what a
real console offers: a coarse, precise rotary macro control *and* a
fast, intuitive spatial gesture for casual placement - not a
duplication to be resolved in favour of one winner.

**Sync mechanism.** No custom event bus was needed: `getSliderState(name)`
(`juce_webview.js`) returns one JS-side singleton `SliderState` object
per parameter name - the main knob and the pad both call
`getSliderState("imager")` and get back the *exact same* object. Whichever
widget calls `setNormalisedValue()`, the change round-trips through the
native `WebSliderRelay`/`WebSliderParameterAttachment` bridge and comes
back as a `valueChanged` event that fires on that one shared object,
notifying every listener registered on it - both the knob's own render
function and the pad's. This is the same mechanism that already
keeps host automation in sync with every other control in the plugin;
no new bridging code was written.

**Listener/microphone marker.** A small flat mark (a short rounded "cap"
over a thin "stem", built from two `<span>`s with CSS backgrounds - no
images/SVG, matching the project's UI rules) sits fixed at the pad's own
bottom-centre - the ORIGINAL/centred reference point. It is not
interactive; it exists purely so the pad reads as a spatial/listening-
position display rather than a generic XY input.

**Interaction.** Pointer drag anywhere in the pad (`pointerdown`/
`pointermove`/`pointerup` with `setPointerCapture`, matching every other
control's pattern) updates both parameters continuously, clamped to the
pad's own bounds. `sliderDragStarted()`/`sliderDragEnded()` are called on
*both* `imager`'s and `imageTilt`'s SliderState at the start/end of a
single pad gesture, so a host sees one coherent automation gesture per
axis rather than two independently-bounded ones. Double-click (and the
`Home` key) resets to bottom-centre (`imager=0`, `imageTilt=0`). Arrow
keys nudge one axis at a time (Left/Right = tilt, Up/Down = image
amount), Shift for fine control, matching the step sizes every other
control uses. Accessibility: the pad is a focusable, keyboard-operable
control with a live `aria-label` describing its current position in
plain text (`"Image field: Tilt L60, Image 85%"`) - a full native 2-axis
ARIA role does not exist, so this is a deliberate, honest minimum rather
than a claim of complete ARIA-slider compliance.

**Layout.** IMAGE's aux zone grew (a new `--field-pad-size` token,
`modules.css`'s `.module__aux--imager` height allowance increased
accordingly) to fit the tri-scale, a compact `FIELD` caption, and the
square pad underneath the main knob - the other six modules' panels are
untouched. The pad's own width is driven by its parent's actual
available space first and capped by `--field-pad-size` only as a
ceiling (`width: 100%; max-width: var(--field-pad-size, ...)`) rather
than the reverse - an earlier version of this control specified a fixed
clamp() width with `max-width: 100%` as an afterthought, which let the
pad's own intrinsic size push the whole IMAGE column wider than its
fair share of the 7-column row at the smallest supported window
(600x400), overflowing the editor itself; this was caught by real
screenshot testing at all three supported window sizes, not assumed.
The puck's own travel range is inset a few percent from the pad's true
edges so it never visually overhangs the border at the extremes.

A scratch JUCE host app loaded the real built `.vst3`, created its real
WebView2 editor, and captured native-window screenshots (Win32
`PrintWindow` with `PW_RENDERFULLCONTENT`) confirming by direct
screenshot - not by reading source - that:

- **IMAGE 0% / TILT CENTER** (fresh-instance resting state,
  `docs/screenshots/image-pad-center.png`): knob at rest, tri-scale
  marker at `ORIGINAL`, puck resting at the pad's bottom-centre right by
  the listener mark.
- **IMAGE 100% / TILT CENTER** (`image-pad-wide-center.png`): knob
  rotated fully, tri-scale marker at `WIDE`, puck at top-centre.
- **IMAGE 100% / TILT LEFT 100** (`image-pad-wide-left.png`): puck at
  the top-left corner, fully inside the pad's border.
- **IMAGE 100% / TILT RIGHT 100** (`image-pad-wide-right.png`): puck at
  the top-right corner, mirrored.
- **Responsive** at 600x400 (minimum), 960x640 (default) and 1350x900
  (maximum): the pad itself stays fully visible and usable at every
  size - at 600x400 specifically, IMAGE's own header/tri-scale text
  truncation is a **pre-existing** limitation (confirmed unchanged by
  comparing against the prior linear-TILT-slider UI at the same window
  size, before this round's changes), not something this round
  introduced or was in scope to fix.
- **State restore**: a *second*, freshly-created plugin instance loaded
  purely from a saved state (`imager=85%`, `imageTilt=-60`, no live
  automation call after load) rendered the knob and puck at the correct
  restored position immediately - confirming the pad's position is a
  pure function of the same saved parameter state every other control
  already relies on, not something that needs its own persistence path.

## Listening artifacts

No normalisation applied between variants within either set:

- `docs/audio/image-tilt-input.wav` / `-left100.wav` / `-left50.wav` /
  `-center.wav` / `-right50.wav` / `-right100.wav` - a synthetic
  sustained "vocal-like" source (fundamental + 4 partials, slow amplitude
  wobble) at IMAGE=100% through the full TILT range.
- `docs/audio/image-pan-motion-tilt-center.wav` / `-left.wav` /
  `-right.wav` - a centred-bass + decorrelated-stereo-highs source
  through PAN at 100% (MOTION), then IMAGE TILT at CENTER/-100%/+100%,
  demonstrating TILT biasing PAN's own moving spatial field.

## Known limitations

- The frequency-dependent bass-safety shelf approximates the original
  product sketch's 4-breakpoint description (`<80Hz very small, 80-150Hz
  limited, 150-300Hz appearing, 300Hz+ full`) with a single smooth RBJ
  shelf rather than matching those exact breakpoints - a deliberate
  choice (see "Frequency-dependent bass safety" above) favouring the
  project's proven no-vector-sum-bump construction over an exact
  breakpoint match. The measured curve is smooth, monotonic, and
  qualitatively consistent with the description, but does not hit `-0dB`
  precisely at 80Hz or full amount precisely at 300Hz.
- `thetaMax = 42 degrees` (not the true 45-degree "vanish point") is a
  judgement call, not a value derived from a stated product target - see
  "Why thetaMax = 42 degrees, not 45" above for the reasoning (keeping a
  small but nonzero Mid contribution on the quiet channel at full tilt).
  A different value would still satisfy every tested acceptance
  criterion; this one was chosen for a clear, generous safety margin.
- Mono fold-down at the combined worst case (IMAGE=100%+TILT=±100%)
  measures a real -3.05dB change, disclosed rather than hidden - not
  investigated further this round since it stayed well short of any
  "catastrophic loss" threshold and the underlying cause (TILT's own
  large, deliberate Mid-domain gain imbalance reducing the quiet
  channel's contribution to the mono sum) is already fully understood
  and expected.
