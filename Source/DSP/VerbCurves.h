#pragma once

#include <algorithm>
#include <array>
#include <cmath>

/*
    Single source of truth for how the VERB / VINTAGE SPACE macro
    parameter (0..1, the `reverb` APVTS value) morphs the module's send
    amount, decay, pre-delay - see Source/DSP/VerbProcessor and
    docs/DSP_VERB.md.

    One vintage electromechanical PLATE machine at every setting - not a
    ROOM->PLATE->CHAMBER morph. The macro only ever changes send amount,
    decay time, apparent size (via decay), and pre-delay; the plate's own
    physical character (delay-line layout, diffusion, damping shape,
    analog send/return coloration) is fixed, matching a real plate's send
    level and time knobs rather than a switch between different machines.

        0%   = DRY   - bit-exact (up to float rounding) identity.
        50%  = PLATE  - classic, dense, dark studio plate.
        100% = DEEP    - longer, deeper, still a usable insert effect.
*/

namespace uni76::dsp
{
    inline float verbSmoothstep (float x) noexcept
    {
        const auto c = std::clamp (x, 0.0f, 1.0f);
        return c * c * (3.0f - 2.0f * c);
    }

    /** Piecewise-smoothstepped interpolation through 5 anchor points at
        t = 0, 0.25, 0.5, 0.75, 1.0 - each segment is independently
        smoothstepped (C0 continuous at the anchors, matching the shape
        every other module's macro curves already use for named anchor
        points - see EqCurves.h/PanoramaCurves.h). Used for every VERB
        macro curve below so each one can hit its own explicitly-specified
        target at each quarter-point without forcing a single global
        polynomial through all five. */
    inline float verbPiecewise (float t01, const std::array<float, 5>& values) noexcept
    {
        // std::clamp does not actually clamp NaN (all comparisons against
        // NaN are false, so it returns the unclamped input unchanged) -
        // an std::isfinite guard is needed *before* clamping, otherwise a
        // non-finite t01 reaches `(int) scaled` below, which is undefined
        // behaviour and can produce an out-of-range `segment` (a real
        // Debug-mode "array subscript out of range" crash was caught this
        // way - see docs/DSP_VERB.md's "A real bug found by Debug-mode
        // testing" section). Falls back to 0.0 (t=0, the DRY/identity
        // point) rather than silently picking some other value.
        const auto safeT01 = std::isfinite (t01) ? t01 : 0.0f;
        const auto t = std::clamp (safeT01, 0.0f, 1.0f);
        const auto scaled = t * 4.0f;
        const auto segment = std::min (3, (int) scaled);
        const auto local = verbSmoothstep (scaled - (float) segment);
        return values[(size_t) segment] + (values[(size_t) segment + 1] - values[(size_t) segment]) * local;
    }

    // ---- Wet send amount -----------------------------------------------------
    //
    // wet(0) = 0.0 exactly - the one hard requirement that makes DRY a
    // provable bypass (see VerbProcessor.cpp). 100% knob position is
    // explicitly NOT 100% wet - dry always remains present, matching a
    // real insert-effect plate send, not a full wet/dry replace.
    inline constexpr std::array<float, 5> verbWetAnchors { 0.0f, 0.10f, 0.225f, 0.35f, 0.475f };

    inline float verbWetGain (float t01) noexcept
    {
        return verbPiecewise (t01, verbWetAnchors);
    }

    // ---- Decay (RT60, seconds) -----------------------------------------------
    //
    // decay(0) is never audible (wet gain is exactly 0 there) but is still
    // defined smoothly down to a short, harmless value rather than being
    // left at the 25% anchor's value, so nothing downstream has to special-
    // case t=0 for numerical reasons (e.g. feedback-gain-from-RT60 division).
    // Scaled up from the raw target RT60s (0.35/0.75/1.7/2.85/4.0) - the
    // per-line feedback gain formula in VerbProcessor.cpp targets RT60
    // assuming *only* the flat gain contributes to decay, but the per-
    // line damping filter (frequency-dependent extra loss - see
    // `verbDampingHz` below) removes additional energy every pass on top
    // of that, so the *actual measured* RT60 undershoots the nominal
    // target unless compensated here. Empirically measured and tuned -
    // see docs/DSP_VERB.md's "RT60" section for the before/after numbers.
    inline constexpr std::array<float, 5> verbDecayAnchors { 0.5f, 1.1f, 2.6f, 4.3f, 6.0f };

    inline float verbDecaySeconds (float t01) noexcept
    {
        return verbPiecewise (t01, verbDecayAnchors);
    }

    // ---- Pre-delay (ms) -------------------------------------------------------
    //
    // Deliberately modest throughout - a real plate is felt as fast/
    // immediate, not a slapback delay. See docs/DSP_VERB.md's "Plate
    // physical character" section.
    inline constexpr std::array<float, 5> verbPreDelayMsAnchors { 0.0f, 2.5f, 8.0f, 14.0f, 20.0f };

