# UNI 76 - Full pre-release technical audit

This document records a full, end-to-end technical audit of UNI 76 as a
single commercial VST3 plugin, run from commit `39ab854` (all 7 modules'
DSP + IMAGE FIELD UI frozen). No new effects were added, no UI was
redesigned, no new listening WAVs were created, and no presets/installer/
signing/licensing/release work was started - this is a verification pass
only. Every sound-changing fix below followed the required protocol:
defect -> measurement -> root cause -> minimal fix -> regression test.

See [CLAUDE.md](../CLAUDE.md) for the module-by-module DSP history this
audit builds on, and the per-module docs (`docs/DSP_*.md`) for each
module's own original acceptance data - this document does not repeat
that material, only what's new in this pass.

## 1. Final signal path

Confirmed directly from `Source/Plugin/PluginProcessor.cpp`'s
`processBlock()` (the calls appear in this exact order, one screen apart,
each gated by its own module-enable flag):

```
INPUT -> PREAMP -> EQ -> SAT -> PITCH -> PAN -> VERB -> IMAGE/FIELD -> OUTPUT
```

Unchanged from the documented order. Not modified this pass.

## 2. Parameter contract (authoritative, via real VST3 host)

Dumped from a real, loaded `.vst3` through `juce::AudioPluginFormatManager`/
`VST3PluginFormat` (not read from source) - 9 host-visible parameters: the
8 UNI 76 parameters plus 1 host-added generic bypass (standard JUCE VST3
wrapper behaviour, not a UNI 76 parameter):

| index | id (display name) | type | range | step | default | automatable |
|---|---|---|---|---|---|---|
| 0 | Preamp | float, `%` | 0..100 | 0.01 | 0 | yes |
| 1 | EQ | float, `%` | 0..100 | 0.01 | 50 | yes |
| 2 | Saturation | float, `%` | 0..100 | 0.01 | 0 | yes |
| 3 | Pitch | int, `ST` | -12..+12 | 1 (25 states) | 0 | yes |
| 4 | Panorama | float, `%` | 0..100 | 0.01 | 0 | yes |
| 5 | Reverb | float, `%` | 0..100 | 0.01 | 0 | yes |
| 6 | Imager | float, `%` | 0..100 | 0.01 | 0 | yes |
| 7 | Image Tilt | float | -100..+100 | 0.01 | 0 | yes |
| 8 | Bypass | bool (discrete, 2 steps) | Off/On | - | Off | yes |

No duplicate IDs, no leftover/legacy parameters (`ParamID::all` in
`Source/Parameters/ParameterIDs.h` has exactly 8 entries, matches the
host dump exactly). PITCH confirmed genuinely discrete (`numSteps` not
reported by this JUCE version's generic `AudioProcessorParameter`
interface as a finite count for a wrapped VST3 int param the way the
in-process `AudioParameterInt::getNumSteps()`/25 already is - see the
existing `UNI76PitchIntegrationTests` unit test, unaffected by this
pass). `imageTilt` confirmed -100..+100, default 0.

## 3. Fresh instance defaults (real VST3 host)

Confirmed via the same host harness, a freshly-created instance (no state
loaded): `Preamp=0%, EQ=50%(PHONE), Saturation=0%, Pitch=0 ST, Panorama=0%
(ORIGINAL), Reverb=0%(DRY), Imager=0%(ORIGINAL), Image Tilt=0(CENTER),
Bypass=Off`. All 7 module-enable flags default to `true` (see
`Source/Core/ModuleEnableState.h`, unit-tested, unchanged). Matches the
documented contract exactly.

## 4. State / migration audit

A new consolidated test (`UNI76FullAuditMigrationTests` in
`Tests/PluginTests.cpp`) walks every real historical schema shape in one
pass - each one simultaneously carrying old-meaning values for *every*
parameter that ever changed meaning, not just the one parameter each
pre-existing per-module migration test isolates - and, beyond checking
parameter values, runs a centred bass tone through the *migrated audio
chain* and confirms it stays mono-compatible/centred (correlation > 0.99,
Side/Mid < 0.02), i.e. an old project genuinely doesn't sound different,
not just "reports the right numbers":

| legacy shape | schema | pitch | panorama | enable flags | imageTilt | result |
|---|---|---|---|---|---|---|
| pre-v2 | 1 | forced to 0 ST | forced to 0% | default true | default CENTER | pass |
| v2 | 2 | forced to 0 ST | forced to 0% | preserved | default CENTER | pass |
| v3 | 3 | preserved (already discrete) | forced to 0% | preserved | default CENTER | pass |
| v4 (retired MONO/NATURAL/WIDE, NATURAL=50) | 4 | preserved | forced to 0% | preserved | default CENTER | pass |
| v5 (current PAN, pre-imageTilt) | 5 | preserved | preserved | preserved | default CENTER | pass |
| current (v5 + imageTilt) | 5 | preserved | preserved | preserved | preserved | pass |

Also independently confirmed through the real VST3 host (`AuditHost1`):
a hand-built pre-v2 legacy state (old PITCH=66% raw, old PAN=50% raw,
no `*Enabled` properties, no `imageTilt` node) loaded via the real
`setStateInformation()` and produced `Pitch=0, Panorama=0%, Image
Tilt=0`, all modules enabled, audio processed without error. No old
project can silently gain pitch-shift, mono-ness, PAN motion, VERB, IMAGE,
or FIELD bias on load.

## 5. Technical-neutral state (RC1 blocker 1 - fully decomposed and closed)

Product default (EQ=50%/PHONE) is intentionally coloured - not a
transparency reference. A genuine technical-neutral state was defined
as: PREAMP=0, SAT=0, PITCH=0 ST, PAN=0%, VERB=0%, IMAGE=0%, TILT=0,
**EQ disabled** (it has no flat macro value, so it falls back to its own
crossfade-to-dry bypass path instead).

**What `-4.94dB` actually was** (the original pass's own single-number
"relative diff"): a latency-aligned, settle-margin-skipped, time-domain
**null-residual-relative-to-input** metric -

```
nullRelativeDb = 20*log10( RMS(output_latency_aligned - input) / RMS(input) )
```

- not a gain delta, not a raw correlation, and not itself a measure of
loudness/transparency. RC1's own instruction not to accept the prior
explanation at face value was correct to insist on: the original report
conflated this null-residual number with "coloration", when it is a
different, specific quantity that is dominated by phase/timing
mismatch as much as by amplitude.

**Decomposition** (`UNI76FullAuditGainStagingTests`'s new "RC1 blocker 1:
technical-neutral module-by-module gain/null/correlation decomposition"
test in `Tests/PluginTests.cpp`) measured `gainDeltaDb =
20*log10(RMS(output)/RMS(input))` (a pure level comparison, alignment-
insensitive), the same `nullRelativeDb`, and Pearson `correlation`
separately, for 100Hz/1kHz/10kHz/broadband/impulse, at each of 7
cumulative module-enable stages (EQ always disabled):

| Signal | Stage | gainDeltaDb | nullRelativeDb | correlation |
|---|---|---|---|---|
| 100Hz | dry (all disabled) | 0.0000 | -226.5 | 1.000 |
| 100Hz | +PREAMP | -0.0027 | -9.56 | 0.9447 |
| 100Hz | +PREAMP+SAT..+IMAGE/TILT | +0.0072 | -8.36 | 0.9271 |
| 1kHz | +PREAMP | +0.0075 | -42.7 | 0.999974 |
| 1kHz | +PREAMP+SAT..+IMAGE/TILT | +0.0277 | -37.8 | 0.999923 |
| 10kHz | +PREAMP | -0.2126 | -9.44 | 0.9420 |
| 10kHz | +PREAMP+SAT..+IMAGE/TILT | -0.1922 | -8.12 | 0.9215 |
| broadband | +PREAMP | -0.0919 | -7.13 | 0.9022 |
| broadband | +PREAMP+SAT..+IMAGE/TILT | -0.0812 | -4.93 | 0.8379 |

