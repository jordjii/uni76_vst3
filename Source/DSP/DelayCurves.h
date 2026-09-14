#pragma once

#include <algorithm>
#include <array>
#include <cmath>

/*
    Single source of truth for DELAY's tempo-sync math and fixed feedback-
    path constants - see Source/DSP/DelayProcessor and docs/DSP_DELAY.md.

    Same BPM-sync pattern PAN's own RATE knob already established
    (Source/DSP/PanoramaCurves.h's "Tempo-synced motion rate" section) -
    a fixed table of musical note divisions resolved against the host's
    current tempo (read once per block from AudioPlayHead, see
    PluginProcessor.cpp), not a free-running millisecond value.
*/

namespace uni76::dsp
{
    struct DelayDivision
    {
        const char* label;
        double beatsPerNote; // quarter-note beats, independent of time signature
    };

    // Exactly the 5 divisions the product brief asks for - no manual
    // millisecond entry anywhere in this module.
    inline constexpr std::array<DelayDivision, 5> delayDivisions
    { {
        { "1/4",   1.0 },
        { "1/8",   0.5 },
        { "1/8D",  0.75 },
        { "1/8T",  1.0 / 3.0 },
        { "1/16",  0.25 },
    } };

    inline constexpr int delayDivisionCount = (int) delayDivisions.size();
    inline constexpr int delayDefaultDivisionIndex = 1; // "1/8"

    // A non-finite, zero, or absurd host tempo (some hosts report 0 before
    // transport ever starts, or don't report tempo at all) falls back to
    // this rather than reaching a division-by-zero or NaN below - same
    // reasoning and same fallback value as PanoramaCurves.h's own
    // panRateFallbackBpm.
    inline constexpr double delayFallbackBpm = 120.0;

    // Safety bounds on the resolved delay TIME itself (not on BPM) - a
    // very slow host tempo combined with 1/4 note could otherwise demand
    // an arbitrarily large buffer. 20 BPM's own 1/4 note is 3.0s; 4.0s
    // gives headroom above that while staying a small, fixed allocation
    // (sized once in prepare(), never resized in process()).
    inline constexpr double delayMinTimeSeconds = 0.005; // 5ms floor - avoids a degenerate near-zero read/write collision
    inline constexpr double delayMaxTimeSeconds = 4.0;

    /** Resolves a division index + host tempo to a real delay time in
        seconds, clamped to the safety bounds above. The only call site
        for this is DelayProcessor.cpp; VerbCurves.h/PanoramaCurves.h's own
        parallel BPM-sync helpers are deliberately not shared code (no
        header dependency between unrelated modules), matching this
        project's own "own constants, not a shared refactor" precedent
        (see docs/DSP_SAT.md). */
    inline double delayTimeSecondsForDivision (int divisionIndex, double hostBpm) noexcept
    {
        const auto safeBpm = (std::isfinite (hostBpm) && hostBpm > 1.0) ? hostBpm : delayFallbackBpm;
        const auto clampedIndex = std::clamp (divisionIndex, 0, delayDivisionCount - 1);
        const auto seconds = (60.0 / safeBpm) * delayDivisions[(size_t) clampedIndex].beatsPerNote;
        return std::clamp (seconds, delayMinTimeSeconds, delayMaxTimeSeconds);
    }

    // ---- Click-free time changes: two crossfaded "voices" ---------------------
    //
    // Per the product brief's own suggestion: rather than sweeping one
    // read pointer's speed when the target time changes (which bends
    // pitch, audibly, for the duration of the sweep), DelayProcessor keeps
    // TWO independent read taps ("voices") into the same delay buffer,
    // each holding a CONSTANT delay time while active. Changing division/
    // tempo re-times the currently-inactive voice to the new target and
    // crossfades to it over this many seconds - the active voice's own
    // pitch is never bent, only which of two constant-time taps is
    // audible changes, and gradually.
    inline constexpr double delayVoiceCrossfadeSeconds = 0.05;

    // ---- Feedback path (character + safety) ------------------------------------
    //
    // FEEDBACK (`delayFeedback`, 0..95%) maps directly to the per-pass
    // loop gain, capped below 1.0 by construction (95% max, not 100%) -
    // the hard ceiling the product brief asks for ("защита от runaway
    // feedback... ограничение feedback gain ниже единицы"). Combined with
    // the tone-shaping below (every pass loses a little more energy to
    // the lowpass/highpass than a bare gain multiply would suggest), the
    // *effective* loop gain is always somewhat lower than the raw
    // feedback percentage - by design, the same safety margin
    // VERB's own per-line feedback gain clamp already establishes for a
    // different module's own recirculating loop.
    inline constexpr float delayFeedbackMaxGain = 0.95f;

    // Soft low-cut inside the feedback path only (never the dry path) -
    // "чтобы feedback не гудел": without this, bass content recirculating
    // dozens of times would build into a low, boomy hum well before the
    // higher frequencies became inaudible. Well above true sub-bass, low
    // enough to leave real body/warmth in the repeats.
    inline constexpr float delayFeedbackHighpassHz = 100.0f;

    // Progressive high-frequency softening inside the feedback path - a
    // one-pole lowpass INSIDE the loop (not a static output filter),
    // exactly the "damping" mechanism VERB's own per-line feedback path
    // uses (Source/DSP/VerbCurves.h's verbDampingHz) - what makes each
    // successive repeat measurably darker than the one before it, not
    // just the wet signal darker than the dry one at every instant.
    inline constexpr float delayFeedbackDampingHz = 3200.0f;

    // Very gentle, fixed (not macro-controlled) analog-style saturation on
    // every pass through the feedback loop - "деликатная... очень мягкая
    // аналоговая сатурация", and, combined with the sub-unity feedback
    // gain above, part of the runaway-feedback safety net (a bounded
    // tanh() can never produce a sample outside (-1/tanh, +1/tanh) times
    // its own drive gain, regardless of how much energy has built up) -
    // reusing the same bounded per-half-gain waveshaper shape PREAMP/SAT/
    // VERB already use elsewhere in this plugin, deliberately tiny
    // constants of its own (not shared - see docs/DSP_SAT.md's "own
    // constants, not a shared refactor" precedent).
    inline constexpr float delayFeedbackDriveGain  = 0.35f;
    inline constexpr float delayFeedbackAsymmetry  = 0.05f;
}