    inline float verbPreDelayMs (float t01) noexcept
    {
        return verbPiecewise (t01, verbPreDelayMsAnchors);
    }

    // ---- Wet-path bass isolation (350Hz) --------------------------------------
    //
    // A cascaded (4-pole, -24dB/oct) Butterworth highpass on the wet SEND
    // path - not a brickwall FIR (no latency, no ringing) - plus a second,
    // lighter safety highpass on the wet OUTPUT (after the plate network),
    // catching any low-frequency energy the plate's own recirculation
    // might otherwise sustain (a feedback network's own resonances aren't
    // guaranteed to respect an input-side filter alone - see
    // docs/DSP_VERB.md's "350Hz wet-path isolation" section). The DRY path
    // never passes through either of these - see VerbProcessor.cpp.
    inline constexpr float verbWetSendHighpassHz   = 350.0f;
    inline constexpr float verbWetOutputHighpassHz = 350.0f;

    // ---- Plate delay-line layout -----------------------------------------------
    //
    // 12 lines (within the requested 8 minimum / 12-16 explored range) -
    // short, densely-spaced, deliberately non-commensurate lengths (no
    // small-integer ratios between any two) so no single comb frequency
    // dominates - see docs/DSP_VERB.md's "Plate modal/FDN architecture"
    // section for the measured mode-density verification. Millisecond
    // values, converted to samples at the actual sample rate in
    // VerbProcessor::prepare() - this is what keeps the plate's character
    // (not just its RT60) sample-rate-independent.
    inline constexpr int verbNumLines = 12;
    inline constexpr std::array<float, 12> verbLineLengthsMs
    {
        5.3f, 6.8f, 8.1f, 9.7f, 11.3f, 13.7f,
        16.1f, 19.3f, 22.9f, 27.1f, 31.7f, 37.3f
    };

    // Fixed diffuser (early-density) allpass chain, ahead of the FDN tank -
    // short, decreasing delay lengths, same low-Q allpass primitive
    // PanoramaProcessor's decorrelation uses (Biquad.h's makeAllpass is
    // 2nd-order/IIR; this diffuser instead uses classic short-delay
    // Schroeder-style allpass sections, appropriate for building early
    // reflection density quickly - NOT used alone as the whole reverb,
    // only as a pre-density stage ahead of the real FDN tank, which is
    // what makes this architecturally different from a "cheap Schroeder"
    // reverb - see docs/DSP_VERB.md).
    inline constexpr int verbNumDiffusers = 4;
    inline constexpr std::array<float, 4> verbDiffuserLengthsMs { 3.1f, 2.3f, 1.7f, 1.1f };
    inline constexpr float verbDiffuserGain = 0.6f;

    // ---- Frequency-dependent damping (HF decays faster than mid) --------------
    //
    // One-pole lowpass inside each delay line's feedback path (Moorer/Jot-
    // style damping, not a static output filter - this is what makes the
    // *tail itself* progressively darker over time, distinct from
    // verbReturnBandwidthHz below, which limits the wet signal's overall
    // top end at every instant). Measured/tuned via docs/DSP_VERB.md's
    // "frequency-dependent decay" section.
    // Raised from an initial 4200Hz during tuning: even a one-pole
    // lowpass's small per-pass insertion loss *below* its own cutoff
    // (not just above it) compounds hugely over the hundreds of feedback
    // passes a multi-second RT60 needs (~-0.24dB/pass at 1kHz against a
    // 4200Hz cutoff, over ~300 passes, compounds to roughly -70dB on its
    // own - silently capping 1kHz's own decay far below its nominal
    // target, which is what the first measurement round caught). Raising
    // it too far (9000Hz was tried) backfired differently: with per-pass
    // damping loss that small, the shortest line's *total* loop gain
    // (flat gain from the RT60 formula, ~0.994 for the 5.3ms line at a
    // 6s target, times a near-unity damping response) sits close enough
    // to 1.0 to measurably distort the result (THD roughly doubled,
    // steady-state level became erratic) - a real stability-margin
    // symptom, not a measurement artifact. 6000Hz is the settled middle
    // ground: still well above the audible band's own damping-loss
    // compounding problem, with enough headroom below `verbLineFeedbackGainMax`
    // (see below) that no line's loop gain gets close to the boundary.
    inline constexpr float verbDampingHz = 6000.0f;

    // Hard safety ceiling on any single line's per-pass feedback gain,
    // independent of the RT60-derived formula (VerbProcessor.cpp) -
    // guarantees the FDN can never be pushed into a technically-unstable
    // (gain >= 1) or borderline-ringy configuration by any combination of
    // decay-anchor tuning and line length, present or future.
    inline constexpr float verbLineFeedbackGainMax = 0.985f;