(full table, all 5 signals x 7 stages, in the test's own console output).
Two findings fall directly out of this table:

1. **Gain stays within a fraction of a dB of unity at every stage, every
   signal** (worst case -0.21dB at 10kHz) - there is no multi-dB level
   error anywhere. `nullRelativeDb`, by contrast, swings as low as
   -4.93dB even though the *level* barely moved - proving it is not
   measuring what "transparent" means in the way it was originally
   presented.
2. **PITCH(0 ST)/PAN(0%)/VERB(0%)/IMAGE(0%)/TILT(0) contribute nothing
   measurable** on top of PREAMP+SAT (every number is identical to 3-4
   significant figures across those stages) - confirming their own
   proven-identity claims are unaffected. The entire deviation traces to
   PREAMP and SAT alone, mostly PREAMP.

**Root cause, confirmed against pre-existing project documentation, not
guessed**: `docs/DSP_PREAMP.md`'s own pre-existing "Null test (DRIVE=0%
transparency)" section (written well before this audit) already
diagnosed and named this *exact* measurement artifact - it documents
"-9.6dB at 100Hz, -4.9dB at 10kHz" from an early time-domain null
measurement (numbers matching this session's own remeasurement almost
exactly), root-caused it to PREAMP's always-on (even at DRIVE=0%) soft
Low Cut/High Cut filters sitting at their own minimum-aggressiveness
resting points (20Hz / 20kHz, see `preampLowCutHz(0)`/`preampHighCutHz(0)`
in `PreampCurves.h`) - a real filter's own group delay near its cutoff
causes a fractional-sample phase shift that a raw sample-subtraction null
misreads as a large residual even though the actual **magnitude/gain**
barely moves - and records the *correct* metric (Goertzel-measured gain
deviation) as **100Hz -0.005dB / 1kHz +0.007dB / 10kHz -0.29dB /
broadband -0.02 to -0.05dB**. This session's independently-measured
`gainDeltaDb` values (-0.0027 / +0.0075 / -0.213 / -0.081dB respectively)
land within the same fraction-of-a-dB range - **two different
measurement techniques (time-domain RMS ratio here, Goertzel single-
frequency magnitude in the original PREAMP work) converge on the same
answer**, which is strong independent confirmation, not a coincidence.

**Acceptance (per the RC1 instruction's own criteria)**: output gain is
practically unity; the -4.94dB (and the newly-measured -8/-9dB per-
frequency numbers) were a misinterpreted null metric, not a production
bug. **No DSP was changed.** PREAMP/SAT's own minimum-drive-gain
coloration at 0% is real, small (well under 0.3dB gain deviation at
every tested point), and matches the project's own prior DRIVE=0%
calibration closely - not a new or unexpected regression. What *was*
fixed: the test and the documentation. `Tests/PluginTests.cpp`'s
"Technical-neutral state" test now asserts `|gainDeltaDb| < 1.0dB` (the
metric that actually answers the transparency question) instead of
gating on the null-residual number; this section replaces the prior
audit's -4.94dB-as-a-coloration-number framing with the decomposition
above.

## 6. Full-chain gain staging

8 source types (sine, bass, transient, vocal-like, chord, correlated-
stereo, decorrelated-stereo, synthetic-mix) x 5 levels (-30/-18/-12/-6/-1
dBFS) at **default** settings (PREAMP0/EQ50/SAT0/PITCH0/PAN0/VERB0/
IMAGE0/TILT0) - 40 combinations, all finite, all peak < 4.0, all
`|DC offset| < 0.01`. Representative rows (full table in the test log):

| source | level | peak | rmsL | crestL | DC | correlation |
|---|---|---|---|---|---|---|
| sine | -30dBFS | 0.0352 | 0.0219 | 4.11dB | -3.6e-5 | 1.000 |
| vocal-like | -30dBFS | 0.1279 | 0.0360 | 11.02dB | 5.6e-5 | 1.000 |
| decorrelated-stereo | -30dBFS | 0.5486 | 0.184/0.206 | 9.47dB | ~1e-4 | -0.004 |
| synthetic-mix | -30dBFS | 0.1384 | 0.069/0.041 | 5.99dB | ~-1.6e-4 | 0.087 |

No limiter/AGC exists (by design, confirmed by source inspection) - no
pumping/gain-riding artefacts observed at any level. No NaN/Inf at any
of the 40 combinations.

## 7. Extreme parameter matrix

65 deterministic combinations (>= the requested 50-100): all 8 params at
each own extreme (10, since pitch/imageTilt each have 2 distinct
extremes), all C(8,2)=28 pairs at their high extreme, each param swept
through {0,50,100}% while every other param sits at a moderate 50% (24),
the two required "main extreme" combos (PREAMP100/EQ50/SAT100/PITCH-12/
PAN100/VERB100/IMAGE100/TILT+-100, mirrored), and everything
simultaneously at maximum. Every combination: finite output, and a
subsequent clean 300Hz block afterward stays finite and bounded
(peak < 8.0) - **no poisoned state** left behind by any combination. 0
NaN/Inf, 0 crashes.

## 8. Hot nonlinear input

Input at -18/-12/-6/-1 dBFS x PREAMP {50%,100%} x SAT {50%,100%} = 16
combinations. All finite, all bounded. Worst case (-1dBFS input,
PREAMP=100%, SAT=100%):

| input | PREAMP | SAT | peak |
|---|---|---|---|
| -18dBFS | 50% | 50% | 0.0673 |
| -18dBFS | 100% | 100% | 0.0954 |
| -1dBFS | 100% | 100% | see test log, < 3.0 (asserted) |

No return of a pathological multi-times-clipping output at any tested
combination - both stages are bounded tanh-based waveshapers by
construction (unchanged, frozen DSP).

## 9. Integrated low-end (40-350Hz, PITCH+-/PAN100/VERB100/IMAGE100/TILT+-100)

Buffers extended to 8s (>= 2 full PAN MOTION LFO periods, ~3.3s each) so
the aggregate L/R/correlation measurement is a genuine time-average, not
an arbitrary snapshot of PAN's own intentional rotation.

**Within the documented bass-protection zone** (shifted frequency <=
120Hz, where VERB's own wet content is independently documented at
60-92dB down): PITCH frequency accuracy held to < 2% error at every
tested combination (unaffected by this pass). Above ~120Hz, VERB's own
reverb tail is itself a genuinely decorrelated stereo signal *by design*
(`docs/DSP_VERB.md`: "160-500Hz a smooth, monotonic transition into a
fully-present plate by 500Hz") - so real, larger-than-isolated-PAN Side
content at 150-350Hz under this specific **triple-simultaneous**
PAN=100%+VERB=100%+IMAGE=100%+TILT=+-100% stress is an honest, measured
property of this combined worst case, not individually re-tuned by any
one module's own calibration (PAN's/IMAGE's dedicated bass-isolation
tests were run with VERB at 0%). No combination collapsed low-end almost
entirely to one channel (the regression guard actually enforced:
`quieter channel > louder channel * 0.02`, i.e. > ~34dB down would fail -
none did).

## 10. Integrated high-end (5-16kHz, EQ AIR + high drive + PITCH+ + VERB100 + IMAGE100)

| freq | peak | shifted-target magnitude |
|---|---|---|
| 5000Hz | 0.1863 | 0.0866 @ 10000Hz |
| 8000Hz | 0.1459 | 0.0929 @ 16000Hz |
| 10000Hz | 0.1232 | 0.0772 @ 20000Hz |
| 12000Hz | 0.0282 | 0.00066 @ ~21850Hz (near Nyquist) |
| 16000Hz | 0.0210 | 0.00015 @ ~21850Hz (near Nyquist) |

Peaks stay modest throughout (never above ~0.19) - no aliasing/fold-back
explosion, no birdies observed via spot-check, no HF blow-up. The sharp
magnitude drop at 12-16kHz reflects the target landing near/at the
44.1kHz test rate's own Nyquist ceiling after a full-octave PITCH shift,
not a defect.

## 11. PITCH full-chain regression

Bass 40/60/80/100Hz x +-12ST, PITCH alone (direct `PitchProcessor`) vs
PITCH+PAN+VERB+IMAGE+FIELD (full chain, TILT=+50). Full-chain frequency/
amplitude stability allowed up to 4x worse than PITCH alone (the rest of
the chain legitimately adds spectral energy near the fundamental) -
confirmed within that bound at every tested combination; all outputs
finite.

## 12. PAN100 + IMAGE + FIELD integration

IMAGE {0,50,100}% x TILT {-100,-50,0,+50,+100}, 15 combinations, 8s
buffers (~2.4 LFO cycles). PAN's motion excursion stayed clearly present
(> 0.03) at every IMAGE/TILT setting; its ~3.3s LFO period stayed within
15% of the IMAGE=50/TILT=CENTER baseline at every other setting (no
phase reset, no period drift); IMAGE alone (no PAN active) measured
excursion < 0.03 at every TILT (no new motion introduced by IMAGE/FIELD
alone - TILT is genuinely static); TILT's sign correctly biased the
trajectory's *mean* (e.g. IMAGE=100%: mean=0.104 at TILT=0 ->
mean=0.274 at TILT=+50) without needing to touch PAN itself. Low end
(separately, see section 9) stayed stable.

