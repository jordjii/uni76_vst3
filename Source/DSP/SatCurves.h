#pragma once

#include <algorithm>
#include <cmath>

/*
    Single source of truth for how the SATURATION / HEAT macro parameter
    (0..1) maps onto every drive-dependent shape inside
    Source/DSP/SatProcessor. See docs/DSP_SAT.md for the measured curve
    values and the reasoning behind them, and for how this deliberately
    differs from PreampCurves.h's PREAMP model (a static per-sample
    transformer waveshaper) - SAT is an energy-dependent
    saturation/compression stage: a fixed frequency tilt (protects bass,
    pushes highs harder into the nonlinearity) surrounds an
    envelope-driven dynamic gain stage and a bounded per-half-gain
    waveshaper.
*/

namespace uni76::dsp
{
    inline float satClamp01 (float t) noexcept
    {
        return std::clamp (t, 0.0f, 1.0f);
    }

    // ---- Nonlinear stage drive -------------------------------------------
    //
    // Same *safe* bounded per-half-gain structure PREAMP's calibration
    // pass settled on (each half independently tanh()-bounded - see
    // docs/DSP_PREAMP.md's "What changed in the nonlinear model") but with
    // SAT's own constants: a higher ceiling gain than PREAMP, since the
    // product brief explicitly allows SAT to be more aggressive, backed by
    // the frequency tilt and the dynamic compression stage keeping the
    // result musical rather than harsh.
    inline constexpr float satDriveGainMin = 0.08f;
    inline constexpr float satDriveGainMax = 12.0f;
    // Same fix, same reason, as PreampCurves.h's own exponent (see the
    // long comment there): 1.2 back-loaded the curve so the whole lower
    // half of HEAT sat below tanh()'s linear region and did nothing
    // audible. 0.6 front-loads it into a continuous progression. Note
    // satCompressionStrength() below deliberately shares this exponent,
    // so the dynamic-gain stage still grows in step with the waveshaper
    // - the crest-factor regression documented there stays fixed.
    inline constexpr float satDriveShapeExponent = 0.6f;

    inline float satDriveGainLinear (float heatNormalised01) noexcept
    {
        const auto t  = satClamp01 (heatNormalised01);
        const auto tt = std::pow (t, satDriveShapeExponent);
        return satDriveGainMin * std::pow (satDriveGainMax / satDriveGainMin, tt);
    }

    // Fractional gain reduction on the waveshaper's negative half only -
    // bounded by construction, never the unbounded additive term the
    // original PREAMP model got wrong.
    //
    // Raised from 0.20 to 0.28 (live-testing follow-up, consistent with
    // the same real-analog-saturator/tube-preamp research that raised
    // PreampCurves.h's own asymmetry - see that file's comment and
    // docs/DSP_SAT.md's "Tube-character follow-up" section) - kept a
    // little more conservative than PREAMP's own 0.32, since SAT already
    // carries more total character from its own dynamic compression
    // stage on top of this waveshaper.
    inline constexpr float satAsymmetryMax = 0.28f;

    inline float satAsymmetryAmount (float heatNormalised01) noexcept
    {
        return satAsymmetryMax * satClamp01 (heatNormalised01);
    }

    // ---- Energy-dependent dynamic gain (the "memory"/compression part) --
    //
    // gainReduction = 1 / (1 + compressionStrength(t) * envelope)
    //
    // Bounded in (0, 1] for any non-negative envelope - a soft downward
    // "glue" compression ahead of the waveshaper, distinct from PREAMP's
    // purely static per-sample model. envelope is the fast-attack/
    // slow-release follower tracking the (tilted) signal's rectified
    // level - see EnvelopeFollower in Biquad.h.
    inline constexpr float satCompressionStrengthMax = 2.2f;
    // Attack is instant by construction (EnvelopeFollower peak-holds - see
    // Biquad.h), not a smoothed few-ms coefficient - see that class's
    // comment for why. Only release needs a time constant.
    inline constexpr float satEnvelopeReleaseMs = 90.0f;

    inline float satCompressionStrength (float heatNormalised01) noexcept
    {
        // Same shaping exponent as the drive curve, so the envelope-driven
        // gain reduction and the waveshaper's own peak compression grow
        // together rather than the (linear) compression outpacing the
        // (back-loaded) waveshaper early - a mismatch there was measured
        // to *raise* crest factor through the 25-50% range before it fell
        // back down at 75-100%, the opposite of "smoothly decreases".
        const auto t = satClamp01 (heatNormalised01);
        return satCompressionStrengthMax * std::pow (t, satDriveShapeExponent);
    }

    // ---- Frequency tilt (pre-emphasis / de-emphasis pair) -----------------
    //
    // A low-shelf cut + high-shelf boost ahead of the nonlinearity (and
    // their exact algebraic inverses after it) - protects the fundamental
    // of low-end content from the same aggressive nonlinear treatment as
    // the mids (satisfying "bass stays controlled"), and pushes highs
    // harder into the waveshaper so they come out relatively compressed/
    // rounded after the matching de-emphasis cut (satisfying "highs get
    // softer at high HEAT" without a fixed, level-independent low-pass).
    // Both shelf gains scale with t (0 at HEAT=0), so the character grows
    // gradually with drive rather than snapping to a fixed EQ shape the
    // moment HEAT leaves zero. At HEAT=0 both shelves are exactly 0dB
    // (identity), and pre/de-emphasis are exact algebraic inverses of each
    // other at every t, so there is no coloration added at 0% regardless
    // of how non-identity the waveshaper is elsewhere in the chain.
    inline constexpr float satLowShelfFreqHz = 150.0f;
    inline constexpr float satLowShelfMaxDb = -4.0f;

    inline constexpr float satHighShelfFreqHz = 3800.0f;
    inline constexpr float satHighShelfMaxDb = 5.0f;

    inline float satLowShelfGainDb (float heatNormalised01) noexcept
    {
        return satLowShelfMaxDb * satClamp01 (heatNormalised01);
    }

    inline float satHighShelfGainDb (float heatNormalised01) noexcept
    {
        return satHighShelfMaxDb * satClamp01 (heatNormalised01);
    }

    // ---- Output compensation ---------------------------------------------
    // Retuned alongside the drive curve above (same reasoning as
    // PreampCurves.h's own trim): -18dB was flattening nearly all of
    // HEAT's level growth, which - combined with the back-loaded drive
    // curve - is why the control measured and sounded like it did
    // nothing. SAT's own dynamic-gain stage already provides real,
    // program-dependent level control, so the static trim only needs to
    // keep the top of the range usable, not normalise the whole sweep.
    inline constexpr float satOutputTrimMaxDb = -8.0f;
    inline constexpr float satOutputTrimExponent = 2.0f;

    inline float satOutputCompensationDb (float heatNormalised01) noexcept
    {
        const auto t = satClamp01 (heatNormalised01);
        return satOutputTrimMaxDb * std::pow (t, satOutputTrimExponent);
    }

    // ---- Smoothing / bypass -----------------------------------------------
    inline constexpr double satDriveSmoothingSeconds  = 0.025;
    inline constexpr double satBypassSmoothingSeconds = 0.02;

    // ---- DC protection (fixed, not drive-dependent) ------------------------
    inline constexpr float satDcBlockerHz = 5.0f;
}
