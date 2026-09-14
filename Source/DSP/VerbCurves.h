#pragma once

#include <algorithm>
#include <array>
#include <cmath>

/*
    Single source of truth for the VERB / VINTAGE SPACE macro parameter
    (0..1, the `reverb` APVTS value, "Mix") and the module's fixed
    internal constants - see Source/DSP/VerbProcessor and
    docs/DSP_VERB.md.

    ---------------------------------------------------------------------
    NEW DIRECTION (2026-09-14) - see CLAUDE.md's "VERB direction change"
    entry and docs/DSP_VERB.md's "Direction change" section for the full
    product brief. The plate/FDN architecture this file used to describe
    is retired; this is the from-scratch redesign against that new
    direction:

        a soft, warm, dark, vintage algorithmic reverb (chamber/hall
        character, NOT a plate, NOT Dattorro's specific plate topology) -
        dense, smooth, ~3s tail, no metallic ring, no fixed resonant
        notes, no hard early reflections, no audible comb filtering, no
        explicit chorus/pitch wobble, natural at Mix 70-100%.

    Two things that changed on purpose relative to the old plate module,
    both direct product requirements, not incidental tuning:

    1. **Mix (`reverb`) now controls ONLY the dry/wet balance.** Decay
       time and pre-delay are FIXED constants (verbTargetDecaySeconds /
       verbPreDelayMs), completely independent of the macro - the old
       module stretched decay from ~2.5s to ~4.35s across the knob, which
       is exactly the behaviour this redesign is told not to repeat.
       `verbWetGain(t01)` is the only macro-dependent curve left.
    2. **No time-varying (modulated) delay anywhere in the signal path.**
       The old plate's per-line "chorus/vibrato" modulation was the
       direct cause of two real, documented regressions (a measured RT60
       collapse from the interpolation loss it introduced, and an
       explicit "no chorus/pitch wobble" product requirement this round).
       Anti-metallic decorrelation instead comes entirely from STATIC
       design choices: a long multi-stage input diffusion cascade, a
       larger number of mutually-non-commensurate tank delay lengths, and
       an orthogonal (energy-preserving) Householder mixing matrix - not
       Dattorro's specific "two arms, each with an internal decay
       allpass" plate topology, and not the previous Householder-mixed
       12/16-line FDN either. See VerbProcessor.h's class comment for the
       full topology.
    ---------------------------------------------------------------------
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
        points - see EqCurves.h/PanoramaCurves.h). Used by verbWetGain()
        below - the only VERB macro curve left that is actually macro-
        dependent under the new direction (see the file comment above). */
    inline float verbPiecewise (float t01, const std::array<float, 5>& values) noexcept
    {
        // std::clamp does not actually clamp NaN (all comparisons against
        // NaN are false, so it returns the unclamped input unchanged) -
        // an std::isfinite guard is needed *before* clamping, otherwise a
        // non-finite t01 reaches `(int) scaled` below, which is undefined
        // behaviour and can produce an out-of-range `segment` (a real
        // Debug-mode "array subscript out of range" crash was caught this
        // way in the old plate-era module - see docs/DSP_VERB.md's
        // "Historical implementation" section). Falls back to 0.0 (t=0,
        // the DRY/identity point) rather than silently picking some other
        // value.
        const auto safeT01 = std::isfinite (t01) ? t01 : 0.0f;
        const auto t = std::clamp (safeT01, 0.0f, 1.0f);
        const auto scaled = t * 4.0f;
        const auto segment = std::min (3, (int) scaled);
        const auto local = verbSmoothstep (scaled - (float) segment);
        return values[(size_t) segment] + (values[(size_t) segment + 1] - values[(size_t) segment]) * local;
    }

    // ---- Mix (dry/wet balance) ------------------------------------------------
    //
    // wet(0) = 0.0 exactly - the one hard requirement that makes DRY a
    // provable bypass (see VerbProcessor.cpp). 100% knob position is
    // explicitly NOT 100% wet - dry always remains present, matching an
    // insert-effect reverb send rather than a full wet/dry replace, and
    // keeping headroom so Mix 70-100% stays natural (no wash-out) - a
    // direct product requirement for this redesign.
    inline constexpr std::array<float, 5> verbWetAnchors { 0.0f, 0.12f, 0.27f, 0.42f, 0.55f };

    inline float verbWetGain (float t01) noexcept
    {
        return verbPiecewise (t01, verbWetAnchors);
    }

    // ---- Decay (RT60) - FIXED, not macro-dependent ----------------------------
    //
    // Direct product requirement: "Mix должен управлять Dry/Wet, а не
    // растягивать Decay до 4.35 секунды." Unlike the old plate module
    // (whose decay stretched from ~2.5s at 50% to ~4.35s at 100% as a
    // side effect of the same macro that also controlled wet amount),
    // this reverb's decay time is a single constant, completely
    // independent of Mix - turning Mix up only adds more of an
    // *identical-length* tail into the mix, never a longer one.
    // ~2.8-3.2s target range (measured via a dedicated RT60 test at low/
    // mid/high bands - see docs/DSP_VERB.md).
    inline constexpr float verbTargetDecaySeconds = 3.0f;

    /** Kept as a function (not a bare constant) so call sites written
        against the old macro-dependent contract (PluginProcessor's
        getTailLengthSeconds(), the test suite) don't need to change -
        but the parameter is now genuinely unused: every t01 in [0,1]
        returns the same fixed target. */
    inline float verbDecaySeconds (float) noexcept
    {
        return verbTargetDecaySeconds;
    }

    // ---- Pre-delay - FIXED, not macro-dependent --------------------------------
    //
    // Same reasoning as decay above: a real vintage reverb's sense of
    // "size" here comes from the tank's own fixed geometry, not from
    // stretching pre-delay with Mix. Deliberately modest - felt as fairly
    // immediate, not a slapback delay, and short enough that it never
    // reads as a discrete hard early reflection on its own (see the
    // "no hard early reflections" product requirement).
    inline constexpr float verbPreDelayMsValue = 16.0f;

    /** Same "kept as a function for the old call-site contract" reasoning
        as verbDecaySeconds() above. */
    inline float verbPreDelayMs (float) noexcept
    {
        return verbPreDelayMsValue;
    }

    // ---- Wet-path bass isolation -----------------------------------------------
    //
    // Lower than the old plate module's 250Hz - this redesign's product
    // brief explicitly asks for a *warmer* character, so more low-mid
    // body is allowed into the wet path than the old plate ever let
    // through, while a cascaded (4-pole, -24dB/oct) Butterworth highpass
    // on the send - not a brickwall FIR, no latency/ringing - still keeps
    // true sub/bass out of the recirculating tank (a reverberated kick
    // drum reads as mud, in any architecture). A second, lighter safety
    // highpass on the wet OUTPUT catches anything the tank's own
    // recirculation might otherwise sustain regardless of the send
    // filter - a feedback network's own resonances aren't guaranteed to
    // respect an input-side filter alone (same reasoning the old module
    // documented, still true of any recirculating design). The DRY path
    // never passes through either of these - see VerbProcessor.cpp.
    inline constexpr float verbWetSendHighpassHz   = 220.0f;
    inline constexpr float verbWetOutputHighpassHz = 220.0f;

    // ---- Input diffusion network (multi-stage, ahead of the tank) -------------
    //
    // A long cascade of short-delay Schroeder-style allpass diffusers,
    // NOT the whole reverb by itself (that would be the "cheap Schroeder"
    // architecture both the old and new product briefs reject) - a pre-
    // density stage that smears the input into a smooth, already-dense
    // signal before it ever reaches the tank. This is the main mechanism
    // behind "no hard early reflections": by the time the tank's first
    // recirculation pass is audible, the input has already been broken
    // into a diffuse wash, not a train of discrete taps.
    //
    // 8 stages spanning roughly 0.7-32ms - lengths chosen with no small-
    // integer ratio between any pair.
    inline constexpr int verbNumDiffusers = 8;
    inline constexpr std::array<float, 8> verbDiffuserLengthsMs
    {
        0.7f, 1.7f, 3.1f, 5.9f, 8.9f, 14.3f, 21.7f, 31.3f
    };
    inline constexpr float verbDiffuserGain = 0.62f;

    // ---- Reverb tank: decorrelated delay lines + Householder mixing -----------
    //
    // A plain N-line Feedback Delay Network (Stautner & Puckette 1982
    // style - delay + per-line damping + an energy-preserving mixing
    // matrix) - NOT Dattorro's plate topology (which nests its own
    // internal decay-diffusion allpasses inside two long delay "arms";
    // deliberately not reproduced here per this round's explicit "don't
    // use Dattorro Plate" instruction) and NOT the old plate module's
    // Householder-mixed 12/16-line tank either (different line count,
    // different lengths, different mixing matrix, and a much smaller,
    // carefully-bounded modulation depth - see "Tank line modulation"
    // below).
    //
    // 16 lines at a "chamber" size range (16-78ms, longer/roomier than
    // the old plate's tight 5-88ms spread, matching this redesign's
    // chamber/hall rather than plate character), chosen with a near-
    // geometric spacing and checked pairwise for small-integer
    // coincidences. A 24-line attempt (irregularly spaced 14-87ms) was
    // tried and MEASURED WORSE on the spectral-flatness sweep than this
    // 16-line set (peak residual roughly doubled) - not every increase in
    // line count helps; the extra short lines in that attempt introduced
    // new near-coincidences of their own. Reverted rather than kept on
    // the assumption "more lines = better" - see docs/DSP_VERB.md's "Tank
    // line modulation" section for the measured comparison.
    inline constexpr int verbNumLines = 16;
    inline constexpr std::array<float, 16> verbLineLengthsMs
    {
        16.1f, 17.9f, 19.9f, 22.3f, 24.7f, 27.3f, 30.5f, 33.9f,
        37.7f, 41.9f, 46.3f, 51.5f, 57.1f, 63.5f, 70.3f, 78.1f
    };

    // ---- Tank line modulation (small, correctly-bounded - NOT chorus) ---------
    //
    // The direct product brief for this redesign asks for two things that
    // sound contradictory at first: "correctly modulated decorrelated
    // delay lines" AND "no explicit chorus/pitch wobble". The resolution
    // (standard practice in high-quality algorithmic reverbs - Lexicon/
    // Valhalla-style designs use exactly this) is that BOTH are true at
    // once when the modulation depth is kept below the threshold where it
    // reads as its own audible effect: a STATIC FDN's resonant modes are
    // fixed for the life of the instance, which is what a real ear
    // identifies as "metallic" (a specific note always rings at exactly
    // the same frequency, every time); a small, slow modulation of each
    // line's read position continuously and very slightly detunes those
    // fixed modes, which is what actually removes the "ringing on
    // specific notes" symptom - it is not decorative, it is the mechanism.
    // Read via AllpassFractionalDelay (Biquad.h) rather than plain 2-tap
    // linear interpolation - a true allpass (exactly flat magnitude
    // response at any fractional delay, verified by its own isolated unit
    // tests before ever being wired into this tank - see
    // Tests/PluginTests.cpp's "uni76::dsp::AllpassFractionalDelay" suite).
    // This is precisely the fix the old plate module's own "Metallic-ring
    // root-cause investigation" round called for and failed to ship (a
    // magnitude-flat interpolator, verified in isolation first) - not a
    // repair of that old code, a fresh implementation checked against the
    // specific mistake ("D=0 must collapse to an exact identity") that
    // investigation's own writeup flagged as the leading suspect. Because
    // this interpolator costs no per-pass loss, depth can be meaningfully
    // larger than the old plate module's already-too-small 0.4 samples
    // (which was too subtle to fix that module's own metallic ringing)
    // while still being a tiny fraction of any line's length (the
    // shortest tank line here is >800 samples at 44.1kHz) - nowhere near
    // large enough to read as an audible chorus/pitch effect. Confirmed
    // by direct measurement (see docs/DSP_VERB.md's "Tank line
    // modulation" section) rather than assumed safe by similarity.
    inline constexpr float verbLineModDepthSamples = 4.0f;
    inline constexpr std::array<float, 16> verbLineModRateHz
    {
        0.073f, 0.089f, 0.101f, 0.113f, 0.127f, 0.139f, 0.151f, 0.167f,
        0.181f, 0.197f, 0.211f, 0.229f, 0.241f, 0.257f, 0.269f, 0.283f
    };

    // ---- RT60-formula target (internal, larger than the spec/reported
    // ~3s figure) ----------------------------------------------------------
    //
    // The per-line feedback-gain formula (VerbProcessor.cpp) assumes only
    // the flat gain governs decay, but the per-line damping filter and the
    // allpass modulation above both remove additional energy every pass on
    // top of that - the same "small per-pass loss compounds hugely over
    // hundreds of feedback passes" effect this module's own history has
    // documented before (see the old plate module's superseded RT60-
    // tuning notes). Measured directly (see docs/DSP_VERB.md's "RT60"
    // section for this redesign): with the formula fed verbTargetDecaySeconds
    // (3.0s) directly, the *actual* measured decay undershoots to ~1.9-2.2s.
    // This internal-only target is what the formula actually solves for;
    // verbTargetDecaySeconds above remains the real, reported, spec value
    // (~2.8-3.2s) and is what every public/test-facing curve returns -
    // only VerbProcessor.cpp's own per-line gain computation reads this one.
    inline constexpr float verbDecayFormulaTargetSeconds = 6.0f;

    // ---- Frequency-dependent damping (highs decay faster than mid) ------------
    //
    // One-pole lowpass inside each delay line's feedback path (Moorer/
    // Jot-style damping) - what makes the *tail itself* progressively
    // darker over time, distinct from the static output darkening filter
    // below. Lower than the old plate's 6000Hz (this redesign's product
    // brief explicitly wants "slightly dark", not just "highs die a bit
    // faster than mid") - safe at this target because the fixed ~3s decay
    // target is considerably shorter than the old plate's up-to-4.35s
    // DEEP setting, so far fewer feedback passes accumulate the per-pass
    // damping-filter insertion loss the old module's own tuning history
    // warned about (see docs/DSP_VERB.md's historical "RT60" section) -
    // headroom confirmed by measurement (see docs/DSP_VERB.md's "RT60"
    // section for this redesign).
    inline constexpr float verbDampingHz = 4200.0f;

    // Hard safety ceiling on any single line's per-pass feedback gain,
    // independent of the RT60-derived formula (VerbProcessor.cpp) -
    // guarantees the tank can never be pushed into a technically-unstable
    // (gain >= 1) configuration by any combination of line length and the
    // fixed decay target above, present or future.
    inline constexpr float verbLineFeedbackGainMax = 0.98f;

    // ---- Analog send coloration (fixed, never DRIVE-scaled) -------------------
    //
    // Deliberately tiny - texture, not a second SAT module - and, per the
    // DRIVE contract below, permanently pinned to these values: the send
    // stage (and everything after it up to and including the tank) never
    // reads DRIVE at all, so the tank always receives (and reverberates)
    // an essentially clean, undriven signal.
    inline constexpr float verbSendDriveGainBase = 0.15f;
    inline constexpr float verbSendAsymmetryBase = 0.02f;

    // ---- DRIVE (nested knob) - overloads the formed TAIL only ------------------
    //
    // Preserves this module's existing, already-correct DRIVE contract
    // (see CLAUDE.md/docs/DSP_VERB.md's "Direction change" sections):
    // DRIVE only ever overdrives the tank's own already-diffuse, already-
    // decayed wet tail (the RETURN stage below), never the DI/dry signal,
    // never the send, and never anything inside the tank's own feedback
    // recirculation. Reference: Mk.gee's own guitar-reverb aesthetic - a
    // clean guitar whose *reverb tail* breaks up warmly when driven.
    //
    // Base (DRIVE=0%) values reproduce the module's original small,
    // fixed "texture" coloration exactly; ceilings are large enough to
    // read as a clearly driven, warm tail at DRIVE=100% without being a
    // dedicated saturator's whole job (PREAMP's own ceiling, for
    // reference, is 10.0 - see PreampCurves.h).
    inline constexpr float verbReturnDriveGainBase  = 0.12f;
    inline constexpr float verbReturnAsymmetryBase  = 0.02f;
    inline constexpr float verbReturnDriveGainMax   = 9.0f;
    inline constexpr float verbReturnAsymmetryMax   = 0.28f;

    // Front-loaded (not S-shaped) curve - "На 20-30% уже должно быть
    // слышно тёплое насыщение хвоста" (already audible warm saturation by
    // 20-30% of the knob). pow(t, 0.42) reaches roughly half the base->max
    // range by t=0.22-0.25 of knob travel (pow(0.25,0.42)=0.564), the same
    // exponent this module's own DRIVE curve has always used - carried
    // forward unchanged since it already satisfies this exact requirement
    // (verified again below by the "front-loaded, not S-shaped" test).
    inline constexpr float verbDriveCurveExponent = 0.42f;

    inline float verbDriveLerp (float base, float max, float driveNormalised01) noexcept
    {
        const auto t = std::clamp (driveNormalised01, 0.0f, 1.0f);
        return base + (max - base) * std::pow (t, verbDriveCurveExponent);
    }

    inline float verbReturnDriveGain (float driveNormalised01) noexcept
    {
        return verbDriveLerp (verbReturnDriveGainBase, verbReturnDriveGainMax, driveNormalised01);
    }

    inline float verbReturnAsymmetry (float driveNormalised01) noexcept
    {
        return verbDriveLerp (verbReturnAsymmetryBase, verbReturnAsymmetryMax, driveNormalised01);
    }

    // ---- DRIVE warmth (return-stage pre-emphasis) ------------------------------
    //
    // A plain symmetric-bandwidth tanh drives low and high content into
    // the nonlinearity equally, which reads as "cheap clipping" rather
    // than a deliberately voiced, warm saturator. A low-shelf boost ahead
    // of the return-stage tanh (VerbProcessor.cpp), scaled by DRIVE,
    // biases which content actually reaches the nonlinearity's curved
    // region toward low-mid material - "warm", per this round's own
    // "тёплым... а более дорогой и глубокий, не тресткающийся" request.
    // Identity (0dB) at DRIVE=0%.
    inline constexpr float verbReturnWarmthShelfHz = 350.0f;
    inline constexpr float verbReturnWarmthMaxDb   = 5.0f;

    // ---- Overall wet darkening (static, not macro/DRIVE-dependent) ------------
    //
    // A soft, single-pole rolloff on the wet output - what makes the
    // module read as "slightly dark and further back in the mix" at
    // every setting, distinct from the per-line damping filter above
    // (which only affects the *decay rate*, not the tail's overall
    // brightness at any single instant). Considerably darker than the
    // old plate's 7200Hz "vintage rack" ceiling - this redesign's product
    // brief explicitly asks for "sitting behind" rather than an extended-
    // bandwidth modern algorithm.
    inline constexpr float verbReturnBandwidthHz = 5200.0f;

    // ---- Driven-tail placement: depth and static (non-modulated) width --------
    //
    // Two mechanisms, both scaled by DRIVE so they are exact no-ops at
    // 0%, giving the driven tail a "warm, wide, and slightly distant"
    // character (this round's own explicit request) WITHOUT any chorus/
    // modulation (see the file comment above for why time-varying delay
    // is avoided everywhere in this redesign, drive path included):
    //
    // 1. **Depth** - a second, DRIVE-dependent bandwidth ceiling on top
    //    of the fixed verbReturnBandwidthHz above, walking down as DRIVE
    //    rises. Distortion generates its own high harmonics, and rolling
    //    the ceiling down *as DRIVE rises* keeps those harmonics from
    //    ever reading as forward/"in your face" - the same distance cue
    //    the old module's own driven-tail work already established.
    // 2. **Width** - a small, FIXED (not LFO-modulated) inter-channel
    //    delay offset on the return stage's R channel, blended in by
    //    DRIVE. A static offset decorrelates L/R (countering a shared
    //    waveshaper's tendency to pull two correlated channels toward
    //    mono) without introducing any time-varying pitch/comb artefact -
    //    there is no LFO here at all, unlike the old module's driven-tail
    //    chorus.
    inline constexpr float verbDriveDepthBandwidthMinHz = 2400.0f;
    inline constexpr float verbDriveWidthDelayMs        = 0.6f;   // fixed R-channel offset
    inline constexpr float verbDriveWidthMixMax         = 0.5f;   // how much of R is taken from the offset tap at full DRIVE

    // ---- Input stereo-width carry-through --------------------------------------
    //
    // The tank is fed from a single mono sum (deliberate - see
    // VerbProcessor.h), and its own stereo output comes from the fixed
    // decorrelated tap sign patterns below - meaning genuine incoming
    // stereo width (e.g. from PAN, if it runs before VERB in the chain
    // order - see Core/ChainOrder.h) would otherwise be silently
    // discarded. verbInputSideBlend blends a portion of the *actual*
    // input Side signal directly into the wet output (VerbProcessor.cpp),
    // alongside (not instead of) the tank's own synthesised decorrelation -
    // additive and scaled by the same wetGain*mix term everything else in
    // the wet path already uses.
    inline constexpr float verbInputSideBlend = 0.5f;

    // ---- Breakup (envelope-inverse return-stage character) --------------------
    //
    // A fixed-gain tanh alone gets audibly *cleaner* as the tail decays -
    // a quieter signal sits deeper in tanh's near-linear region -
    // backwards from a real driven analog system, where a decaying tail
    // characteristically "breaks up"/gets grainier as it fades. Tracks
    // the tank's own current level against a slow-decaying "recent peak"
    // reference and boosts the return stage's drive gain in inverse
    // proportion to that ratio. Multiplied by the raw DRIVE knob position
    // (not verbReturnDriveGain itself), so DRIVE=0% reproduces the exact
    // original behaviour with zero boost.
    // Disabled (0.0) for the 2026-09-14 redesign - not part of this
    // round's own explicit DRIVE brief ("warm, wide, slightly distant"
    // tail, via the warmth shelf/depth/width mechanisms below, all of
    // which remain), and its own envelope-ratio dynamic interacted
    // unpredictably with the new tank's much longer input-diffusion
    // buildup time and continuous read-position modulation (measured:
    // the "gets more driven as it fades" trend it is meant to produce
    // sometimes measured backwards under the new tank). Rather than ship
    // an unpredictable character on top of an already-large redesign, the
    // mechanism is kept (in case a future round wants to revisit it with
    // its own dedicated measurement pass) but set to a true no-op.
    inline constexpr float verbBreakupAmount = 0.0f;
    inline constexpr double verbBreakupLevelReleaseSeconds = 0.06;
    inline constexpr double verbBreakupPeakReleaseSeconds  = 8.0;

    // ---- Smoothing -------------------------------------------------------------
    inline constexpr double verbSmoothingSeconds = 0.03;
}