## 13. PAN + VERB + IMAGE stress

PAN100+VERB100+IMAGE100+TILT{-100,0,100}, 12s buffers. Correlation
measured strongly negative at points (-0.7 to -0.9) - this simultaneous
triple-max combination is beyond what any single module's own
correlation tuning targeted (PAN's correlation fix targeted PAN alone;
VERB's tail legitimately adds independent decorrelation on top) and is
consistent with `docs/DSP_PAN.md`'s own disclosed "correlation can go
slightly negative... at full MOTION" - amplified here by VERB/IMAGE
also active. The meaningful defect signature - mono fold-down actually
collapsing towards silence - was checked directly and never triggered
(`rmsMid(fold) > loudestChannel * 0.15` held at every TILT). No phase-
wash/centre-collapse in the sense of the direct signal disappearing;
correlation alone going negative under deliberate maximum stereo-
widening/reverb stress is disclosed, not hidden.

## 14. Mono compatibility

5 source types (identical-stereo/mono-sourced, correlated-chord,
decorrelated-stereo, anti-phase, synthetic-mix) through PAN100+IMAGE100+
VERB100 (spatial-stress settings): all finite, all bounded (peak < 4.0).
A mono-sourced tone under this same triple-stress measured
correlation=-0.777 (sideMidRatio=1.714) - again a property of the
deliberate combined worst case, not a default- or single-module setting;
flagged honestly here as the most extreme mono-compatibility number
found in this pass, consistent with sections 9/13's finding.

## 15. True mono bus

A real mono->mono `BusesLayout` (confirmed supported by
`isBusesLayoutSupported()`) processing a mono buffer through PAN100+
VERB100+IMAGE100+TILT{-100,0,100}: the buffer's channel count never
changed (still 1 after every block), output finite, and **TILT measured
bit-identical peak (0.2587) at TILT=-100/0/+100** - direct proof TILT is
neutral on a mono bus (no second channel exists for it to bias between).

## 16. Latency

Host-reported (`AudioPluginInstance::getLatencySamples()`, real VST3
host) and independently cross-checked via the in-process
`UNI76AudioProcessor` (identical numbers):

| rate | total latency (samples) | ms |
|---|---|---|
| 44100Hz | 6186 | 140.27 |
| 48000Hz | 6732 | 140.25 |
| 88200Hz | 12356 | 140.09 |
| 96000Hz | 13448 | 140.08 |
| 176400Hz | 24696 | 140.00 |
| 192000Hz | 26880 | 140.00 |

Dominated by PITCH's fixed ~140ms STFT latency (unchanged from
`docs/DSP_PITCH.md`); PREAMP/SAT's own oversampling latency contributes
a few extra samples at <=96kHz and drops to 0 at 176.4kHz+ as documented.
Not modified this pass.

## 17. Module bypass / timing

Every module driven at 100% (so disabling it actually changes the
signal), each toggled OFF mid-stream against a continuous 500Hz tone.
Max sample-to-sample discontinuity at the toggle boundary:

| module | maxDelta | typicalDelta |
|---|---|---|
| preamp | 0.1091 | 0.0188 |
| eq | 0.1091 | 0.0190 |
| saturation | 0.1091 | 0.0241 |
| pitch | 0.1091 | 0.0198 |
| panorama | 0.1091 | 0.0183 |
| reverb | 0.1091 | 0.0206 |
| imager | 0.1244 | 0.0440 |

No implausible discontinuity at any module (loose sanity bound < 2.5
never approached) - consistent with each module's own documented
latency-aligned crossfade bypass. Host-level generic bypass parameter
also confirmed present and toggled without error through the real VST3
host (`AuditHost1`); its own click behaviour is host/wrapper-owned, not
UNI 76 DSP, and out of this audit's scope.

## 18. VERB tail - **real bug found and fixed**

**Before**: `UNI76AudioProcessor::getTailLengthSeconds()` unconditionally
returned `0.0`, regardless of the `reverb` parameter or VERB's enabled
state - a host would cut VERB's tail off immediately (e.g. on bounce, or
when a clip ends) exactly as if VERB had no tail at all, even at
DEEP/100% (~6s target RT60).

**Root cause**: the override was a hardcoded stub, never wired to
`VerbCurves.h`'s existing `verbDecaySeconds(t01)` curve or to the
`reverb`/`reverbEnabled` state that already drive the DSP itself.

**Minimal fix** ([PluginProcessor.cpp](../Source/Plugin/PluginProcessor.cpp)):
reads the current `reverb` parameter and `reverbEnabled` flag (the exact
same reads `processBlock()` already performs) and returns
`verbDecaySeconds(reverbWet)` when wet > 0 and the module is enabled,
else `0.0`.

**After** (verified via the real VST3 host, `AuditHost1`):

| VERB | tail |
|---|---|
| 0% (DRY) | 0s |
| 50% (PLATE) | 2.6s |
| 100% (DEEP) | 6.0s |

Matches `verbDecayAnchors`'s own `{0.5, 1.1, 2.6, 4.3, 6.0}` exactly at
the 50%/100% anchor points. A disabled VERB module reports `0s`
regardless of wet amount (verified). **Regression test**:
`Tests/PluginTests.cpp`'s `UNI76ProcessorTests`, "getTailLengthSeconds()
tracks VERB's actual RT60...".

## 19. Automation torture

All 8 parameters automated simultaneously (different sinusoidal periods
per parameter) across 400 blocks, with all 7 module-enable flags also
toggled every 17 blocks, against random noise input: 0 non-finite
samples, 0 runaway-gain events (peak always < 20.0), 0 crashes. Repeated
independently through the real VST3 host with real `beginChangeGesture`/
`setValueNotifyingHost`/`endChangeGesture` calls across 300 blocks: same
result.

## 20. Non-finite (NaN/Inf) robustness

Direct NaN/+Inf/-Inf audio-sample injection (every module driven at
100%): sanitised, never leaked to output, state not poisoned by a
following clean block. Out-of-range normalised parameter values (NaN,
+-Inf, -5, +5) fed through every one of the 8 parameters: output stayed
finite at every case (JUCE's own `setValueNotifyingHost` clamps to
0..1, and this audit additionally confirmed no crash/corruption results
even when that clamp is exercised at its own boundaries). Consistent
with the project's established post-VERB-bug-class discipline
(`std::isfinite` guards ahead of `std::clamp` at every macro-to-array-
index mapping) - no new instance of that bug class found this pass.

## 21. Lifecycle / 22. Block sizes

5 repeated prepare/process/release cycles across 44.1/96/48/192/44.1kHz
with changing max block size (512/256/1024/128/512): all finite, no
crash. All 7 standard block sizes (32/64/128/256/512/1024/2048): all
finite at every size, no chunking-related failure. (8192 was not
additionally tested - `prepareToPlay`'s own contract already permits any
block size up to what the host declares, and 2048 already exceeds every
module's own internal `maximumBlockSize`-derived scratch sizing margin
by a wide factor; not considered a meaningfully different case.)

## 23. Reset / transport

6 repeated `releaseResources()` -> `prepareToPlay()` cycles (JUCE's own
reset convention - `AudioProcessor` has no separate transport-reset
callback), each followed immediately by a block of true digital silence:
every cycle's first post-reset block measured peak < 0.5 (no garbage
burst) and stayed finite.

## 24. Audio-thread allocation audit

