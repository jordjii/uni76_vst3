#pragma once

#include <algorithm>
#include <cmath>

/*
    Single source of truth for how the EQ / PHONE TONE macro parameter
    (0..1, the `eq` APVTS value) morphs the EQ module's filter network -
    see Source/DSP/EqProcessor and docs/DSP_EQ.md.

    Replaced (UX polish pass, live-testing feedback) from the earlier
    DARK/PHONE/AIR five-stage morph with a always-on, steep two-cut
    band-pass "telephone" filter, modelled directly on a real reference
    plugin's (FabFilter Pro-Q3) cut-band settings: a highpass and a
    lowpass, each 48dB/octave (4 cascaded 2nd-order stages - see
    EqProcessor.cpp), whose corner frequencies sweep together as one
    macro moves, always keeping the same ratio between them (the same
    "bandwidth in octaves"), while each cut's own Q stays fixed:

        t=0.0   (displayed -50%, knob fully left)  -> HP  72Hz / LP  1132Hz
        t=0.5   (displayed   0%, knob centred)      -> HP 461Hz / LP  7288Hz
        t=1.0   (displayed +50%, knob fully right)  -> HP 748Hz / LP 11855Hz

    The APVTS `eq` parameter itself is unchanged - still a plain 0..100%
    AudioParameterFloat defaulting to 50% (see ParameterLayout.cpp and
    CLAUDE.md's "8 immutable public parameters" - this is a DSP-behaviour
    and UI-label change only, not a parameter-contract change, so it
    needed no schema bump). The knob's displayed value is remapped to
    -50%..+50% purely in the frontend (see app.js) so the panel reads as
    a symmetric, centred control even though the underlying value is
    still the same 0..1 range every other macro curve in this codebase
    uses.

    HP/LP corner frequencies interpolate geometrically (log-linear, reads
    as an even sweep - same convention every other module's macro curves
    already use); each cut's own Q is identical at all three anchors
    (0.765 for the HP, 0.676 for the LP) so it needs no interpolation at
    all, though eqMorph() still linearly interpolates it for robustness
    against a future anchor retune that gives Q its own trajectory.
*/

namespace uni76::dsp
{
    struct EqCutParams
    {
        float hpHz, hpQ;
        float lpHz, lpQ;
    };

    // ---- Named anchors -----------------------------------------------------
    //
    // Same LP/HP ratio (~15.7-15.9x, ~3.97-3.99 octaves of passband) at
    // every anchor - the macro sweeps a fixed-width "phone band" window up
    // and down in frequency rather than opening/closing it.
    inline constexpr EqCutParams eqAnchorLeft   { 72.0f,  0.765f, 1132.0f,  0.676f };
    inline constexpr EqCutParams eqAnchorCenter { 461.0f, 0.765f, 7288.0f,  0.676f };
    inline constexpr EqCutParams eqAnchorRight  { 748.0f, 0.765f, 11855.0f, 0.676f };

    inline float eqSmoothstep (float x) noexcept
    {
        const auto c = std::clamp (x, 0.0f, 1.0f);
        return c * c * (3.0f - 2.0f * c);
    }

    inline float eqLerp (float a, float b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    /** Geometric (log-linear) interpolation - reads as an even sweep. */
    inline float eqLerpHz (float a, float b, float t) noexcept
    {
        return std::exp (eqLerp (std::log (a), std::log (b), t));
    }

    inline EqCutParams eqMorph (const EqCutParams& a, const EqCutParams& b, float localT) noexcept
    {
        const auto t = eqSmoothstep (localT);
        return EqCutParams {
            eqLerpHz (a.hpHz, b.hpHz, t), eqLerp (a.hpQ, b.hpQ, t),
            eqLerpHz (a.lpHz, b.lpHz, t), eqLerp (a.lpQ, b.lpQ, t),
        };
    }

    /** The single entry point: EQ/PHONE TONE (0..1) -> the two cuts'
        frequency/Q, continuous and slope-continuous through t=0.5. */
    inline EqCutParams eqParamsAt (float eqNormalised01) noexcept
    {
        const auto t = std::clamp (eqNormalised01, 0.0f, 1.0f);

        if (t <= 0.5f)
            return eqMorph (eqAnchorLeft, eqAnchorCenter, t / 0.5f);

        return eqMorph (eqAnchorCenter, eqAnchorRight, (t - 0.5f) / 0.5f);
    }

    // ---- Smoothing -------------------------------------------------------------
    inline constexpr double eqParameterSmoothingSeconds = 0.035;
    inline constexpr double eqBypassSmoothingSeconds     = 0.02;
}
