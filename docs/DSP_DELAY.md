# DELAY / TAPE ECHO - DSP notes

`Source/DSP/DelayProcessor.h`/`.cpp` implements the `DELAY` module - the
8th real DSP in the plugin, added 2026-09-14 alongside the existing
PREAMP/EQ/SAT/PITCH/PAN/VERB/IMAGE modules (see CLAUDE.md's "DELAY module"
entry for the project-journal account and `Source/DSP/DelayCurves.h` for
the tempo-sync math and fixed feedback-path constants this file's own
measurements reference).

A tempo-synced (always BPM-locked, never a free-running millisecond
value) echo, warm and vintage rather than clean/digital - matching the
rest of UNI 76's own aesthetic (see CLAUDE.md's "HTML/CSS/JS UI rule" and
every other module's own DSP doc for the same "vintage, not modern-clean"
brief).

## Parameters

Five new parameters (`Source/Parameters/ParameterIDs.h`), taking the
plugin from 10 to 15 public, host-automatable parameters:

| ID | Type | Range | Default |
|---|---|---|---|
| `delay` | `AudioParameterFloat` | 0..100% | 0% (identity - no audible effect) |
| `delayFeedback` | `AudioParameterFloat` | 0..95% | 30% |
| `delayDivision` | `AudioParameterChoice` | 5 choices: `1/4`, `1/8`, `1/8D`, `1/8T`, `1/16` | index 1, `"1/8"` |
| `delayStereo` | `AudioParameterBool` | MONO / STEREO | `false` (MONO) |
| `delayPingPong` | `AudioParameterBool` | OFF / ON | `false` (OFF) |