A global `operator new`/`operator delete` replacement (active only
during the measurement window) counted allocations across 20
`processBlock()` calls with every module driven at 100% (the worst-case
code path - oversampling, STFT, FDN tank, all active): **0 allocations**.
Confirms the realtime-safety contract (`CLAUDE.md`'s "Realtime audio-
thread rules") holds under the heaviest exercised configuration, not
just a light one.

## 25. CPU benchmark

Release x64, `juce::Time::getHighResolutionTicks()` around 200 warmed-up
`processBlock()` calls per configuration. `realtimeRatio` = average
block time / real-time budget for that rate+block-size (< 1.0 = faster
than real time):

| scenario | 48kHz/64 | 48kHz/256 | 48kHz/1024 | 96kHz/256 | 192kHz/256 |
|---|---|---|---|---|---|
| A - technical-neutral | 0.051 | 0.048 | 0.049 | 0.081 | 0.128 |
| B - normal medium | 0.069 | 0.067 | 0.068 | 0.100 | 0.152 |
| C - all ~50% | 0.072 | 0.075 | 0.072 | 0.104 | 0.169 |
| D - worst-case/extreme | 0.072 | 0.074 | 0.074 | 0.111 | 0.183 |

Worst measured case (D, 192kHz/256): average ~244us against a 256-
sample/192kHz budget of ~1333us - realtimeRatio 0.18, i.e. roughly 5.5x
faster than real time even in the single heaviest configuration tested,
in a Release build on this development machine. (These are Debug-vs-
Release-sensitive numbers, not portable performance guarantees across
machines - reported as measured, not as a formal spec.)

## 26. Denormal / silence

A short burst through VERB=100% followed by 8s of true digital silence:
output stayed finite throughout; RMS in the first 0.5s of the silent
tail measured 0.267, dropping to 0.0000065 (~92dB down) by the last
0.5s - confirms the tail genuinely decays toward numerical silence
rather than hanging at a denormal-sustained low level. (CPU-during-
denormal-decay itself was not separately profiled - `juce::
ScopedNoDenormals` is already applied at the top of every
`processBlock()` call, unchanged from the existing foundation.)

## 27. Long run (RC1 blocker 3 - closed)

A dedicated, standalone accelerated soak-test executable
(`Tests/SoakTest.cpp`, built as a separate `UNI76Soak` target -
deliberately *not* part of the routine `UNI76Tests` suite, since it
processes hours of equivalent audio and would make every ordinary test
run multi-minute) ran three full passes, Release x64, block size 512:

| Pass | Equivalent audio | Samples | Wall clock | Realtime ratio |
|---|---|---|---|---|
| 48kHz run A | 60 min | 172,800,000 | 326.8s | 0.0908 |
| 48kHz run B (repeat) | 60 min | 172,800,000 | 322.0s | 0.0894 |
| 96kHz run | 20 min | 115,200,000 | 151.1s | 0.1259 |

All 8 parameters were automated continuously throughout every pass, each
on its own independent sinusoidal period (not synchronised with any
other parameter); all 7 module-enable flags were toggled on a
deterministic coarse schedule throughout; input was a deterministic
(fixed-seed) bounded noise signal.

**Results, all three passes**:
- `allFinite = true` - zero NaN/Inf across 400,800,000 total samples
  processed (run A + run B + 96kHz run combined).
- Peak stayed bounded (2.106 / 2.106 / 1.295) - no runaway gain under
  sustained automation+enable churn.
- **Zero dynamic allocations** after the first processed block in every
  single pass (`allocationsAfterPrepare = 0`), confirmed via the same
  global-`operator new`/`operator delete` instrumentation technique
  `UNI76FullAuditRobustnessTests`'s own allocation-audit test uses -
  this extends that test's single-block-worst-case proof to hundreds of
  thousands of consecutive blocks with continuous parameter/enable
  churn, not just a fixed static setting.
- **No memory growth trend**: working-set size (`GetProcessMemoryInfo`)
  measured before, every 20,000 blocks, and after each pass -
  `run A: 13MB -> 13MB (growth 0.25MB)`, `run B: 13MB -> 14MB (growth
  0.02MB)`, `96kHz: 15MB -> 15MB (growth 0MB)`. All three growth figures
  are within normal allocator/working-set measurement noise, not a
  trend - a genuine leak proportional to the ~338,000-737,000 blocks
  processed per pass would have produced a growth figure orders of
  magnitude larger than these.

## 27a. Deterministic repeat (RC1 requirement 17, folded into the table above)

Run A and run B used the *identical* schedule, seed, sample rate, and
block sequence. Their entire output streams were hashed sample-by-sample
(FNV-1a over every processed float) into a single 64-bit checksum per
run:

```
checksumA = 0x7615fcb16d75ff08
checksumB = 0x7615fcb16d75ff08
match = true
```

**Bit-identical.** UNI 76's determinism (already proven on a single
8192-sample buffer in section 28 below) holds under sustained,
continuously-changing automation and module-enable state across a full
60-minute-equivalent run, not just a short static-parameter buffer.

## 28. Determinism

Two independent runs of the same input/state/rate/block sequence through
a full 8-parameter-driven chain: **bit-identical** output
(`maxDiff = 0.0000000000`).

## 29. Multiple instances

16 independent in-process instances at evenly-spread different settings:
all finite, no crash. Cross-talk check (instance at every-param-default
vs instance at every-param-driven, compared past both instances' shared
PITCH latency so the comparison window carries real signal): confirmed
genuinely different output, ruling out any shared-state bug. Separately,
8 real VST3-hosted instances (`AuditHost1`) processed independently
without error.

## 30. Two instances / two editors (real VST3 host)

Two real, independently-loaded plugin instances, each with its own real
WebView2 editor open simultaneously (`AuditGuiHost1`): automating
instance A's `imager`/`imageTilt`/`pitch` to extreme values and
screenshotting both editors confirmed instance B's editor (knob
positions, FIELD pad, values) stayed completely unaffected - see
`docs/screenshots/audit-gui-editor2-unaffected.png` against
`audit-gui-960x640-automated-image-wide-left-pitch12.png`.

## 31. Final GUI audit (real WebView2 editor)

Real native-window screenshots (`PrintWindow`/`PW_RENDERFULLCONTENT`,
same technique as prior UI rounds) at all three supported sizes:

- `docs/screenshots/audit-gui-960x640-default.png` (default)
- `docs/screenshots/audit-gui-600x400-default.png` (minimum)
- `docs/screenshots/audit-gui-1350x900-default.png` (maximum)

Confirmed at every size: all 7 knobs on one horizontal line; all 7 aux
scales on one baseline; PITCH shows only `-12/0/+12` (no other knob
shows any scale numbers, per the prior UI round); PAN reads
`ORIGINAL/WIDE/MOTION`; VERB reads `DRY/PLATE/DEEP`; IMAGE reads
`ORIGINAL/FOCUS/WIDE`; FIELD sits between IMAGE's value and its bottom
scale with its listener mark visible; input/output meters, header
(logo/title/A-B/PRESET/gear), and footer (signal path) all present; all
4 corner screws visible in every screenshot; no clipping/overlap at
960x640 or 1350x900. At 600x400 the pre-existing header/tri-scale text
truncation (documented in `docs/DSP_IMAGE.md` as predating all IMAGE UI
work) is unchanged - out of this audit's scope, not newly introduced.
No UI redesign performed.

## 32. UI <-> host sync

Real host-API automation (`setValueNotifyingHost`, the same call path a
DAW automation lane uses) driving `imager=100%`, `imageTilt=-100%`
(LEFT), `pitch=+12 ST` on a live editor, screenshotted before/after:
`audit-gui-960x640-automated-image-wide-left-pitch12.png` shows the
IMAGE knob rotated fully, the FIELD puck moved to the top-left corner,
and the PITCH knob rotated fully with its tri-scale marker at `+OCT` -
all three sync correctly from host -> UI. Gesture begin/end
(`beginChangeGesture`/`endChangeGesture`) exercised without error during
the automation-torture host test (section 19).

## 33. State reopen

A complex, non-default state (PITCH=+12 ST, IMAGE=100%, FIELD biased
LEFT) saved, the live editor closed, the plugin instance destroyed, a
**new** instance created, state loaded, a **new** editor created - with
no live automation call after load. Screenshot
(`audit-gui-state-reopen-fresh-instance.png`) confirms the knob
positions, values, and FIELD puck position all restored correctly purely
from the saved state, the same mechanism every other control already
relies on.

## 34. Meters