    // ---- Analog send/return coloration -----------------------------------------
    //
    // Base (DRIVE=0%) values are deliberately tiny - texture, not a second
    // SAT module. Same bounded per-half-gain tanh() shape PREAMP/SAT use,
    // own (much smaller) constants - wet-path only, dry is never touched.
    // See docs/DSP_VERB.md's "Analog send electronics"/"Analog return
    // stage" sections for measured THD at the base values.
    // Reduced during tuning - measured wet-path THD at the original,
    // larger values reached ~5.9% on a sustained full-level tone at
    // 100% wet (edge of "texture, not distortion"); these land closer to
    // 2% under the same worst-case test - see docs/DSP_VERB.md's
    // "Analog nonlinearity" section for the measured H2/H3/THD table.
    inline constexpr float verbSendDriveGainBase  = 0.18f;
    inline constexpr float verbSendAsymmetryBase  = 0.02f;
    inline constexpr float verbReturnDriveGainBase = 0.12f;
    inline constexpr float verbReturnAsymmetryBase = 0.02f;

    // ---- DRIVE (nested knob, live-testing follow-up round) --------------------
    //
    // The base values above were fixed for every previous round of this
    // module's development - DRIVE (`ParamID::verbDrive`) now scales both
    // the send and return stages together, from that same tiny-texture
    // resting point up to a genuinely hot, audibly-driven plate -
    // referencing a real reference plugin's (Vynl Audio Voyager-Verb) own
    // nested-knob DRIVE control (see the module's own class comment /
    // docs/DSP_VERB.md's "Drive (nested knob)" section). DRIVE=0% must
    // reproduce the base values exactly (verbSmoothstep(0)==0), so every
    // existing preset/session that never touches the new knob sounds
    // identical to before this round shipped.
    //
    // Asymmetry ceilings (0.30/0.24) sit in the same range PREAMP/SAT's
    // own asymmetry maxima do (0.32/0.28) - real, audible even-harmonic
    // character at full DRIVE, not a token gesture.
    //
    // Drive-gain ceilings: an early version of this curve used 1.1/0.75 -
    // reasonable-looking numbers that turned out to reproduce exactly the
    // "no audible effect" bug this session's own PREAMP/SAT drive-curve
    // fix diagnosed and corrected. At this module's own -18dBFS reference
    // test level (amplitude ~0.126), a gain of 1.1 only reaches a tanh
    // argument of ~0.14 - still deep in tanh's near-linear region (linear
    // to within a fraction of a percent below ~0.2) - so "full DRIVE"
    // measured barely more THD than DRIVE=0% (1.92% -> 2.16%, caught by a
    // dedicated regression test before this shipped, not discovered
    // later). PREAMP's own drive-gain ceiling for comparison is 10.0
    // (PreampCurves.h) - VERB's DRIVE is deliberately less extreme than a
    // dedicated saturator (this is still a reverb's send/return
    // coloration, not PREAMP's own job), but large enough to push the
    // tanh well into its curved region at the reference level: gain 7.0
    // on a 0.126-amplitude signal reaches tanh(0.88), clearly compressed.
    inline constexpr float verbSendDriveGainMax   = 7.0f;
    inline constexpr float verbSendAsymmetryMax   = 0.30f;
    inline constexpr float verbReturnDriveGainMax = 5.0f;
    inline constexpr float verbReturnAsymmetryMax = 0.24f;

    inline float verbDriveLerp (float base, float max, float driveNormalised01) noexcept
    {
        const auto t = std::clamp (driveNormalised01, 0.0f, 1.0f);
        return base + (max - base) * verbSmoothstep (t);
    }

    inline float verbSendDriveGain (float driveNormalised01) noexcept
    {
        return verbDriveLerp (verbSendDriveGainBase, verbSendDriveGainMax, driveNormalised01);
    }

    inline float verbSendAsymmetry (float driveNormalised01) noexcept
    {
        return verbDriveLerp (verbSendAsymmetryBase, verbSendAsymmetryMax, driveNormalised01);
    }

    inline float verbReturnDriveGain (float driveNormalised01) noexcept
    {
        return verbDriveLerp (verbReturnDriveGainBase, verbReturnDriveGainMax, driveNormalised01);
    }

    inline float verbReturnAsymmetry (float driveNormalised01) noexcept
    {
        return verbDriveLerp (verbReturnAsymmetryBase, verbReturnAsymmetryMax, driveNormalised01);
    }

    // Overall wet bandwidth ceiling (return stage) - soft, single-pole
    // rolloff, not brickwall. Sits well above the damping filter's own
    // cutoff so the two effects are both audible/measurable independently
    // (a static ceiling on top-end brightness vs. a progressively-
    // darkening tail).
    inline constexpr float verbReturnBandwidthHz = 9500.0f;

    // ---- Smoothing -------------------------------------------------------------
    inline constexpr double verbSmoothingSeconds = 0.03;
}