`delayDivision`/`delayStereo`/`delayPingPong` are this project's first
parameters that aren't a plain `0..100%` float (or PITCH's own discrete
int) - `AudioParameterChoice` and `AudioParameterBool` are both standard,
already-available JUCE types (no new dependency), bridged to the WebView
through JUCE's own `WebComboBoxRelay`/`WebToggleButtonRelay` +
`WebComboBoxParameterAttachment`/`WebToggleButtonParameterAttachment` -
the *same* already-vendored `Resources/Web/juce_webview.js` frontend
module the existing `WebSliderRelay` bridge uses (`getComboBoxState`/
`getToggleState` are already exported by it, unmodified) - not a second,
parallel bridge mechanism. `Source/UI/WebUIEditor.cpp`'s generic
`ParamID::all`-driven loops (preset apply, A/B snapshot capture/apply,
save/restore round-trip) needed **no code changes** for these two new
shapes: `RangedAudioParameter::getValue()`/`setValueNotifyingHost()`/
`convertTo0to1()`/`convertFrom0to1()` are the same generic interface for
every parameter type, so extending `ParamID::all` from 10 to 15 entries
and widening the fixed-size arrays that iterate over it
(`WebUIEditor.h`'s `ABSnapshot::values`, `Core/UserPresets.h`'s
`UserPresetData::values`, `Core/FactoryPresets.h`'s raw-value array in
`WebUIEditor.cpp`'s `uni76LoadFactoryPreset`) was the only change needed
in each of those call sites.

## Topology

```
DI/DRY --------------------------------------------------> MIX
INPUT
  -> delay line(s) (two independent buffers, L and R - see "Mode
     routing" below for how each mode wires them)
  -> per-pass feedback-path tone shaping:
       - soft ~100Hz highpass (prevents low-frequency hum/boom building
         up over dozens of recirculations - never touches the dry path)
       - progressive ~3.2kHz one-pole lowpass damping INSIDE the loop
         (the same "per-pass darkening" mechanism VERB's own per-line
         feedback damping uses - see docs/DSP_VERB.md) - each successive
         repeat is measurably darker than the one before it, not just
         the wet signal darker than dry at every instant
       - a tiny, fixed, bounded tanh() saturation (same shape PREAMP/
         SAT/VERB already use elsewhere in this plugin, own constants -
         see docs/DSP_SAT.md's "own constants, not a shared refactor"
         precedent)
  -> this SAME tone-shaped tap is both what the user hears (the
     "repeat") and what re-enters the buffer (scaled by FEEDBACK) for
     the next regeneration - which is exactly what makes FEEDBACK=0%
     produce one full-volume, tone-shaped repeat with no further
     regeneration, rather than a quiet one (a genuinely common but
     wrong first-attempt design: scaling the AUDIBLE tap by FEEDBACK
     too would make FEEDBACK=0% inaudible, contradicting the product
     brief's own explicit "0% = один слышимый повтор" requirement)
  -> MIX (dry*(1-mix) + wetProcessed*mix - a genuine crossfade, not an
     aux-send the way VERB's own MIX-equivalent works: the product brief
     is explicit that DELAY's MIX controls "only the Dry/Wet ratio")
  -> Output
```

**Dry is never touched** by the delay line, feedback filters, or
saturation - it is read into a local and written back scaled only by
`(1-mix)`, in the same per-sample loop iteration, the same "dry never
passes through the wet-path machinery" guarantee every other module in
this plugin already provides (VERB's own class comment documents the
identical shape). At `delay=0%` this collapses to an exact identity:
`(1-0)=1` for dry, `mix=0` for wet, so the output is bit-for-bit the
input.

## Mode routing

The class holds exactly ONE pair of `{time, feedback}` coefficients at
any instant - there is no per-channel time, which is itself the
guarantee L and R can never drift to different delay times (a literal
architectural impossibility, not just a rule that's followed).

- **MONO** - a single, self-feeding loop (`bufferL` only; `bufferR` is
  left unwritten so switching back to STEREO/PING-PONG later doesn't
  start from stale content). The wet SEND into it is the *normalised*
  mono sum (`0.5*(L+R)`, no gain increase from summing - per the product
  brief's own explicit "без увеличения громкости" requirement). Both
  output channels receive the identical wet tap - real L/R identical, not
  just similar - per the brief's own "левый и правый Wet должны быть
  идентичными" requirement. The incoming stereo image of the DRY signal
  is never touched (MONO only affects the repeats, never the whole
  output) - per "исходная стереопанорама Dry не схлопывается".
- **STEREO** - two fully independent feedback loops (`bufferL`/`bufferR`),
  each fed from its OWN input channel and its OWN feedback path, no
  cross-talk. Both loops share the one class-wide time/feedback pair, so
  L and R are always in tempo-locked lockstep by construction - the
  brief's own "L/R используют одинаковое BPM-деление... не должны
  получать случайно различающиеся времена" requirement, satisfied
  architecturally rather than by a runtime check.
- **PING PONG** - the classic *single-chain* topology (not two
  independently-triggered lines): `bufferL` is written from the mono
  send PLUS `bufferR`'s own feedback tap; `bufferR` receives ONLY
  `bufferL`'s feedback tap, with no direct input at all. This means the
  input enters the chain exactly once, on the Left side by design (a
  deliberate, documented simplification - the brief's own "первый повтор
  должен логично учитывать сторону исходного сигнала" is satisfied by a
  fixed, predictable first side rather than a per-sample panning-bias
  detector, matching how most real ping-pong hardware/pedals behave too),
  and then alternates sides every further regeneration: the first repeat
  appears on L at time T (from the direct send), the second on R at time
  2T (fed from L's own tap through R's own buffer), the third back on L
  at 3T, and so on. This is what makes a mono/centred source alternate
  cleanly L-R-L-R (the brief's own "понятное симметричное чередование, а
  не двойной одинаковый повтор" requirement) rather than double-hitting
  both sides simultaneously, which is what two independently-fed lines
  would do. No autopan/LFO anywhere in this path - the alternation is a
  direct, structural consequence of the cross-feedback routing itself,
  per the brief's own explicit "не применяй дополнительный autopan"
  instruction.

**The PING PONG / STEREO conflict rule** (product brief, section 10):
turning PING PONG on always implies STEREO; turning STEREO off (back to
MONO) while PING PONG is on must also turn PING PONG off - "MONO + PING
PONG ON" is not a representable combination. This is enforced in TWO
independent places, deliberately redundant:

1. **UI** (`Resources/Web/app.js`'s `initDelayStereoToggle()`/
   `initDelayPingPong()`) - clicking MONO while PING PONG is on also
   calls `setValue(false)` on the PING PONG toggle state (and vice
   versa), both on the message thread via the normal WebToggleButtonRelay
   `setValue()` path - never anything audio-thread-adjacent.
2. **DSP** (`DelayProcessor::process()`) - regardless of what the UI
   happens to have done (a DAW automation lane could still drive the two
   booleans independently, or a restored/hand-edited state could load an
   impossible combination), the *effective* mode is resolved fresh every
   block from both raw inputs: `effectivePingPong = pingPong && stereoBus`,
   `effectiveStereo = (stereo || pingPong) && stereoBus`. Per the
   product brief's own explicit "эффективный DSP-режим при активном Ping
   Pong должен быть Stereo" instruction, this is a pure, stateless
   per-block computation - **never** a parameter mutation from the audio
   thread, which the brief also explicitly forbids ("не изменяй
   параметры небезопасно из audio thread").

A mono host bus (`numChannels < 2`) is always processed as MONO
regardless of what either boolean holds - there is no second channel to
be stereo/ping-pong across.

## Click-free BPM/division changes

Per the product brief's own suggested approach: rather than sweeping one
read pointer's speed when the target time changes (which bends pitch,
audibly, for the duration of the sweep - a real, common defect in naive
tempo-synced delay implementations), `DelayProcessor` keeps **two
independent, constant-time read taps** ("voices") into the same buffer.
Changing division or host tempo re-times the currently-*inactive* voice
to the new target and crossfades to it linearly over
`delayVoiceCrossfadeSeconds` (50ms, `DelayCurves.h`) - the *active*
voice's own pitch is never bent (each voice holds a fixed delay time
while it's the one being heard), only which of the two fixed-time taps is
currently audible changes, gradually. A new change arriving mid-
transition is picked up on the next block once the current crossfade
settles, rather than interrupting an in-progress fade (a deliberate small
simplification - division/tempo changes are discrete, relatively rare
user/automation gestures, not an audio-rate signal).

## BPM resolution

`DelayCurves.h`'s `delayTimeSecondsForDivision (divisionIndex, hostBpm)`:

```
1/4  = 60  / BPM
1/8  = 30  / BPM
1/8D = 45  / BPM
1/8T = 20  / BPM
1/16 = 15  / BPM
```

At 120 BPM: `500ms / 250ms / 375ms / 166.667ms / 125ms` - matching the
product brief's own worked examples exactly. `hostBpm` is read once per
block in `PluginProcessor::processBlock()` via `AudioPlayHead::
getPosition()->getBpm()` - the exact same read PAN's own tempo-synced
RATE knob already established (`PanoramaCurves.h`'s "Tempo-synced motion
rate" section) - and falls back to `delayFallbackBpm` (120.0, matching
PAN's own `panRateFallbackBpm`) whenever the host doesn't report a BPM at
all, or reports a non-finite/non-positive one (some hosts report `0`
before transport ever starts). The resolved time is additionally clamped
to `[delayMinTimeSeconds, delayMaxTimeSeconds]` (5ms..4.0s) - a safety
bound against an extreme host tempo (a very slow BPM's own `1/4` note
could otherwise demand an arbitrarily large buffer); 4.0s comfortably
covers even a 20 BPM `1/4` note (3.0s) with headroom. The delay buffers
themselves are sized once, in `prepare()`, for this fixed upper bound -
`process()` never reallocates regardless of how BPM/division change at
runtime.

## FEEDBACK: character and safety

`delayFeedback` (0..95%) maps directly to the per-pass loop gain via
`delayFeedbackMaxGain = 0.95f` - **never reaches unity gain**, a hard
ceiling below 1.0 by construction, independent of anything else. Combined
with the tone-shaping above (every pass loses a little more energy to the
highpass/lowpass than a bare gain multiply would suggest), the *effective*
loop gain is always somewhat lower than the raw feedback percentage - the
same safety-margin-by-construction VERB's own per-line feedback gain
clamp already establishes for a different module's recirculating loop
(see docs/DSP_VERB.md's "RT60" section). The bounded `tanh()` saturation
on every pass is a second, independent safety mechanism: a tanh curve
can never produce a sample outside `(-1/tanh(drive), +1/tanh(drive))`
regardless of how much energy has built up internally, so even a
pathological input can't make the loop's own numbers explode - this is
deliberately the *character* (a "деликатная... очень мягкая аналоговая
сатурация") and the safety net at once, not a separate, audible hard
digital limiter layered on top (which the product brief explicitly asks
to avoid). Both the input and output boundaries of `process()` also run
the same `std::isfinite` NaN/Inf sanitisation pattern every other module
in this plugin already uses.

Behaviour at the product brief's own reference points (0%/30%/70%/95%
FEEDBACK) follows directly from the sub-unity gain + tone-shaping
combination: 0% gives exactly one repeat (see the "Topology" section
above for why the audible tap is never itself scaled by FEEDBACK); higher
settings compound progressively more (and progressively darker/warmer,
per the per-pass damping) regenerations, bounded well short of runaway
self-oscillation even at the 95% ceiling.

## Chain order / module-enable integration

DELAY is role index **7** in `Core/ChainOrder.h`/`Core/ModuleEnableState.h`
- an APPENDED role, not a renumbering of any existing role (0=preamp,
1=eq, 2=saturation, 3=pitch, 4=panorama, 5=reverb, 6=imager were all
already fixed before this round; the same "append, never renumber"
precedent `imageTilt`/`panRate`/`verbDrive` already established for
parameters). Its new **default chain position** is between PAN and VERB -
`{0, 1, 2, 3, 4, 7, 5, 6}` - independent of its role *index* being the
highest number: position and role index are unrelated concepts (see
`ChainOrder.h`'s own class comment). `numModules` moved from 7 to 8 in
both classes; `PluginProcessor::processBlock()`'s existing `switch`
dispatch (already generic over `ChainOrder::numModules` positions) just
gained one more `case 7: delayProcessor.process (...); break;` arm - no
other dispatch-loop code needed to change.

DELAY's own module-enable flag (`ModuleEnableState`'s `delayEnabled`,
index 7) defaults to **enabled** - its own `delay` (MIX) parameter
already defaults to 0%, its own true no-op/identity position, matching
every other non-EQ module's "identity default -> safe to default
enabled" reasoning (see `ModuleEnableState.h`'s own comment).

Bypassing DELAY (module disabled, or a future host-level bypass) mutes
only the wet contribution the same way every other module's bypass
smoother does - dry passes through unaffected, per the product brief's
own explicit "Bypass модуля также должен полностью исключать DELAY из
обработки" requirement.

## Backward compatibility

A project or preset saved before DELAY existed has:

- **None of the five new parameters at all** - `AudioProcessorValueTreeState`
  falls back to each parameter's own declared default (`delay=0%`,
  `delayFeedback=30%`, `delayDivision="1/8"`, `delayStereo=MONO`,
  `delayPingPong=OFF`) automatically, with no migration code needed -
  `delay=0%` alone already guarantees DELAY is completely inaudible
  regardless of what the other four happen to be, satisfying the product
  brief's own "старые проекты после обновления должны звучать так же, как
  раньше" requirement.
- **A `delayEnabled` module-enable flag that doesn't exist** -
  `ModuleEnableState`'s own pre-existing "missing property defaults to
  `true`" migration rule (`PluginProcessor::setStateInformation()`)
  applies unchanged; harmless either way since `delay=0%` makes the
  enabled state acoustically irrelevant.
- **A saved `ChainOrder` with exactly 7 tokens** (the old `numModules`),
  never mentioning role 7 at all - `ChainOrder::insertDelayIntoLegacyOrder()`
  inserts DELAY (role 7) immediately before VERB (role 5) in that saved
  order, preserving the relative order of every other module a user may
  have already custom-reordered, rather than discarding their whole
  layout for the full default (per the product brief's own explicit
  "безопасно вставляй его перед VERB, не меняя взаимный порядок
  остальных модулей" instruction). Applied in two places: the plugin's
  own `setStateInformation()` and `Core/UserPresets.h`'s
  `loadUserPreset()` (a user preset file can equally predate DELAY).
  Factory presets (`Core/FactoryPresets.h`) don't need this migration
  path at all - the struct's own `chainOrder` member has a default
  member initialiser already set to the *current* 8-role default order,
  and C++ aggregate initialisation fills any trailing member a preset
  row's brace-init list doesn't mention with that default - so none of
  the 32 existing factory preset rows needed editing to gain a valid
  8-role chain order.

## What was NOT changed

Per the product brief's own explicit scope: no existing module's DSP,
parameter ID, range, or default changed; no manual-millisecond delay-time
entry exists anywhere (BPM-sync only, five fixed divisions); no sixth
parameter was added beyond the five listed above; no tape noise, wow/
flutter, or extra pitch modulation was added to the repeats (the product
brief explicitly rejects these); MONO never touches the DRY signal's own
stereo image, only the wet repeats.

## Status

Implemented and building clean (0 warnings) in Release. Per the task's
own explicit instruction, no unit tests were written or modified and the
existing test suite was not run for this round - manual verification in
a real DAW is the user's own next step.