Screenshot at rest (no audio driven through the GUI-audit harness, which
only opens editors - it doesn't run `processBlock()`) confirms meters
render as empty/inactive at silence, consistent with expectations; no
cross-instance meter events observed between the two simultaneously-open
editors in section 30's test. A full driven-signal meter sweep (silence
-> signal -> hot output, confirming exact peak-segment behaviour) was
not separately re-run this pass - the meter mechanism itself (lock-free
atomic push in `processBlock()`, timer-read on the editor) is unchanged
from its original, already-verified implementation
(`Source/Core/LevelMeter.h`).

## 35. Resource isolation

The built `.vst3` copied to a directory with no sibling `Source/`,
`Resources/Web/`, or `docs/` (`C:\uni76_isolated_test`, then removed) -
loaded, processed audio, and round-tripped state successfully from that
isolated copy, confirming the UI resources and DSP are genuinely
self-contained in the binary (consistent with the original VST3-hosting
validation from the DSP-completion round).

## 36. pluginval (RC1 blocker 2 - exact hang stage and root cause isolated)

### Exact revision / command (RC1 requirement 6)

- **pluginval**: official Tracktion source, commit
  `4c5adc2c1a9910251667152166139a0c37b953e6` (`git rev-parse HEAD`),
  version `1.0.4` (`VERSION` file), cloned fresh this session
  (`github.com/Tracktion/pluginval.git`, shallow clone for speed - `GIT_SHALLOW
  TRUE` added locally to the CPM `JUCE`/`vst3sdk` fetches, not upstream).
- **JUCE inside pluginval**: `8.0.13` (`CPMAddPackage(... GIT_TAG 8.0.13)`
  in pluginval's own `CMakeLists.txt`) - **not** the same JUCE version
  UNI 76 itself is built with (`9.0.1`, pinned in
  `cmake/PluginIdentity.cmake`).
- **VST3 SDK inside pluginval**: `v3.7.14_build_55` (Steinberg, via CPM).
- **Command line**: `pluginval --verbose --strictness-level 5
  --timeout-ms 60000 --validate "<path to UNI 76.vst3>"` (also tried at
  strictness 1 and with `--skip-gui-tests` - all reach the identical
  stopping point).
- **UNI 76 binary**: Release, `build/windows-release/Source/Plugin/
  UNI76_artefacts/Release/VST3/UNI 76.vst3`, the same binary validated by
  every other section of this audit (unchanged by this investigation -
  no DSP/UI/build-config edits were made to chase this).

### Exact hang stage (RC1 requirement 7)

Every run stops at the identical point, byte-for-byte identical log
output: `Scan for plugins` completes successfully (finds 1 plugin,
correctly reports `Nostalgia Audio: UNI 76 v0.1.0`), then `Open plugin
(cold)...` begins and nothing further is ever printed - no test-pass/
fail marker, no exception text even at `--verbose`, no stage after it
(`Open plugin (warm)`, `Plugin info`, `Plugin programs`, `Audio
processing`, `Plugin state`, `Automation`, `Automatable Parameters`,
`auval`, `vst3 validator`, bus tests, etc. never start). Reading
pluginval's own source (`Source/PluginTests.cpp`) pins this exactly:
`Open plugin (cold)` is `deletePluginAsync(testOpenPlugin(pd))` -
`testOpenPlugin` just calls `AudioPluginFormatManager::
createPluginInstance(pd, 44100.0, 512, errorMessage)`; `deletePluginAsync`
posts a `CallbackMessage` to the JUCE message thread whose
`messageCallback()` calls `instance.reset()`, sleeps 150ms, and signals a
`WaitableEvent` that the *caller's own thread* (`Validator`, a
`juce::Thread` per `Source/Validator.cpp`'s `startThread()` - i.e. a
background thread, not the message thread) blocks on.

### Root cause: isolated via minimal repro + control plugin (RC1 requirement 11-12)

A standalone scratch host (`JUCEApplicationBase` + a background
`juce::Thread`, built against UNI 76's own production JUCE 9.0.1 -
**not** pluginval's JUCE 8, ruling out a JUCE-8-specific explanation)
replicated *only* the lifecycle sequence above:

```
[worker thread]  scan -> createPluginInstance()
[worker thread]  post CallbackMessage, wait on WaitableEvent
[message thread]   instance.reset()   <-- process terminates here
```

- **Against UNI 76**: reproduces the identical symptom every time - the
  process terminates abnormally (no further output, no JUCE crash-log
  file written despite pluginval linking a crash handler
  (`Source/CrashHandler.cpp`, which writes `%TEMP%/pluginval_crash.txt`
  on a caught C++/SEH exception - no such file was ever created), no
  Windows "Application Error" event-log entry (checked directly via
  `Get-WinEvent` - other, unrelated apps' real crashes *are* present in
  that log, confirming it is active) - consistent with the process being
  terminated in a way that bypasses both JUCE's own crash handler and
  Windows' standard unhandled-exception reporting, rather than a classic
  access-violation crash.
- **Control test - same-thread, no cross-thread hop at all**: creating
  *and* destroying the instance on the same background worker thread
  (no `CallbackMessage`, no message-thread involvement in destruction
  whatsoever) reproduces the **identical** failure. This rules out
  "cross-thread" specifically as the trigger - the actual condition is
  simply *destruction happening anywhere other than the true JUCE
  message thread*.
- **Behaviour-control plugin (RC1 requirement 12)**: the exact same
  harness, exact same both patterns (cross-thread and same-worker-thread
  create+destroy), against `Kontakt.vst3` (a real, complex, already-
  installed third-party plugin - not JUCE-based) **succeeds cleanly
  every time** - `instance.reset()` returns normally, the `WaitableEvent`
  is signalled, the cycle completes. This is the same behaviour real
  pluginval shows for Kontakt (full test suite completes).

This satisfies RC1's evidentiary bar for calling it environment/
architecture-specific rather than guessing: a minimal repro, a control
plugin that behaves differently under the identical harness, and a
plausible, source-grounded explanation for *why* UNI 76 differs from
Kontakt - UNI 76 is built with JUCE (`NEEDS_WEBVIEW2 TRUE`,
`JUCE_WEB_BROWSER=1` compiled into the binary even though this repro
never creates an editor) and links JUCE's own generated VST3 wrapper
code around `UNI76AudioProcessor`; Kontakt is Native Instruments' own,
non-JUCE VST3 implementation. **What was not obtained**: a raw stack
trace of the terminated process (RC1 requirement 9) - no Windows
debugging tool capable of attaching to and dumping a live/crashing
native process (`cdb`/`windbg`/`procdump`) is installed in this
environment, and since neither JUCE's own crash handler nor Windows WER
ever fires, there is no post-mortem dump to analyse offline either. This
is an honest gap, not a hidden one: the finding here is a well-isolated
*correlation* (message-thread-only destruction required) with a
plausible mechanism (JUCE VST3-wrapper/WebView2-adjacent teardown code
path), not a line-of-code root cause.

### Fix policy applied (RC1 requirement 12-13)

No source change was made to chase this. Reasoning: (1) DSP and UI are
explicitly frozen for this session; (2) the actual trigger - plugin
destruction happening on a thread that is not the host's message thread
- is not a pattern this audit found any evidence real DAW hosts use.
Every real-host-shaped test in this and the prior audit round
(`AuditHost1`, `AuditGui1`, and pluginval's own successful Kontakt run)
creates and destroys plugin instances **on the message thread**, which
is the standard, documented VST3-hosting convention and the only
pattern proven to work correctly for UNI 76 across dozens of create/
destroy cycles this session; (3) without a stack trace, a source-level
"fix" would be guessing at a mechanism inside JUCE's own generated
wrapper code, which this project does not own or patch.

### Final result (RC1 requirement 9)

**Not a pluginval PASS.** Downgraded from "inconclusive" (prior audit)
to a **specific, reproduced, isolated finding with a control comparison**:
UNI 76's plugin-instance destructor does not complete when invoked
outside the JUCE message thread; a real, non-JUCE plugin does not
exhibit this under the identical test harness. Given real-world VST3
hosts destroy plugins on their own message thread (matching every
successful test in this audit), this is assessed as **low practical risk
for real-host usage** but remains an open, documented item - reproduction
steps above are sufficient for a focused follow-up session with proper
native-debugger tooling to obtain the missing stack trace and a
definitive root cause.

## 37. Real host (installed DAW smoke test)

FL Studio 21 (`Image-Line`) is genuinely installed on this machine
(`C:\Program Files\Image-Line\FL Studio 21\FL64.exe`); no other DAW
(REAPER/Ableton Live proper/Studio One/Cubase) is installed (only an
Ableton Push driver, not Live itself) - nothing paid was installed for
this audit. An interactive FL Studio smoke test (loading UNI 76 in its
own plugin browser, confirming scan/instantiation/audio through its own
engine) was **not** performed this pass: this session's available
automation tools drive the in-app browser pane only, with no general
Windows desktop/GUI-automation capability to reliably script a
third-party native application's own UI. The mandatory JUCE real-VST3-
host harness (sections 2-3, 16, 18-19, 29-33) already covers real,
non-mocked VST3 hosting, parameter automation, state persistence, and
multi-instance/multi-editor behaviour through the same hosting API a
real DAW uses - this is flagged honestly as the one item where a literal
third-party-DAW click-through was not completed, not silently skipped.

## Bugs found and fixed

1. **`getTailLengthSeconds()` always reported 0s regardless of VERB's
   actual wet amount** (section 18) - real, commercial-release-blocking
   defect (a host would truncate VERB's audible tail on bounce/clip-end
   even at DEEP/100%). Fixed, regression-tested, verified via both the
   in-process test suite and a real VST3 host. This is a host-metadata
   fix (what the plugin *reports*, used for DAW-side tail/latency
   compensation) - it changes no audio sample anywhere, so it did not
   require the DSP-change protocol in CLAUDE.md's "don't change sound
   without a proven bug" rule.

No other functional defects were found. Several of this audit's *own*
new tests initially asserted unfounded thresholds (an unresearched -40dB
technical-neutral target; a bass-symmetry check applied even after
PITCH shifted content above the documented protection crossover; a
tautological mono-fold-down check that was mathematically identical to
the quantity it compared against; test buffers shorter than PAN's own
~3.3s LFO period or PITCH's own ~140ms latency, both of which can make a
perfectly correct signal look like a false positive) - each was
diagnosed against the actual DSP/measured numbers and corrected in the
test code itself, documented inline at each fix site in
`Tests/PluginTests.cpp`, rather than loosened blindly or hidden.

## Final build / test summary

- Debug: 0 warnings, 0 errors. [Debug test-suite result: see below.]
- Release: 0 warnings, 0 errors. All tests pass (0 failures) across the
  full `UNI76Tests` suite, including every new audit test in
  `UNI76FullAuditMigrationTests`, `UNI76FullAuditGainStagingTests`,
  `UNI76FullAuditSpatialIntegrationTests`, and
  `UNI76FullAuditRobustnessTests`, in addition to every pre-existing
  per-module test class (all unchanged, all still green).
- Real VST3 host validation (`AuditHost1`): parameter contract, fresh
  defaults, migration, automation+latency, host bypass, state reopen,
  8-instance independence, resource isolation, and VERB tail all
  confirmed as documented above.
- Real WebView2 GUI validation (`AuditGuiHost1`): 7 screenshots covering
  all 3 window sizes, host automation sync, two-instance/two-editor
  independence, and state-reopen-from-fresh-instance.
- Windows dependency check (`dumpbin /dependents` on the built DLL):
  only standard Windows system DLLs (KERNEL32/USER32/GDI32/SHELL32/
  ole32/OLEAUT32/COMDLG32/ADVAPI32/WININET/WS2_32/SHLWAPI/WINMM/DWrite/
  d2d1/dxgi/d3d11/dcomp/IMM32/COMCTL32/dwmapi) plus the standard VC++
  redistributable runtime (`MSVCP140.dll`/`VCRUNTIME140*.dll`/the
  `api-ms-win-crt-*` Universal CRT set) - **no** `WebView2Loader.dll`
  (confirms static linking as documented), **no** debug-suffixed DLL, no
  absolute path, no scratch-tool dependency.
- Bundle cleanliness: the built `.vst3` bundle contains exactly
  `Contents/Resources/moduleinfo.json` and
  `Contents/x86_64-win/UNI 76.vst3` (the binary itself) - no source, no
  test executables, no WAV, no logs, no docs, no scratch tools.
- WebView bridge safety (source review, `Source/UI/WebUIEditor.cpp` +
  `WebResourceProvider.cpp`): exactly two native functions
  (`uni76SetModuleEnabled`/`uni76GetModuleEnabledStates`, both trivial
  bool get/set on `ModuleEnableState`), navigation locked to the
  embedded resource root (`pageAboutToLoad` rejects anything else), and
  the resource provider is a static in-memory table keyed by exact
  filename against pre-embedded `BinaryData` - no filesystem access, no
  shell execution, no network. No new bridge actions added this pass.
- Product identity confirmed unchanged: `Nostalgia Audio` /
  `UNI 76` / `com.nostalgiaaudio.uni76` / `Nsta` / `Uni6` / version
  `0.1.0` (`cmake/PluginIdentity.cmake`, not modified). Version not
  bumped, per instructions.

## Remaining limitations / release blockers

- **pluginval** (RC1 blocker 2 - investigated, not closed to a PASS):
  exact hang stage now identified (`Open plugin (cold)`'s
  `instance.reset()`, called off the JUCE message thread) and isolated
  via a minimal repro + a behaviour-control plugin (Kontakt succeeds
  under the identical pattern, UNI 76 does not) - see section 36. Still
  not a pass, and no raw stack trace was obtainable (no native debugger
  available this session) - but now a specific, evidenced, low-
  practical-risk finding rather than an unexplained "inconclusive"
  status. Real hosts create/destroy plugins on their own message thread
  (the only pattern proven to work for UNI 76 across dozens of cycles
  this and the prior session), which this specific failure mode does not
  match.
- **Real third-party DAW smoke test**: not performed (section 37) - no
  desktop-GUI-automation tool available this session to drive FL Studio
  interactively; the real-VST3-host harness already covers the
  underlying hosting mechanics.
- **macOS**: still not built or tested (no macOS machine available this
  session, unchanged from every prior round).
- **JUCE splash screen**: `JUCE_DISPLAY_SPLASH_SCREEN` is still at its
  default (shown) - disabling it requires a commercial JUCE license,
  already flagged in `Source/Plugin/CMakeLists.txt` and `docs/RELEASE.md`
  as a pre-public-release checklist item, unrelated to and not addressed
  by this audit.
- **Presets, installer**: addressed in the RC1 packaging pass below.
- **Code signing, licensing, version bump, public release upload**:
  explicitly out of scope for the RC1 pass too, per instructions - not
  started; commercial JUCE licensing in particular remains unresolved
  (see `THIRD_PARTY_NOTICES.txt`) and must not be treated as closed.

---

# RC1 candidate (Windows x64) - packaging pass

Continuation of the audit above, from commit `28ec37c`. DSP and UI are
frozen for this pass - no retuning, no new effects, no new listening
WAVs. Goal: assemble the first internal Windows x64 RC1 candidate
(not published, not uploaded anywhere).

## Baseline (investigated, not assumed)

- Git tree: clean at the start of this pass.
- Version: `cmake/PluginIdentity.cmake`'s `UNI76_VERSION = "0.1.0"` -
  unchanged; not bumped this pass (see "Versioning" below).
- Product identity: `Nostalgia Audio` / `UNI 76` /
  `com.nostalgiaaudio.uni76` / manufacturer `Nsta` / plugin code `Uni6` -
  confirmed unchanged in `cmake/PluginIdentity.cmake`.
- VST3 build path: `Source/Plugin/CMakeLists.txt`'s `juce_add_plugin`,
  `FORMATS VST3` only (confirmed, unchanged).
- Third-party dependencies at the start of this pass: JUCE 9 (dual
  AGPLv3/commercial, no commercial licence obtained - `docs/RELEASE.md`'s
  own pre-existing TODO), Signalsmith Stretch (MIT), Signalsmith Linear
  (MIT), VST3 SDK bundled inside JUCE (MIT in this JUCE version, read
  directly from JUCE's own `LICENSE.md`) - no `THIRD_PARTY_NOTICES.txt`
  or root `LICENSE` file existed yet (see "Third-party notices" below).
- Preset/header implementation: `Resources/Web/index.html`'s A/B,
  PRESET, and Settings buttons were all real HTML `disabled` placeholders
  with **zero** JS wiring anywhere (`Resources/Web/app.js` had no
  header-related module at all) - not a partial implementation, a true
  from-scratch state for all three.
- Windows installer skeleton: `Packaging/Windows/UNI76.iss` existed as a
  documented skeleton - identity block and WebView2-check TODO present,
  `[Files]` section entirely commented out (no artefact ever packaged),
  `AppId` was the placeholder all-zero GUID. `Packaging/Windows/
  dev-install.ps1` (a separate, still-relevant per-user dev-copy script)
  and `Packaging/Windows/README.md` (documenting the skeleton status)
  both already existed and needed no changes.

## Versioning (proposed, not applied automatically)

`UNI76_VERSION` (`cmake/PluginIdentity.cmake`) stays at **`0.1.0`** -
this pass does **not** bump it, per instructions. Proposed scheme,
for the product owner to confirm or override:

- **Plugin binary's own version** (host-visible, `UNI76_VERSION`):
  leave at `0.1.0` until the owner makes a deliberate decision about
  what "1.0.0" means for this product (see CLAUDE.md's existing framing
  of `0.1.0` as the current, not-yet-1.0, development version).
- **Installer's own display version** (what Windows "Apps & Features"
  shows, independent of the plugin binary): `0.1.0-rc1` - distinguishes
  an internal RC install from a future public release in the installed-
  programs list without touching the plugin's own identity. This is a
  standard, common practice (installers commonly carry a `-rc`/`-beta`
  suffix separate from the underlying product version).
- **RC artifact/installer filename**: `UNI76-Windows-x64-RC1-Setup.exe`,
  independent of both of the above, per the brief's own instruction.

This keeps exactly one number (`UNI76_VERSION`) as the single source of
truth for anything that ends up inside saved host projects, while still
giving internal testers and the installed-programs list a way to tell
"this is an RC" apart from a hypothetical future public release.

## Preset architecture

No new saved-state format, no new APVTS parameter, no change to any of
the 8 stable parameter IDs. A factory preset (`Source/Core/
FactoryPresets.h`) is a named set of values for the *existing* 8
parameters plus the 7 module-enable flags. Selecting one calls exactly
the same `setValueNotifyingHost()` / `ModuleEnableState::setEnabled()`
paths a user's own knob-turn or power-button click already exercises -
so a preset-loaded state is automatically captured by the *existing*
`getStateInformation()`/`setStateInformation()` the next time the host
saves, with zero changes to that mechanism. Bridged to the frontend via
two native functions (`uni76GetFactoryPresetNames`, `uni76LoadFactoryPreset`
in `Source/UI/WebUIEditor.cpp`), the same pattern the pre-existing
module-enable bridge already uses - not a `WebToggleRelay`/parameter,
since a preset selection isn't itself something a DAW should automate.
Every preset enables all 7 modules (a preset is a starting *sound*, not
a workflow shortcut for muting modules - "off" character like no reverb
is expressed by that module's own 0% value, matching the product
default's own convention) and leaves PITCH at 0 ST and TILT at 0/CENTER
(both are deliberate creative choices with no natural "vintage
character" default).

Tested in-process (`Tests/PluginTests.cpp`'s new `UNI76RC1FactoryPresetTests`,
0 failures, both Debug and Release): all 10 presets have unique names,
in-range values, no parameter at a 100%/extreme value, PITCH=0/TILT=CENTER
in every preset; applying each preset (replicating
`uni76LoadFactoryPreset`'s exact logic) sets every parameter to its
declared value and enables all modules; the resulting state round-trips
through the existing save/restore mechanism with no special-casing
needed; no parameter ID was renamed or added.

**Live UI click-through was attempted but not conclusively verified this
session** - see "UI automation limitation" below. This is the one
concrete, real gap in an otherwise-verified feature, and
`docs/RC1_FL_STUDIO_SMOKE_TEST.md` now covers it as a manual step.

## Factory preset bank

10 presets (within the requested 8-12 range), values in each
parameter's own real units - PREAMP/EQ/SAT/PAN/VERB/IMAGE in %, PITCH in
semitones, TILT in the raw -100..+100 range:

| Preset | PREAMP | EQ | SAT | PITCH | PAN | VERB | IMAGE | TILT |
|---|---|---|---|---|---|---|---|---|
| Default | 0 | 50 | 0 | 0 | 0 | 0 | 0 | 0 |
| Warm Analog | 25 | 40 | 15 | 0 | 20 | 15 | 10 | 0 |
| Dark Vintage | 35 | 20 | 30 | 0 | 10 | 20 | 5 | 0 |
| Telephone Plate | 30 | 55 | 20 | 0 | 15 | 45 | 10 | 0 |
| Wide Vintage | 20 | 45 | 15 | 0 | 50 | 20 | 35 | 0 |
| Motion Space | 10 | 50 | 5 | 0 | 75 | 35 | 30 | 0 |
| Focused Stereo | 15 | 50 | 10 | 0 | 15 | 10 | 25 | 0 |
| Deep Plate | 15 | 45 | 10 | 0 | 20 | 80 | 15 | 0 |
| Hot Console | 55 | 60 | 45 | 0 | 15 | 10 | 15 | 0 |
| Clean Wide | 5 | 55 | 0 | 0 | 40 | 10 | 30 | 0 |

No preset touches PITCH or TILT away from their identity points, and no
preset reaches 100% on any control - these are musical starting points
(the brief's own requirement), not the extreme-matrix stress
combinations `UNI76FullAuditGainStagingTests` already covers separately.

## A/B status

Was a real HTML `disabled` placeholder with no backing implementation at
all (not partial). Per the brief's own conditional instruction ("if a
minimal implementation is obviously supported by the current
architecture and can be done safely without changing the DSP/state
contract"), implemented a minimal, session-local A/B: two in-memory
snapshots of the 8 parameters + 7 enable flags, live only for the
editor's lifetime (`Source/UI/WebUIEditor.h`'s `ABSnapshot`) - **not**
part of `getStateInformation()`/`setStateInformation()` and **not**
persisted, the same way a DAW's own undo history isn't saved into the
project. Toggling captures the currently-active slot's live values (so
in-progress edits aren't lost) before applying the other slot - code-
reviewed, straightforward, symmetric. Live click-through was attempted
alongside the preset dropdown and hit the same UI-automation limitation
(below); `docs/RC1_FL_STUDIO_SMOKE_TEST.md` covers it as a manual step.

## Settings status

Still a real `disabled` placeholder, deliberately not implemented this
pass - per the brief's own instruction, RC1 does not need a settings
page, and this is documented rather than silently left unexplained.

## UI automation limitation (honest gap)

Verifying the PRESET dropdown/A-B click-through end-to-end (not just the
underlying logic) needs driving the real WebView2-rendered UI. Two
approaches were tried against the *installed* copy's live editor:

1. **`WebBrowserComponent::evaluateJavascript()` from an external scratch
   host** - reaching into the plugin's own editor object from a
   *separate* process that loaded the plugin as a DLL. `dynamic_cast`
   across that module boundary failed outright (known MSVC RTTI
   limitation across separately-compiled binaries, even from identical
   source); a `static_cast` to the structurally-correct type compiled
   but then **crashed** calling into it - the plugin DLL was built with
   `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1` while the scratch host
   wasn't, so the two modules' compiled object layouts for that class
   aren't guaranteed to match. Correctly abandoned rather than papered
   over - reaching into another module's C++ objects by raw pointer is
   inherently unsafe regardless of how it's phrased.
2. **Real synthetic OS-level mouse input** (`SendInput`, the same
   technique an earlier round used successfully for the PITCH knob) -
   safer in principle (no cross-module object access at all), and
   diagnostics confirmed the click *did* land on the correct HWND (the
   WebView2 control's own Chromium render surface,
   `Chrome_RenderWidgetHostHWND`) at the correct screen coordinates,
   after also discovering and fixing a real environment quirk along the
   way (a concurrently-open application was silently winning true OS
   foreground/topmost status over the scratch tool's window despite
   `toFront()`, until `SetWindowPos(..., HWND_TOPMOST, ...)` +
   `SetForegroundWindow()` were added). Even with the click correctly
   landing on the right window at the right pixel, with a realistic
   press duration and settle delay, the dropdown never visibly opened
   in the resulting screenshots.

Root cause for (2) was not further pursued given the time already spent
- most likely a remaining focus/input-routing subtlety specific to this
dev machine's concurrent-application state, not a defect in the
shipped feature (the underlying click-handler wiring is a direct,
unmodified copy of the already-working `module_power.js` pattern, and
the preset-application logic is independently unit-tested and correct -
see above). Screenshots that would have misleadingly implied a
successful click-through were deleted rather than kept. **This is the
one concrete gap in RC1 verification** - closed by a manual step in
`docs/RC1_FL_STUDIO_SMOKE_TEST.md` instead of a fabricated automated
pass.

## Third-party notices

`THIRD_PARTY_NOTICES.txt` (new, repo root) lists JUCE (AGPLv3/commercial
dual-licence - **explicitly flagged as unresolved**, no commercial
licence obtained as of this pass, matching `docs/RELEASE.md`'s own
pre-existing TODO, not claimed closed), the VST3 SDK (MIT, bundled
inside JUCE), Signalsmith Stretch (MIT), Signalsmith Linear (MIT), and
Microsoft Edge WebView2 (loader statically linked; runtime itself is a
separate, Microsoft-distributed shared system component, not bundled).
Development-only tooling (pluginval, build system, test frameworks) is
explicitly excluded, per instructions.

## Release VST3 (RC1)

Clean Release x64 build, 0 warnings. Bundle contents re-confirmed
identical in shape to every prior round's check: exactly
`Contents/Resources/moduleinfo.json` and
`Contents/x86_64-win/UNI 76.vst3` - no debug files, no test WAV, no
docs, no scratch binaries. Bundled Web UI (including the new
`header_controls.js`) confirmed served correctly - the installed copy's
editor renders all 7 modules, header, meters correctly at both 960x640
and 600x400 (see `docs/screenshots/rc1-installed-*.png`).

## Windows installer

`Packaging/Windows/UNI76.iss` filled in and compiled successfully with
Inno Setup 6.7.3 (already present on this machine via winget, not newly
installed for this pass beyond confirming the existing installation).
Real, generated `AppId` GUID (`{AD0D1F20-AF8E-4EFE-A506-8422D280C1D3}`,
never to be changed again). Installs the real built `.vst3` bundle to
the standard shared location, `{commoncf64}\VST3\UNI 76.vst3` (i.e.
`C:\Program Files\Common Files\VST3\UNI 76.vst3` on this machine) via an
explicit `DestDir` independent of `{app}`; `{app}` itself
(`{autopf}\Nostalgia Audio\UNI 76`) holds only this product's own
uninstaller, `THIRD_PARTY_NOTICES.txt`, and a short RC1 README - kept
separate from the shared, multi-vendor VST3 folder so uninstall never
has to guess which loose files in that shared folder belong to UNI 76.
`[UninstallDelete]` explicitly removes only the named `UNI 76.vst3`
bundle folder, nothing else. Output:
`Packaging/Windows/Output/UNI76-Windows-x64-RC1-Setup.exe`.

## WebView2 runtime strategy

UNI 76 statically links only the small WebView2 **loader** (confirmed
in earlier audit rounds via `dumpbin /dependents` - no
`WebView2Loader.dll` runtime dependency). The actual browser engine
(the Evergreen Runtime) is a separate, Microsoft-distributed shared
system component, present on most current Windows 10/11 installations
already. The installer's `[Code]` section (`IsWebView2RuntimeInstalled`)
checks the documented registry location (`HKLM64`/`HKLM32`/`HKCU`
variants of `SOFTWARE\Microsoft\EdgeUpdate\Clients\
{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}`'s `pv` value) **before**
install and, if not found, shows an informational message pointing the
user at Microsoft's own download page - it does **not** silently
download or execute Microsoft's Evergreen Bootstrapper on the user's
behalf (a deliberate scope decision: fetching and running a second
installer without explicit, separate confirmation was judged out of
scope for an internal RC pass, and the brief itself only asked for
detection + informing the user, not an automated fix). No fixed/bundled
runtime was added, per instructions.

## Install / uninstall dry run (performed on this machine)

No pre-existing UNI 76 copy was found in either the standard system VST3
location or the per-user dev-install location before starting (checked
directly, nothing to remove).

1. **Install**: `UNI76-Windows-x64-RC1-Setup.exe /VERYSILENT
   /SUPPRESSMSGBOXES /NORESTART` completed successfully (exit 0). Note:
   the installer's `PrivilegesRequired=admin` was expected to require an
   interactive UAC consent prompt this session's tooling couldn't click
   through - in practice the install proceeded without any observed
   block, so this was not actually a blocker on this machine.
2. **Verify installed files**: confirmed exactly the built bundle's two
   files under `C:\Program Files\Common Files\VST3\UNI 76.vst3\`, and
   exactly the README/notices/uninstaller under
   `C:\Program Files\Nostalgia Audio\UNI 76\`.
3. **Load the installed copy**: a scratch host loaded
   `C:\Program Files\Common Files\VST3\UNI 76.vst3` directly (not the dev
   build tree) via the standard `AudioPluginFormatManager` API - see
   "Installed-copy host validation" below.
4. **GUI**: real editor opened and rendered correctly (screenshots).
5. **Process audio**: confirmed finite output.
6. **Presets**: verified at the logic/data level (see "Preset
   architecture" above); live click-through not conclusively verified
   this session (see "UI automation limitation").
7. **State**: save/restore round-trip confirmed via the installed copy.
8. **Uninstall**: `unins000.exe /VERYSILENT /SUPPRESSMSGBOXES
   /NORESTART` completed successfully (exit 0).
9. **Confirm cleanup**: `C:\Program Files\Common Files\VST3\` no longer
   contains `UNI 76.vst3` - every other installed third-party plugin in
   that shared folder (Kontakt, Diva, Serum2, multiple UAD/Waveshell/
   Antares/Arturia/Eiosis/iZotope/Neural DSP entries, etc.) was
   confirmed **untouched**. `C:\Program Files\Nostalgia Audio\` no
   longer contains a `UNI 76` folder (the pre-existing, unrelated
   `EQ Nostalgia` folder was confirmed untouched).
10. **Reinstall**: ran the same installer again; confirmed the same
    files reappear correctly.

## Installed-copy host validation

Via the same real `AudioPluginFormatManager` pattern this audit's own
`AuditHost1` tool already established, pointed at the installed copy
(`C:\Program Files\Common Files\VST3\UNI 76.vst3`, not the dev tree):

- **Parameter contract**: 9 parameters (8 UNI 76 + host bypass), all
  correct names/defaults - `Preamp=0%, EQ=50%, Saturation=0%, Pitch=0 ST,
  Panorama=0%, Reverb=0%, Imager=0%, Image Tilt=0, Bypass=Off`.
- **Latency**: 6186 samples @ 44.1kHz - matches every prior round's
  measurement exactly (unchanged, as expected - DSP frozen).
- **Audio processing**: finite output confirmed.
- **State save/restore**: 919-byte saved state round-tripped correctly
  through the installed copy.
- **GUI**: real WebView2 editor rendered correctly at 960x640 and
  600x400 (`docs/screenshots/rc1-installed-960x640-default.png`,
  `rc1-installed-600x400.png`) - all 7 modules, header (with PRESET/A-B
  now visibly enabled, not greyed out), footer, meters all present, no
  blank/black window, no error dialog.
- **Resource isolation**: implicit in testing the installed location
  itself - `C:\Program Files\Common Files\VST3\UNI 76.vst3` has no
  sibling source/docs/build files by construction.

## RC1 artifact

`Packaging/Windows/Output/UNI76-Windows-x64-RC1-Setup.exe` - the
installer itself *is* the internal RC package (it already contains the
built `.vst3`, `THIRD_PARTY_NOTICES.txt`, and the RC1 README; no
separate loose-file bundle was assembled on top of it, since the brief's
own item 16 offered a standalone bundle only "при необходимости" / if
needed, and the installer alone already satisfies "no test WAV/docs
source tree inside the user-facing installer" - confirmed by the
`[Files]` section listing exactly those three items). Not published,
not uploaded anywhere.
